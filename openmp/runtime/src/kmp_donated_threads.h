/*
 * kmp_donated_threads.h -- OpenMP workers obtained from donated threads
 */

//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// With KMP_USE_DONATED_THREADS the runtime never calls pthread_create(). The
// application donates threads it already owns by calling kmp_donate_thread()
// from each one; a donated thread parks until the runtime asks for a worker,
// then serves as one until the runtime shuts down. It is never joined: it goes
// back to the application by returning from kmp_donate_thread(), which is also
// what a donation the runtime never needed does, with EAGAIN, once the table is
// spent.
//
// The runtime reaches the table through the two internal entry points below,
// from __kmp_create_worker() and __kmp_reap_worker() -- the same per-platform
// boundary that already has a Linux and a Windows implementation. Nothing else
// in the runtime is aware of the feature.

#ifndef KMP_DONATED_THREADS_H
#define KMP_DONATED_THREADS_H

#include "kmp_config.h"

// kmp_donate_thread() is declared in the installed omp.h -- unconditionally and
// on every platform, because the ABI must not vary by configuration: a program
// that calls it where the feature is compiled out gets a defined ENOSYS rather
// than a link failure. It is deliberately not re-declared here, so that its
// calling convention has exactly one definition.
//
// It is also the only one of the feature's entry points that runs on a donated
// thread, and the only one the application calls at all. The three below run on
// the runtime's own threads.

#if KMP_USE_DONATED_THREADS

// For pthread_t, the one platform type in this header. Include this header
// after "kmp.h", as both users do: platforms whose <pthread.h> is gated behind
// feature-test macros -- z/OS behind _XOPEN_SOURCE, and so behind whatever the
// build sets -- want the runtime's own include order, not a second one reached
// from somewhere else.
#include <pthread.h>

// ---------------------------------------------------------------------------
// Tunables. All three are compile-time constants, gathered here so that the
// only numbers in the feature are in one place.
// ---------------------------------------------------------------------------

// Capacity of the donated-thread table. Every entry is allocated in .bss
// whether or not it is used, so this is purely a memory-versus-ceiling trade.
// An entry is dominated by the mutex and condition variable that make it its
// own thread's wakeup, and both are platform-sized -- 128 bytes per entry on
// Linux and 152 on Darwin -- so 256 entries cost around 32-39 KB of .bss. The
// alternative, one lock for the whole table, would save that and pay for it in
// every donor waking on every other donor's handoff. The useful ceiling on
// the target platform is whatever MAXTHREADS/MAXTHREADTASKS the deployment's
// BPXPRMxx sets, which is a system-programmer setting rather than a tuning
// constant here.
#ifndef KMP_MAX_DONATED_THREADS
#define KMP_MAX_DONATED_THREADS 256
#endif

// How long the first parallel region waits for the application to finish
// donating the threads it declared through KMP_DEVICE_THREAD_LIMIT.
//
// This is paid at most once per generation and it buys the feature's central
// guarantee: that the runtime is never narrower than the application
// asked for, regardless of how the donating threads happen to be
// scheduled. It is charged to first-region latency, so it wants to be a little
// longer than the application's thread start-up takes and no longer -- long
// enough that crossing it means the declaration was wrong rather than merely
// late. It is wall clock and not a count of expired waits: the wait is woken by
// every donation, and a budget spent only by waits that expire would not be a
// bound on anything an application can observe.
#ifndef KMP_DONATED_INIT_BUDGET_MSEC
#define KMP_DONATED_INIT_BUDGET_MSEC 5000
#endif

// Granularity of that wait, which is taken a slice at a time within the budget
// above rather than as one wait for the whole of it. It bounds how long the
// wait can stay blocked without re-reading the table, so a lost wakeup or a
// step of the clock delays the decision by a slice instead of by the budget:
// pthread_condattr_setclock() exists on neither Darwin nor z/OS, so
// pthread_cond_timedwait() can only ever use the realtime clock, which can
// step.
//
// Those two are the only things it bounds, and both leave a wait that could
// already have finished sitting for up to a slice, so the slice is the delay
// they cost. It is sub-second because that delay is charged to first-region
// latency while the donation it is waiting on takes milliseconds, and because
// shrinking it buys that at no price worth naming: the loop leaves on the
// budget and never on a slice, so a smaller slice does not shorten the wait
// that a shortfall pays -- it only divides it into more wakeups, and only on
// that already-degenerate path. Below a hundred milliseconds or so there is
// nothing left to win, the remaining delay being smaller than the spread on
// thread start-up itself.
#ifndef KMP_DONATED_WAIT_SLICE_MSEC
#define KMP_DONATED_WAIT_SLICE_MSEC 100
#endif

// Both of the above are milliseconds, so one constant converts either. kmp.h
// has SEC, USEC and NSEC_PER_USEC, but nothing for nsec-per-msec.
#define KMP_DONATED_NSEC_PER_MSEC (1000 * KMP_NSEC_PER_USEC)

extern "C" {

// Hand the next parked donated thread to the runtime: record (routine, th)
// into its slot, wake it, and return its real pthread_self() so
// __kmp_create_worker() can store that into ds_thread.
//
// Called on the thread that wants a worker, never on a donated one: the primary
// thread of the team being formed, which reaches here from __kmp_fork_call() ->
// __kmp_allocate_thread() -> __kmp_create_worker() while it holds
// __kmp_forkjoin_lock. Usually that is the root thread; with nested parallelism
// it is whichever thread is forking.
//
// Never waits, and contains nothing that could: __kmp_donated_initialize() has
// already established that the donors exist and lowered the device limit to
// their number, and that limit bounds thread creation, so the donor for this
// request is parked before the request is made. Finding none parked therefore
// means that reasoning is broken, and is fatal on the spot -- waiting for a
// donation that the runtime has already been told not to expect could only turn
// a reportable bug into a hang.
//
// `th` is the kmp_info_t * the worker is being obtained for. It is opaque here
// -- the table never dereferences it, it only stores it so
// __kmp_donated_release_thread() can find the same slot again by identity.
pthread_t __kmp_donated_acquire_thread(void *th, void *(*routine)(void *));

// The runtime is shutting down: no donation is usable again, so later ones are
// turned away and any generation that follows serializes. Donors still parked
// are discarded here, which is what leaves the table empty for that generation
// to size itself from; ones already handed out need nothing, since a slot is
// never offered twice.
//
// A discarded donor is also woken, and its kmp_donate_thread() returns EAGAIN
// with nothing having run on the thread. That is the only point at which a
// donation the runtime never needed is given back, so an application that
// donates more generously than its regions turn out to need gets those threads
// again here rather than losing them for the life of the process.
//
// Called on the thread tearing the runtime down, immediately after it sets
// g_done, from every route that sets it and can safely say so. Three do:
//
//   * __kmp_internal_end(), the ordinary teardown, and the one that matters for
//     ordering -- it runs under __kmp_initz_lock, which
//     __kmp_do_serial_initialize() also needs, so a following generation cannot
//     start without seeing this. Leaving the fact to the donated threads to
//     record on their way out would not give that: nothing in a teardown waits
//     for them to get that far.
//   * the two "root still active" branches of __kmp_internal_end_library() and
//     __kmp_internal_end_thread(), which set g_done and return without ever
//     reaching __kmp_internal_end(). g_abort is set there too, and that makes
//     every later __kmp_internal_end_*() return at its top, so those branches are
//     the last chance to hand the threads back at all.
//
// The one route that does not call it is __kmp_team_handler(), the signal
// handler, and it cannot: this takes __kmp_donated_lock, so a signal delivered to
// a thread already holding it would deadlock the process outright. A donor left
// parked by a signal-initiated teardown is therefore not given back -- see the
// backstop at the tail of kmp_donate_thread(), which covers that case only for
// donors the runtime had assigned.
//
// Idempotent, which is what lets those routes overlap without any of them having
// to know whether another got there first.
void __kmp_donated_disable(void);

// Wait for the donated thread that was assigned `th` to return from `routine`,
// and store that return value into *exit_val. The thread itself is deliberately
// not joined: it belongs to the application.
//
// Deliberately shaped like pthread_join(), which is what it stands in for --
// same out-parameter, same int status, 0 on success. That is what lets
// __kmp_reap_worker() substitute one call for the other and leave the rest of
// the function, including its KMP_DEBUG status and exit_val checks, untouched.
//
// Called on the thread that is tearing the runtime down, never on a donated
// one: whichever thread runs __kmp_internal_end() and drains
// __kmp_thread_pool -- the last root to unregister, the library destructor, or
// the caller of omp_pause_resource(omp_pause_hard) -- reaching here from
// __kmp_reap_thread() -> __kmp_reap_worker() while it holds
// __kmp_forkjoin_lock. It runs once per worker obtained. The donated thread
// is on the other side of this handshake: g_done is already set, so it is
// returning from __kmp_launch_worker() and back into its own
// kmp_donate_thread() frame, which is what publishes dt_done here.
int __kmp_donated_release_thread(void *th, void **exit_val);

// Settle this generation's thread limit, once, before the runtime asks for its
// first worker. `nthreads_requested` is what the region being formed asked for;
// a request of 1 needs no worker and returns immediately.
//
// KMP_DEVICE_THREAD_LIMIT is read as the application's *declaration* that it
// will donate that many threads less one, and this function holds it to it: it
// waits for the declared number to arrive, so the first region is as wide as
// the application asked for rather than as wide as thread scheduling happened
// to allow. That wait is the feature's guarantee, and it is why nothing else
// here has to reason about when donation happens relative to initialization.
//
// It lowers __kmp_max_nth only when the declaration cannot be honored -- fewer
// threads arrived than were promised, none were promised at all, or a previous
// generation has already used them up. Lowering it is what keeps the shortfall
// graceful: the existing device-limit check in __kmp_reserve_threads then forms
// a smaller team, or serializes the region, instead of the runtime asking for a
// donor that does not exist. Aborting is never the answer, because on the
// platform this targets the remedy for a shortfall is a PARMLIB change and an
// IPL, so failing the process turns a capacity problem into an outage.
//
// Called on the thread about to fork, from __kmp_fork_call(), before it takes
// __kmp_forkjoin_lock -- so no runtime lock is held across the wait, and a
// donation arriving late can still get through. That is also why two threads can
// be in here at once, each about to form its own first team: the limit is
// settled by whichever gets there first, and the other returns having settled
// nothing rather than deciding it again from a table the winner has since drawn
// on. Deliberately *not* called from
// serial initialization: that runs on the first OpenMP call of any kind,
// including a bare omp_get_max_threads() during application start-up, which may
// well precede the donation loop.
//
// A request of one thread returns without settling anything, so a program whose
// first region is serial still has its limit settled by the first region that
// actually needs a worker.
void __kmp_donated_initialize(int nthreads_requested);

// Begin a new generation, so that __kmp_donated_initialize() settles a limit
// for it. Called once per generation on the thread performing serial
// initialization, from __kmp_do_serial_initialize() under __kmp_initz_lock --
// which is also where the environment is re-parsed, so it is exactly where
// __kmp_max_nth returns to its declared value and the previous generation's
// decision stops being true.
//
// It records the boundary rather than clearing a flag, and that is what keeps a
// settle still running from the generation just ended from being taken for this
// one's: that settle carries the generation it was computed for, which no longer
// matches, so neither its stamp nor its limit is allowed to land. See the two
// stamps in the .cpp.
void __kmp_donated_new_generation(void);

// Quiesce the donated-thread table across fork(), and mark it unusable in the
// child. Called from __kmp_atfork_prepare(), __kmp_atfork_parent() and
// __kmp_atfork_child(), which already do the same for __kmp_initz_lock and
// __kmp_forkjoin_lock; the table's lock belongs in that set for the same reason
// and must be taken last, because it is the innermost lock in the runtime.
//
// _prepare holds the lock over the fork so the child inherits it in a known
// state rather than possibly held by a thread that did not survive. Without
// that, an entry point in the child would block on it forever --
// kmp_donate_thread() takes it before it can test anything.
//
// _child then marks every donation unusable, because only the forking thread
// survives while the table still names the donors of the parent. The next
// generation there serializes rather than waiting for threads that no longer
// exist, and kmp_donate_thread() turns later donations away instead of parking
// them. On the ordinary path the same fact is recorded by
// __kmp_donated_disable() above, which every teardown route reaches --
// omp_pause_resource(omp_pause_hard), __kmpc_end, last-root unregistration, the
// library destructor -- rather than only the one that has an API.
//
// Unlike that path, _child does not go on to wake the donors it discarded: they
// did not survive the fork, so there is no thread to hand back and no dt_lock
// that may be touched. Only the table's own lock is used in the child, which is
// why the per-slot locks need no quiescing here.
void __kmp_donated_atfork_prepare(void);
void __kmp_donated_atfork_parent(void);
void __kmp_donated_atfork_child(void);

} // extern "C"

#endif // KMP_USE_DONATED_THREADS
#endif // KMP_DONATED_THREADS_H
