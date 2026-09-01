/*
 * kmp_donated_threads.cpp -- OpenMP workers obtained from donated threads
 */

//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// omp.h first with __KMP_IMP empty, as kmp_stub.cpp does: this file *defines*
// kmp_donate_thread(), so it wants the plain declaration, not the dllimport
// one.
#define __KMP_IMP
#include "omp.h"

#include "kmp.h"
#include "kmp_i18n.h"

#include "kmp_donated_threads.h"

#include <errno.h>

#if KMP_USE_DONATED_THREADS

// Refused in CMake as well; repeated here for a build that defines the macros
// by hand. __kmp_stats_thread_ptr is per-thread state, so a donated thread
// would carry the runtime's copy of it back into the application, pointing into
// a kmp_stats_list that teardown frees.
#if KMP_STATS_ENABLED
#error Donated threads and stats gathering cannot be enabled together
#endif

#include <pthread.h>
#include <sys/time.h>
#include <time.h>

// ---------------------------------------------------------------------------
// The clock
//
// __kmp_donated_initialize() bounds its wait in wall-clock time and takes that
// wait with pthread_cond_timedwait(), which is given an absolute deadline. The
// two therefore have to be read from one clock: a deadline measured against any
// other is not a deadline at all.
//
// Which rules out KMP_NOW(), the runtime's usual answer. On Unix x86 and x86-64
// that is the TSC and not a wall clock (see kmp.h), so a deadline built from it
// lands a few days after the epoch -- always in the past, which would return
// every wait immediately and turn the slicing below into a spin -- while the
// budget would be a count of ticks, and so a different length of time on every
// part. The one clock pthread_cond_timedwait() can be given here is the
// realtime one: pthread_condattr_setclock() exists on neither Darwin nor z/OS.
// It can step, and the slicing is what bounds what that costs.
// ---------------------------------------------------------------------------

static kmp_uint64 __kmp_donated_now_nsec(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (kmp_uint64)KMP_NSEC_PER_SEC * (kmp_uint64)tv.tv_sec +
         (kmp_uint64)KMP_NSEC_PER_USEC * (kmp_uint64)tv.tv_usec;
}

// A slice of zero would make that wait a spin, which is the one value of the
// tunable the code below cannot cap its way out of.
KMP_BUILD_ASSERT(KMP_DONATED_WAIT_SLICE_MSEC > 0);

// ---------------------------------------------------------------------------
// The table
//
// All .bss, with no initialization step: a thread may donate itself before the
// runtime's constructor has run. An unclaimed slot is never read, and each
// slot's mutex and condvar are created by the donor that claims it -- all-zeros
// is not a valid pthread_mutex_t on Darwin.
//
// Two lock levels, and nothing ever holds one while taking the other:
//
//   __kmp_donated_lock -- the table: the claimed count, the spent flag, dt_arg,
//       dt_thread, and every dt_state transition but the two into GONE (those
//       are under dt_lock), and via __kmp_donated_cond the arrival of a
//       donation. Innermost lock in the runtime: nothing is taken while holding
//       it, stdio included, so every trace sits after the unlock.
//   dt_lock, one per slot -- that donated thread's rendezvous with exactly one
//       other thread, so a wakeup disturbs no other parked donor.
//
// Spending the table needs both, hence the split in two: discard the parked
// donations under the table lock, wake their donors after.
//
// Three roles meet at a slot, never the same thread: the donor giving itself
// away, the acquirer wanting a worker, and the releaser tearing the runtime
// down. Each entry point below names the one it runs on.
// ---------------------------------------------------------------------------

// A slot's whole life. Which of the three paths it took is readable from the
// states it passed through, so no flag sits beside them:
//
//   the donation is used
//     EMPTY --donate--> PARKED --acquire--> ASSIGNED --routine returns--> GONE
//                                    (dt_routine and dt_arg           (dt_ret
//                                     are valid)                       is set)
//
//   never needed, and the donor is still there to tell
//     PARKED --discarded by a teardown or a fork parent--> RELEASED
//            --the woken donor runs nothing and leaves--> GONE
//
//   never needed, and there is no donor left to tell
//     PARKED --discarded in a forked child--> GONE
//
// GONE always means: no donor is coming back to this slot. RELEASED is the one
// state with something outstanding -- a donor discarded but not yet woken --
// which is why a forked child skips it. Only a PARKED slot is handed out and
// nothing returns to PARKED, so a donation is used at most once. EMPTY has to
// be zero: the table is .bss and an unclaimed slot has no mutex yet.
enum {
  KMP_DT_EMPTY = 0, // never claimed; dt_lock does not exist yet
  KMP_DT_PARKED, // a donor is waiting in kmp_donate_thread()
  KMP_DT_ASSIGNED, // handed out as a worker, in __kmp_launch_worker()
  KMP_DT_RELEASED, // discarded unused, and the donor has yet to notice
  KMP_DT_GONE // the donor has left; the thread is the application's again
};

typedef struct kmp_donated_thread {
  // Created by the donor before the slot is published, never destroyed.
  pthread_mutex_t dt_lock;
  pthread_cond_t dt_cond;
  // One of KMP_DT_* above, each transition made under whichever lock gives it
  // exclusion.
  std::atomic<kmp_int32> dt_state;
  // dt_lock. What the parked donor waits on -- not KMP_DT_ASSIGNED, which the
  // acquirer publishes under the table lock, before the routine is written.
  void *(*dt_routine)(void *);
  // The kmp_info_t * this worker was obtained for. Table lock, because it is
  // also the key the releaser scans by.
  void *dt_arg;
  void *dt_ret; // dt_lock. Set by the donated thread on return
  pthread_t dt_thread; // table lock. The donated thread's real pthread_self()
} kmp_donated_thread_t;
static kmp_donated_thread_t __kmp_donated_threads[KMP_MAX_DONATED_THREADS];

// Slots ever claimed, hence donations ever arrived. Only increases -- a slot is
// never released -- so the claimed slots are exactly [0, this): it both
// allocates the next one and bounds every scan below.
static int __kmp_donated_nth = 0;

static pthread_mutex_t __kmp_donated_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t __kmp_donated_cond = PTHREAD_COND_INITIALIZER;

// No donation can be used again, and no later one will be taken. Never cleared,
// not even per generation: the donated threads have gone back to the
// application.
static int __kmp_donated_disabled = FALSE;

// Only the first generation ever gets donors: once it tears down -- an ordinary
// shutdown, a pause(hard), a fork -- the donated threads go back to the
// application and __kmp_donated_disabled is set for good.
//
// Counting generations is not what refuses those later donors; disabled is. It
// is what makes the limit be settled *again*. A teardown does not lower
// __kmp_max_nth, and the next generation re-parses the environment and gets the
// wide declared value back, so without the count __kmp_donated_initialize()
// would trust the answer the first generation settled: the runtime would form a
// wide team and ask for donors that no longer exist. Re-settling finds nothing
// parked and lands on 1, which is how a later generation serializes.
//
// Hence two stamps -- the generation, bumped once per serial initialization,
// and the generation whose limit is settled -- rather than a flag beside a
// counter. A flag would have to be cleared at every boundary, and a straggler
// from the generation just ended could set it again in that gap. A stamp is
// never cleared; a stale one just carries a generation that stops matching.
//
// Read without the table lock, on every fork, so they carry their own ordering:
// the settled stamp publishes the __kmp_max_nth settled with it. Zero means "no
// generation yet" and "nothing settled", which keeps both in .bss with the
// table, for donors arriving before any constructor.
static std::atomic<kmp_uint32> __kmp_donated_generation{0};
static std::atomic<kmp_uint32> __kmp_donated_limit_settled{0};

// The two questions asked of those stamps. Both read the settled stamp *before*
// the generation: in that order, an interleaving with a generation bump leaves
// the two disagreeing, which is the conservative answer -- not settled, take
// the lock and look again.

// Has this generation's limit been settled -- so that a donor arriving now is
// too late for the pool, and a second settle has nothing left to decide? True
// once the stamp has caught up to the generation. Both are zero before the
// first generation, which is the "nothing settled yet" the gen != 0 rules out,
// and what lets a donation arrive before serial initialization.
static int __kmp_donated_limit_settled_now(void) {
  kmp_uint32 settled = KMP_ATOMIC_LD_ACQ(&__kmp_donated_limit_settled);
  kmp_uint32 gen = KMP_ATOMIC_LD_ACQ(&__kmp_donated_generation);
  return settled == gen && gen != 0;
}

// Is the settle this call is partway through no longer wanted?
//
// Two root threads can be settling at once: __kmp_donated_initialize() runs
// before __kmp_forkjoin_lock, so two first forks are never serialized against
// each other, and the wait below drops the table lock every slice. Whichever
// settles first wins, and the other must stop here rather than finish -- the
// donors it can still see are just the ones the winner has not claimed yet.
//
// Or this call's generation ended while it slept, and the `wanted` it holds was
// read from a runtime that no longer exists.
static int __kmp_donated_limit_settle_moot(kmp_uint32 my_gen) {
  kmp_uint32 settled = KMP_ATOMIC_LD_ACQ(&__kmp_donated_limit_settled);
  kmp_uint32 gen = KMP_ATOMIC_LD_ACQ(&__kmp_donated_generation);
  // The generation moved on, or -- the same test as above -- it is settled.
  return gen != my_gen || (settled == gen && gen != 0);
}

// Donors parked and not yet handed out. Caller holds __kmp_donated_lock.
static int __kmp_donated_available(void) {
  int n = 0;
  for (int i = 0; i < __kmp_donated_nth; ++i)
    if (KMP_ATOMIC_LD_ACQ(&__kmp_donated_threads[i].dt_state) == KMP_DT_PARKED)
      ++n;
  return n;
}

// Step one of spending the table: no donation is usable again, and the donors
// still parked are discarded -- so a following generation sizes itself from an
// empty table and serializes. Slots already handed out need nothing; each one's
// own donor carries it to GONE.
//
// Given an array, PARKED --> RELEASED and the indices are collected for step
// two to wake once the table lock is dropped. Given NULL, no donor will ever
// wake, so PARKED --> GONE. Returns how many were discarded.
//
// Caller holds __kmp_donated_lock. Idempotent, so its three callers --
// teardown, fork, and a donated thread's own way out -- need not know who got
// there first.
static int __kmp_donated_spend_locked(int *to_wake) {
  __kmp_donated_disabled = TRUE;
  kmp_int32 discarded_state = to_wake != NULL ? KMP_DT_RELEASED : KMP_DT_GONE;
  int n = 0;
  for (int i = 0; i < __kmp_donated_nth; ++i) {
    if (KMP_ATOMIC_LD_ACQ(&__kmp_donated_threads[i].dt_state) ==
        KMP_DT_PARKED) {
      KMP_ATOMIC_ST_REL(&__kmp_donated_threads[i].dt_state, discarded_state);
      if (to_wake != NULL)
        to_wake[n] = i;
      ++n;
    }
  }
  // Under the lock, so a sizing wait cannot miss it between test and wait.
  pthread_cond_broadcast(&__kmp_donated_cond);
  return n;
}

// Step two: wake each donor step one discarded, so it leaves
// kmp_donate_thread() instead of waiting for work that can no longer come.
// Caller holds no lock, which is the point. Nothing is published here -- step
// one already wrote the RELEASED these donors are waiting to see -- so each
// dt_lock is taken only so that the broadcast cannot be missed.
static void __kmp_donated_wake_discarded(const int *to_wake, int n) {
  for (int k = 0; k < n; ++k) {
    kmp_donated_thread_t *dt = &__kmp_donated_threads[to_wake[k]];
    pthread_mutex_lock(&dt->dt_lock);
    pthread_cond_broadcast(&dt->dt_cond);
    pthread_mutex_unlock(&dt->dt_lock);
  }
}

// Both steps, for the callers that hold no lock to begin with.
static void __kmp_donated_spend(void) {
  int to_wake[KMP_MAX_DONATED_THREADS];
  pthread_mutex_lock(&__kmp_donated_lock);
  int n = __kmp_donated_spend_locked(to_wake);
  pthread_mutex_unlock(&__kmp_donated_lock);
  __kmp_donated_wake_discarded(to_wake, n);
}

// ---------------------------------------------------------------------------
// The application-facing entry point. Runs on the donor, and does not return
// until shutdown: __kmp_launch_worker() runs on this thread, in this frame.
// Claims a slot, parks on it, serves as a worker, and gives the thread back.
//
// No runtime calls before the routine is entered: a donor may arrive before the
// runtime has initialized, so there is no catalog and every refusal returns a
// bare errno. KA_TRACE is the exception, but it takes stdio's locks, so traces
// go after the unlock on values snapshotted before it.
// ---------------------------------------------------------------------------

int kmp_donate_thread(void) {
  // Grab the table lock, so no other donor claims a slot and nobody reads the
  // count or this slot's state while they are being set up.
  pthread_mutex_lock(&__kmp_donated_lock);

  // Refuse this donor: donations are off for good (a forked child, a hard
  // pause), or the limit is already settled and this thread arrives too late
  // to join the pool.
  int disabled = __kmp_donated_disabled;
  if (disabled || __kmp_donated_limit_settled_now()) {
    pthread_mutex_unlock(&__kmp_donated_lock);
    KA_TRACE(10,
             ("kmp_donate_thread: refused with EAGAIN: %s\n",
              disabled ? "donations already spent" : "limit already settled"));
    return EAGAIN;
  }
  // Donations may arrive before the limit is settled, so nothing bounds them
  // but the table's capacity.
  int i = __kmp_donated_nth;
  if (i == KMP_MAX_DONATED_THREADS) {
    pthread_mutex_unlock(&__kmp_donated_lock);
    KA_TRACE(10, ("kmp_donate_thread: refused with ENOMEM: table full at %d "
                  "donations\n",
                  KMP_MAX_DONATED_THREADS));
    return ENOMEM;
  }

  // Init the non-zero fields; on failure the donation is refused and the thread
  // goes back to the application.
  kmp_donated_thread_t *dt = &__kmp_donated_threads[i];
  int status = pthread_mutex_init(&dt->dt_lock, NULL);
  if (status != 0) {
    pthread_mutex_unlock(&__kmp_donated_lock);
    KA_TRACE(10, ("kmp_donate_thread: slot %d not claimed: "
                  "pthread_mutex_init failed with %d\n",
                  i, status));
    return status;
  }
  status = pthread_cond_init(&dt->dt_cond, NULL);
  if (status != 0) {
    pthread_mutex_destroy(&dt->dt_lock);
    pthread_mutex_unlock(&__kmp_donated_lock);
    KA_TRACE(10, ("kmp_donate_thread: slot %d not claimed: pthread_cond_init "
                  "failed with %d\n",
                  i, status));
    return status;
  }
  dt->dt_thread = pthread_self();

  // This thread is parked (EMPTY --> PARKED), and bumping the __kmp_donated_nth
  // count makes its entry valid to read.
  KMP_ATOMIC_ST_REL(&dt->dt_state, KMP_DT_PARKED);
  __kmp_donated_nth = i + 1;
  pthread_cond_broadcast(&__kmp_donated_cond); // a sizing wait may be counting
  pthread_mutex_unlock(&__kmp_donated_lock);

  KA_TRACE(10, ("kmp_donate_thread: slot %d published; parking for work\n", i));

  // Now wait to be assigned, meaning: given a routine to execute. Park on this
  // slot's lock, not the table's, so a parked donor obstructs nothing.
  pthread_mutex_lock(&dt->dt_lock);
  while (dt->dt_routine == NULL &&
         KMP_ATOMIC_LD_ACQ(&dt->dt_state) != KMP_DT_RELEASED)
    pthread_cond_wait(&dt->dt_cond, &dt->dt_lock);
  void *(*routine)(void *) = dt->dt_routine;
  void *arg = dt->dt_arg; // ordered by the dt_routine read; see the field
  // Refused instead of assigned: RELEASED --> GONE, in this same critical
  // section, so nobody sees a slot whose donor has left still on its way out.
  if (routine == NULL)
    KMP_ATOMIC_ST_REL(&dt->dt_state, KMP_DT_GONE);
  pthread_mutex_unlock(&dt->dt_lock);

  // Refused, so back to the application. EAGAIN as for a donation refused up
  // front: 0 would claim a generation this thread never served.
  if (routine == NULL) {
    KA_TRACE(10,
             ("kmp_donate_thread: donated thread %d released unused; thread "
              "going back to the application\n",
              i));
    return EAGAIN;
  }

  KA_TRACE(10, ("kmp_donate_thread: donated thread %d entering routine for th "
                "%p\n",
                i, arg));

  // A pthread running the OpenMP code cannot die without taking everything
  // down, but a donated thread eventually returns to the application, so some
  // of its thread-private state has to come back to what it was before the
  // thread became an OpenMP thread. Save it here. Affinity mask, stats pointer
  // and cancel state are deliberately not among them -- new-design-record.md
  // §11 gives the argument, including what dropping the FP half would cost.
  int saved_gtid = __kmp_gtid_get_specific();
#ifdef KMP_TDATA_GTID
  int saved_tdata_gtid = __kmp_gtid;
#endif
#if KMP_ARCH_X86 || KMP_ARCH_X86_64
  kmp_int16 saved_x87_cw;
  kmp_uint32 saved_mxcsr;
  __kmp_store_x87_fpu_control_word(&saved_x87_cw);
  __kmp_store_mxcsr(&saved_mxcsr);
#endif

  // Call the routine, holding neither lock, for the life of the runtime. Keep
  // its return value for the releaser.
  void *ret = routine(arg);

  // Restore the saved state, before the GONE publish below and not after: past
  // it the releaser may reach __kmp_cleanup(), and __kmp_internal_end() clears
  // __kmp_init_gtid, after which __kmp_gtid_set_specific() is a no-op.
#if KMP_ARCH_X86 || KMP_ARCH_X86_64
  __kmp_load_x87_fpu_control_word(&saved_x87_cw);
  __kmp_load_mxcsr(&saved_mxcsr);
#endif
#ifdef KMP_TDATA_GTID
  __kmp_gtid = saved_tdata_gtid;
#endif
  // -1, not KMP_GTID_DNE: the setter stores gtid + 1, so -1 is what puts back
  // the NULL that stops the key's destructor running on this thread at all.
  __kmp_gtid_set_specific(saved_gtid == KMP_GTID_DNE ? -1 : saved_gtid);

  KA_TRACE(10, ("kmp_donate_thread: donated thread %d returned %p; donations "
                "now disabled, thread going back to the application\n",
                i, ret));

  // Publish the return value for __kmp_donated_release_thread() to retrieve,
  // and ASSIGNED --> GONE. Broadcast, because a slot can briefly have two
  // waiters: if the runtime tore down before this thread was scheduled, the
  // releaser can be waiting here while this thread is still parked above.
  pthread_mutex_lock(&dt->dt_lock);
  dt->dt_ret = ret;
  KMP_ATOMIC_ST_REL(&dt->dt_state, KMP_DT_GONE);
  pthread_cond_broadcast(&dt->dt_cond);
  pthread_mutex_unlock(&dt->dt_lock);

  // One donated thread has gone back to the application, so the donations that
  // were never acquired have to be released too. A backstop, not the mechanism:
  // __kmp_donated_disable() has normally done this already, and better ordered.
  // It stays for the teardown routes that set g_done and bail without reaching
  // it -- a root still active in a parallel region, and the signal handler.
  __kmp_donated_spend();

  return 0;
}

// ---------------------------------------------------------------------------
// Internal entry points, in the order a program reaches them. None run on a
// donated thread; each states its caller, and the header gives the call chain.
// ---------------------------------------------------------------------------

// Runs on the thread performing serial initialization, under __kmp_initz_lock,
// which already makes this a single decision point. The only writer of the
// generation stamp, so the increment needs no lock of its own, and acq_rel
// orders it after the __kmp_max_nth that environment parsing restored just
// above -- the limit this generation will be settled against.
void __kmp_donated_new_generation(void) {
  KMP_ATOMIC_INC(&__kmp_donated_generation);
  KA_TRACE(10, ("__kmp_donated_new_generation: generation %u, limit to be "
                "settled again\n",
                (unsigned)KMP_ATOMIC_LD_RLX(&__kmp_donated_generation)));
}

// Runs on the acquirer before it takes __kmp_forkjoin_lock: the thread about to
// form the first team of this generation. Settles that generation's limit once,
// waiting out KMP_DONATED_INIT_BUDGET_MSEC for the donations
// KMP_DEVICE_THREAD_LIMIT declared, and lowering __kmp_max_nth when they do not
// arrive -- which is what makes width a property of configuration rather than
// of thread scheduling.
void __kmp_donated_initialize(int nthreads_requested) {
  // A region wanting one thread needs no worker, and must not settle anything,
  // or a program whose first region is serial would settle without ever
  // waiting.
  if (nthreads_requested <= 1) {
    KA_TRACE(20, ("__kmp_donated_initialize: %d requested, nothing to settle\n",
                  nthreads_requested));
    return;
  }

  // Early exit tested outside of the lock, in case all is already good.
  if (__kmp_donated_limit_settled_now())
    return;

  pthread_mutex_lock(&__kmp_donated_lock);

  // Retest early exit inside the lock, in case another thread did it before
  // this thread got the lock.
  if (__kmp_donated_limit_settled_now()) {
    pthread_mutex_unlock(&__kmp_donated_lock);
    KA_TRACE(20, ("__kmp_donated_initialize: another thread settled the limit "
                  "first\n"));
    return;
  }

  // Determine how many donor threads we want. Ideally, a number was provided,
  // using the KMP_DEVICE_THREAD_LIMIT environment variable (value - 1 as the
  // limit includes the root thread). If not, we will use the number of donor
  // threads currently parked in the table, waiting for none. We also record our
  // current generation, so we can detect if the generation has changed while we
  // are waiting for donors to arrive.
  int declared = __kmp_max_nth_specified;
  int wanted = declared ? __kmp_max_nth - 1 : 0;
  int got = 0;
  kmp_uint32 my_gen = KMP_ATOMIC_LD_ACQ(&__kmp_donated_generation);
  KMP_DEBUG_ASSERT(my_gen != 0); // no fork can precede serial initialization

  // Now wait for the donors to arrive: a slice at a time, until we have as many
  // as we wanted, or the time runs out. Because we have to release the lock
  // while waiting, we have to recheck additional conditions, listed below.
  //
  // Both the budget and each slice are measured on __kmp_donated_now_nsec(),
  // which is the clock the waits below are taken against -- see "The clock"
  // above for why that is not KMP_NOW().
  kmp_uint64 start = __kmp_donated_now_nsec();
  kmp_uint64 now = start;
  kmp_uint64 budget = start + (kmp_uint64)KMP_DONATED_INIT_BUDGET_MSEC *
                                  KMP_DONATED_NSEC_PER_MSEC;
  while (
      // Fewer parked than we wanted (save got for uses after the loop).
      (got = __kmp_donated_available()) < wanted &&
      // A teardown or a fork has not spent the table meanwhile.
      !__kmp_donated_disabled &&
      // No other root settled this generation, and it has not ended under us.
      !__kmp_donated_limit_settle_moot(my_gen) &&
      // Haven't exhausted our time budget.
      now < budget) {
    // Compute the deadline for this slice, capped by our overall budget -- a
    // slice longer than what is left of the budget would be no slicing at all.
    kmp_uint64 deadline = now + (kmp_uint64)KMP_DONATED_WAIT_SLICE_MSEC *
                                    KMP_DONATED_NSEC_PER_MSEC;
    if (deadline > budget)
      deadline = budget;
    // Field by field, not as an aggregate initializer: POSIX fixes the names of
    // these two members but neither their order nor that they are the only
    // ones.
    struct timespec ts;
    ts.tv_sec = (time_t)(deadline / KMP_NSEC_PER_SEC);
    ts.tv_nsec = (long)(deadline % KMP_NSEC_PER_SEC);
    // Until the slice expires or a broadcast (a donation arriving or a spend).
    (void)pthread_cond_timedwait(&__kmp_donated_cond, &__kmp_donated_lock, &ts);
    now = __kmp_donated_now_nsec();
  }

  // Another root settled this generation while we waited, or the generation
  // ended under us. Either way we drop out without touching __kmp_max_nth: the
  // limit we would write was computed against a table, or a runtime, that has
  // since moved on.
  if (__kmp_donated_limit_settle_moot(my_gen)) {
    pthread_mutex_unlock(&__kmp_donated_lock);
    KA_TRACE(20, ("__kmp_donated_initialize: generation %u settle dropped, no "
                  "longer wanted\n",
                  (unsigned)my_gen));
    return;
  }

  // Now we are in charge of settling __kmp_max_nth, the maximum number of
  // threads OpenMP will generate per device. We cannot create pthreads here on
  // the fly, so the limit has to come down to the donors that actually arrived.
  // Two cases, in the order tested below: donations are disabled, and the max
  // is 1 (the root thread alone); or no maximum was given, or we waited too
  // long, and the max is what we got.
  int spent = __kmp_donated_disabled;
  int shortfall = (declared && got < wanted);
  if (spent) {
    KMP_DEBUG_ASSERT(got == 0);
    __kmp_max_nth = 1; // One for the root thread.
  } else if (!declared || shortfall) {
    __kmp_max_nth = got + 1; // Push one for the root
  }
  KMP_ATOMIC_ST_REL(&__kmp_donated_limit_settled, my_gen);
  // So a thread still in the loop leaves at this stamp rather than at its next
  // slice boundary. Under the lock, so it cannot slip between test and wait.
  pthread_cond_broadcast(&__kmp_donated_cond);
  pthread_mutex_unlock(&__kmp_donated_lock);

  KA_TRACE(10, ("__kmp_donated_initialize: %s: %d donated, %d wanted, waited "
                "%dms of a %dms budget, limit now %d\n",
                spent       ? "donations already spent"
                : !declared ? "no declared limit"
                : shortfall ? "declaration not honored"
                            : "declaration honored",
                got, wanted, (int)((now - start) / KMP_DONATED_NSEC_PER_MSEC),
                KMP_DONATED_INIT_BUDGET_MSEC, __kmp_max_nth));

  // Provide feedback as appropriate. The order matters: a spent table is a
  // shortfall too, and being spent is the more useful thing to say.
  if (spent)
    KMP_WARNING(DonatedThreadsDisabled);
  else if (!declared)
    __kmp_msg(kmp_ms_warning, KMP_MSG(DonatedThreadsNoLimit, got),
              KMP_HNT(SetDeviceThreadLimit), __kmp_msg_null);
  else if (shortfall)
    __kmp_msg(kmp_ms_warning,
              KMP_MSG(DonatedThreadsNotAllArrived, wanted, got,
                      KMP_DONATED_INIT_BUDGET_MSEC),
              KMP_HNT(DonateMoreThreads), __kmp_msg_null);
}

// Runs on the acquirer: the primary thread of the team being formed, which is
// forking and needs one more worker. Hands that worker its (routine, th) and
// returns its real pthread_self().
//
// Never waits, and contains nothing that could: __kmp_donated_initialize() has
// already lowered __kmp_max_nth to the donors that arrived, and that limit
// bounds thread creation, so this request's donor is parked before the request
// is made. Finding none parked means that reasoning is broken, which is fatal
// on the spot -- waiting could only turn a reportable bug into a hang.
pthread_t __kmp_donated_acquire_thread(void *th, void *(*routine)(void *)) {
  KMP_DEBUG_ASSERT(th != NULL);
  KMP_DEBUG_ASSERT(routine != NULL);

  pthread_mutex_lock(&__kmp_donated_lock);

  // Locate a PARKED thread, and claim it for this acquirer. The scan is also
  // what makes taking dt_lock below safe: a slot is PARKED only once its donor
  // has created both.
  int j = 0;
  kmp_donated_thread_t *dt = NULL;
  for (; j < __kmp_donated_nth; ++j) {
    if (KMP_ATOMIC_LD_ACQ(&__kmp_donated_threads[j].dt_state) ==
        KMP_DT_PARKED) {
      dt = &__kmp_donated_threads[j];
      break;
    }
  }

  // By the proper setting of __kmp_max_nth, a parked donor should always be
  // found. If not, we report a fatal error as the runtime is not respecting the
  // per-device thread limit.
  if (dt == NULL) {
    int donated = __kmp_donated_nth;
    pthread_mutex_unlock(&__kmp_donated_lock);
    // Every slot claimed and none parked is its own diagnosis, and the remedy
    // is a larger table -- a compile-time constant, so the hint names that.
    if (donated == KMP_MAX_DONATED_THREADS)
      __kmp_fatal(KMP_MSG(DonatedThreadsTableFull, KMP_MAX_DONATED_THREADS),
                  KMP_HNT(EnlargeDonatedTable), __kmp_msg_null);
    __kmp_fatal(KMP_MSG(DonatedThreadsShortfall, donated + 1, donated),
                KMP_HNT(SubmitBugReport), __kmp_msg_null);
  }

  // We have now successfully claimed a PARKED donor thread. We change its state
  // from PARKED to ASSIGNED and init the proper fields. Done before the table
  // lock is released, so no other acquirer can pick this slot and no spend can
  // discard it into RELEASED.
  KMP_ATOMIC_ST_REL(&dt->dt_state, KMP_DT_ASSIGNED);
  // dt_arg before dt_routine, and under the table lock: dt_routine is what the
  // donor waits on, so publishing it first would offer work not yet fully
  // described, and dt_arg is the write the releaser's scan reads.
  dt->dt_arg = th;
  pthread_t handle = dt->dt_thread;
  pthread_mutex_unlock(&__kmp_donated_lock);

  // Now signal the donor thread that its work has been assigned. The thread
  // waiting on its lock and condition will awake and start executing the
  // routine.
  pthread_mutex_lock(&dt->dt_lock);
  dt->dt_routine = routine;
  pthread_cond_broadcast(&dt->dt_cond);
  pthread_mutex_unlock(&dt->dt_lock);

  KA_TRACE(10,
           ("__kmp_donated_acquire_thread: donated thread %d assigned to th "
            "%p\n",
            j, th));
  return handle;
}

// Runs on the thread shutting the runtime down, from __kmp_internal_end() just
// after it sets g_done. The donated threads are on their way out of
// __kmp_launch_worker() and back to the application, so nothing here is usable
// again: later donations are refused and any generation that follows
// serializes.
//
// This is the ordered place to record that, and why the donors' own way out is
// not relied on: it runs under __kmp_initz_lock, which the next generation's
// serial initialization needs, so that generation cannot start without seeing
// it.
void __kmp_donated_disable(void) {
  __kmp_donated_spend();

  KA_TRACE(10, ("__kmp_donated_disable: shutting down; donations disabled\n"));
}

// Runs on the releaser: the thread shutting the runtime down and draining
// __kmp_thread_pool, once per worker it obtained. Waits for that worker's
// routine to return and reports what pthread_join() would have, without joining
// the thread -- it belongs to the application. g_done is already set, so the
// donor it waits for is on its way out of __kmp_launch_worker().
int __kmp_donated_release_thread(void *th, void **exit_val) {
  KMP_DEBUG_ASSERT(th != NULL);
  KMP_DEBUG_ASSERT(exit_val != NULL);

  pthread_mutex_lock(&__kmp_donated_lock);
  // Locate the donated thread by its th value. Use the identity of the
  // kmp_info_t *, never by pthread_t: on z/OS that is a struct and cannot be
  // compared. At most one entry names a given th, since entries are never
  // released and each worker is reaped once.
  kmp_donated_thread_t *dt = NULL;
  for (int i = 0; i < __kmp_donated_nth; ++i) {
    if (__kmp_donated_threads[i].dt_arg == th) {
      dt = &__kmp_donated_threads[i];
      break;
    }
  }
  // We have a fatal error when we cannot locate the donated thread.
  if (dt == NULL) {
    pthread_mutex_unlock(&__kmp_donated_lock);
    __kmp_fatal(KMP_MSG(DonatedThreadNotFound), KMP_HNT(SubmitBugReport),
                __kmp_msg_null);
  }
  // Gather the donor state, which should be either in the routine (ASSIGNED) or
  // already out of it (GONE).
  kmp_int32 state = KMP_ATOMIC_LD_ACQ(&dt->dt_state);
  KMP_DEBUG_USE_VAR(state);
  KMP_DEBUG_ASSERT(state == KMP_DT_ASSIGNED || state == KMP_DT_GONE);
  pthread_mutex_unlock(&__kmp_donated_lock);

  KA_TRACE(10,
           ("__kmp_donated_release_thread: waiting for th %p to return from "
            "its routine\n",
            th));

  // Wait until the donor reaches the GONE state, which means it has finished
  // executing the routine and has set the return value.
  pthread_mutex_lock(&dt->dt_lock);
  while (KMP_ATOMIC_LD_ACQ(&dt->dt_state) != KMP_DT_GONE)
    pthread_cond_wait(&dt->dt_cond, &dt->dt_lock);
  void *ret = dt->dt_ret;
  pthread_mutex_unlock(&dt->dt_lock);

  KA_TRACE(10, ("__kmp_donated_release_thread: th %p returned %p\n", th, ret));
  *exit_val = ret;
  return 0;
}

// The three fork() handlers, from __kmp_atfork_prepare(), _parent() and
// _child(). Holding the table lock across the fork is what lets the child touch
// the table at all: a mutex is inherited in whatever state it was in, and every
// entry point here locks it. The table lock is taken last, being the innermost.
//
// The per-slot locks need no quiescing: the child marks every donation unusable
// before releasing the table lock, so no slot is claimed, handed out or reaped
// there, and only a slot in use is ever locked.
void __kmp_donated_atfork_prepare(void) {
  pthread_mutex_lock(&__kmp_donated_lock);
}

void __kmp_donated_atfork_parent(void) {
  pthread_mutex_unlock(&__kmp_donated_lock);
}

void __kmp_donated_atfork_child(void) {
  // Only the forking thread survives, so the table names threads that no longer
  // exist -- the same fact a teardown records. NULL because there is nobody to
  // wake: PARKED --> GONE directly rather than through RELEASED, and the child
  // stays on the table lock.
  __kmp_donated_spend_locked(NULL);
  pthread_mutex_unlock(&__kmp_donated_lock);
  KA_TRACE(10,
           ("__kmp_donated_atfork_child: the donated threads did not "
            "survive the fork; donations disabled, the child serializes\n"));
}

#else // KMP_USE_DONATED_THREADS

// The declaration in omp.h is unconditional, so the symbol has to exist even
// when the feature is not compiled in.
int kmp_donate_thread(void) {
  KA_TRACE(10, ("kmp_donate_thread: refused with ENOSYS: donated threads are "
                "not compiled in\n"));
  return ENOSYS;
}

#endif // KMP_USE_DONATED_THREADS
