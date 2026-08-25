
#ifndef KMP_PTHREAD_H
#define KMP_PTHREAD_H 1

#include <pthread.h>

// Decide whether to use normal pthreads or pool_pthreads
#define KMP_USE_POOL_PTHREAD 1

#if KMP_USE_POOL_PTHREAD

// There are non-posix pthread calls that are not guaranteed to be define
// everywhere. We thus have to handle them with macros only, so that the macro
// issued calls are expanded in the context they are called.

extern "C" pthread_t getNativeThreadButAssertIfPoolThread(pthread_t thread);
extern "C" pthread_t getPoolNativeThread(pthread_t thread);

#define __pool_fallthrough(fct, thread, ...)                                   \
  fct(getPoolNativeThread(thread), __VA_ARGS__)

#define _pool_unimplemented(fct, thread, ...)                                  \
  fct(getNativeThreadButAssertIfPoolThread(thread), __VA_ARGS__)

/////////////////////////////////////////////////////////////////////////////////
// Redirection needed for pthread_* calls to pool_pthread_*
/////////////////////////////////////////////////////////////////////////////////

// Implemented.
#define pthread_create(thread, attr, start_routine, arg)                       \
  pool_pthread_create(thread, attr, start_routine, arg)
#define pthread_self() pool_pthread_self()
#define pthread_equal(t1, t2) pool_pthread_equal(t1, t2)
#define pthread_detach(thread) pool_pthread_detach(thread)
#define pthread_join(thread, value_ptr) pool_pthread_join(thread, value_ptr)
// Not implemented, assert if called.
#define pthread_exit(value_ptr) pool_pthread_exit(value_ptr)
#define pthread_kill(thread, sig) _pool_unimplemented(pthread_kill, thread, sig)
#define pthread_cancel(thread) pool_pthread_cancel(thread)
// Pass through to original pthreads.
#define pthread_getschedparam(thread, policy, param)                           \
  pool_pthread_getschedparam(thread, policy, param)
#define pthread_setschedparam(thread, policy, param)                           \
  pool_pthread_setschedparam(thread, int policy, param)
#define pthread_getattr_np(thread, attr)                                       \
  __pool_fallthrough(pthread_getattr_np, thread, attr)
#define pthread_attr_get_np(thread, attr)                                      \
  __pool_fallthrough(pthread_attr_get_np, thread, attr)
#define pthread_set_name_np(thread, name)                                      \
  __pool_fallthrough(pthread_set_name_np, thread, name)
#define pthread_setname_np(thread, name)                                       \
  __pool_fallthrough(pthread_setname_np, thread, name)

/////////////////////////////////////////////////////////////////////////////////
// Interface definition for pool_pthread_*
/////////////////////////////////////////////////////////////////////////////////

extern "C" {

/////////////////////////////////////////////////////////////////////////////////
// Pool creation and sizing (applies to the three entry points below)
//
// The next three functions each take a (thread_limit, env_var) pair and each
// implicitly initialize the pool, because none of them can assume it already
// exists: a worker may volunteer before the runtime initializes, and the
// runtime may initialize before any worker volunteers. There is exactly one
// pool per process and it is created at most once.
//
// EVERY NUMBER HERE IS A THREAD LIMIT. Every value this interface takes in or
// hands back -- thread_limit, env_var's value, the two return values -- is an
// OpenMP thread limit, counting the master along with the workers, exactly as
// libomp counts it. How large the pool is underneath is an implementation
// detail and is deliberately not exposed: the pool holds workers only, because
// the master never registers in it, so it is one smaller than the limit and
// that discrepancy stays inside. A limit of 1 is therefore legal and means a
// master with no worker at all, the limit being spent entirely on the master.
//
// WHO SETS THE SIZE. The first caller to enter the initialization critical
// section and find the pool absent creates it, and *that* caller's arguments
// alone determine the limit, using the first of:
// o thread_limit, when it is greater than zero (env_var is then not consulted);
// o the value of env_var, when one is provided and parses as a number;
// o the implementation maximum, when neither yields a value.
// The result is then capped by the implementation maximum, so the effective
// limit may end up lower than requested. This is why the two functions that
// return a value return the *actual* limit: it is the only reliable way to
// learn it.
//
// THE SIZE IS THEN FIXED FOREVER. It is written once, while the creating caller
// holds the lock, and no code path ever changes it afterwards. It survives for
// the life of the process, across any number of subsequent calls.
//
// WHAT HAPPENS TO LATER CALLERS' ARGUMENTS. They are ignored, in the strict
// sense that they are never even read: once the pool exists, every one of these
// functions returns from a fast path that precedes any use of thread_limit or
// env_var. Passing values that disagree with the pool's actual limit is
// therefore harmless to the pool itself -- no re-initialization, no
// reallocation, no race, and nothing leaked -- and a caller that wants a
// different limit simply does not get one, silently. Note however: o env_var is
// written back only by the creating caller, and only if that caller
//   supplied one. What is written back is the thread limit the pool can
//   actually staff, in the same convention it was read under, so that reading
//   the variable back yields the value it was set from. If the pool is created
//   by a caller that passed nullptr, the environment is left alone and libomp
//   will size its teams from whatever it already said, which need not match
//   what the pool can staff.
// o Nothing diagnoses a disagreement. A caller that cares must compare the
//   returned limit against what it asked for.
/////////////////////////////////////////////////////////////////////////////////

// Current thread becomes a worker thread in the thread pool, initializing the
// pool first if it does not exist yet (see above for sizing). Does not return
// while this thread remains a pool worker, so a true return means this thread
// served as a worker and the pool has since shut down.
//
// Returns false, at once, if the pool was already full, in which case this
// thread was *not* taken and is handed straight back to the caller. That is not
// a failure the pool can act on, but it does say something the caller may want
// to hear: more threads were offered than the pool has room for. Since the pool
// holds one fewer thread than the limit -- the master's share -- offering
// exactly thread_limit threads is the easy way to arrive here by accident, one
// too many.
extern bool pool_pthread_become_worker(int thread_limit, const char *env_var);
// Initialize the thread pool (see above for sizing) and populate it by creating
// all of its worker threads here. Return the actual thread limit. Note that the
// worker threads are created only when this call is the one that created the
// pool: a caller that finds the pool already populated by volunteers creates
// nothing.
extern int pool_pthread_create_all_workers(int thread_limit,
                                           const char *env_var);
// Initialize the thread pool (see above for sizing), then wait until every one
// of its worker threads has registered, whether they were created by
// pool_pthread_create_all_workers or volunteered through
// pool_pthread_become_worker. Return the actual thread limit. Waits forever if
// fewer workers than the pool holds ever register; returns at once when the
// limit is 1, as a pool with no worker has nothing to wait for.
extern int pool_pthread_wait_until_fully_populated(int thread_limit,
                                                   const char *env_var);

// Pthread interface implemented (working),
extern int pool_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                               void *(*start_routine)(void *), void *arg);
extern pthread_t pool_pthread_self();
extern int pool_pthread_equal(pthread_t t1, pthread_t t2);
extern int pool_pthread_detach(pthread_t thread);
extern int pool_pthread_join(pthread_t thread, void **value_ptr);

// Pthread interface not implemented (asserts).
extern void pool_pthread_exit(void *value_ptr);
extern int pool_pthread_kill(pthread_t thread, int sig);
extern int pool_pthread_cancel(pthread_t thread);

// Pthread interface pass-through (i.e. sent to normal pthread, providing the
// actual pthread_t values of the underlying threads in the pool).
extern int pool_pthread_getschedparam(pthread_t thread, int *policy,
                                      struct sched_param *param);
extern int pool_pthread_setschedparam(pthread_t thread, int policy,
                                      const struct sched_param *param);
}
#else

// Use normal pthreads, nothing changed.

#endif // KMP_USE_POOL_PTHREAD

#endif // KMP_PTHREAD_H
