
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

// Current thread becomes a worker thread in the thread pool after initializing
// it. The maximum thread pool size is determined by the provided thread_limit
// (when non-zero) and otherwise by the provided env_var, subject to
// implementation limitations. When an env_var is provided, it will be set to
// the maximum thread pool size.
extern void pool_pthread_become_worker(int thread_limit, const char *env_var);
// Initialize thread pool and fully populate the pool of worker threads. The
// maximum thread pool size is determined by the provided thread_limit (when
// non-zero) and otherwise by the provided env_var, subject to implementation
// limitations. When an env_var is provided, it will be set to the maximum
// thread pool size. Return the number of threads in the pool.
extern int pool_pthread_create_all_workers(int thread_limit,
                                           const char *env_var);
// Initialize the thread pool, then wait until it is fully populated by worker
// threads that volunteered through pool_pthread_become_worker. The maximum
// thread pool size is determined by the provided thread_limit (when non-zero)
// and otherwise by the provided env_var, subject to implementation limitations.
// When an env_var is provided, it will be set to the maximum thread pool size.
// Return the number of threads in the pool. The pool is initialized here (and
// not assumed to exist) because this may run before any worker has volunteered;
// whichever of the two arrives first initializes the pool and thereby fixes its
// size, so all callers are expected to pass consistent arguments.
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
