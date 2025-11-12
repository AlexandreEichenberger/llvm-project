#include <atomic>
#include <cassert>
#include <chrono>
#include <functional>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <thread>
#include <unordered_map>

#include "kmp_pool.h"

/////////////////////////////////////////////////////////////////////////////////
// Local Symbols / Variables.
/////////////////////////////////////////////////////////////////////////////////

// Thread local's unique thread ID is used to retrieve the
// WorkerThreadPthreadType associated with a given task. This value is set to
// U when the worker thread W start to work on the routine R associated with the
// uniqueTid U. The field is reset to UNDEF once the routine R has completed its
// work.
thread_local WorkerThreadPthreadType threadSpecificUniqueTid = UNIQUE_TID_UNDEF;

// Independent functions not tied to a class which are needed to implement
// functionality.
extern void *threadExitLoop(void *t) { return nullptr; };

/////////////////////////////////////////////////////////////////////////////////
// Debug and Helper functions
/////////////////////////////////////////////////////////////////////////////////

// Debug printing: 0 none; 1 error; 2 start/stop; 3 all
#define DEBUG 1
#define DP(level, code)                                                        \
  if (level <= DEBUG) {                                                        \
    code;                                                                      \
    fflush(stdout);                                                            \
  }

#define memoryFence() std::atomic_thread_fence(std::memory_order_seq_cst)

///////////////////////////////////////////////////////////////////////////////
// WorkerThreadInfo methods
///////////////////////////////////////////////////////////////////////////////

WorkerThreadInfo::WorkerThreadInfo(int64_t gTid)
    : globalTid(gTid), uniqueTidSeed(gTid) {
  assert(gTid >= 0ll && "expected nonnegative global thread id");
}

///////////////////////////////////////////////////////////////////////////////
// Id management and getters.

int64_t WorkerThreadInfo::getGlobalThreadId() { return globalTid; }

/*static */ int64_t
WorkerThreadInfo::getGlobalTid(WorkerThreadPthreadType uniqueTid) {
  // UniqueTid defined so that its lower bits encode the global thread
  // id associated with that uniqueTid.
  return uniqueTid % MAX_POOL_SIZE;
}

WorkerThreadPthreadType WorkerThreadInfo::getUniqueThreadId() {
  return uniqueTid;
}

WorkerThreadPthreadType WorkerThreadInfo::getNextUniqueTid() {
  // Increment by MAX_POOL_SIZE to preserve the property of uniqueTid.
  uniqueTidSeed += MAX_POOL_SIZE;
  assert(uniqueTidSeed != UNIQUE_TID_UNDEF && "expected defined uniqueTd");
  assert(getGlobalTid(uniqueTidSeed) == globalTid && "mapping issue");
  return uniqueTidSeed;
}

///////////////////////////////////////////////////////////////////////////////
// Routine related calls.

// Call by the parent thread P who detected an idle worker thread W. This call
// uniquely describes the task to be performed by thread W. It defines
// its a routine, its parameter, and its uniqueTid.
WorkerThreadPthreadType
WorkerThreadInfo::setRoutine(WorkerThreadRoutineType newRoutine,
                             WorkerThreadArgType newParameter,
                             bool newDetached) {

  // Work can be assigned only to idle worker threads.. Only one
  // pthread_create can see a given thread transitioning from idle to busy, so
  // there is no race condition here between different threads executing a
  // pthread_create.
  assert(isIdle() && "Expected idle thread when setting a new routine");
  assert(newRoutine && "expected a new routine");
  WorkerThreadPthreadType newUniqueTid = getNextUniqueTid();
  DP(2, printf("  %lu %ld setRoutine: Fct 0x%llx\n", newUniqueTid, globalTid,
               (unsigned long long)newRoutine));
  routine = newRoutine;
  parameter = newParameter;
  detached = newDetached;
  setUniqueTidAndSignalWorkerLoop(newUniqueTid);
  return newUniqueTid;
}

// Call the routine.
void *WorkerThreadInfo::callRoutine() {
  assert(uniqueTid != UNIQUE_TID_UNDEF && "expected defined unique id");
  assert(routine && "expected a routine");
  // Set the thread private threadSpecificUniqueTid so that calls to
  // pthread_self() can find the right pthread id (namely a uniqueTid).
  threadSpecificUniqueTid = uniqueTid;
  return routine(parameter);
}

// Reset the work descriptor to indicate that this thread is idle.
void WorkerThreadInfo::resetRoutine() {
  DP(2, printf("  %lu %ld resetRoutine: Fct 0x%llx\n", uniqueTid, globalTid,
               (unsigned long long)routine));
  routine = nullptr; // Not really needed; used to check nonull is asserts.
  threadSpecificUniqueTid = UNIQUE_TID_UNDEF;
  // Memory fence to ensure that the reset values are seen before the
  // uniqueTid is reset.
  memoryFence();
  // Setting uniqueTid to UNDEF will indicate to others that the work is
  // completed and that the result of the routine can be fetched (for joining
  // threads).
  uniqueTid = UNIQUE_TID_UNDEF;
}

// Idle (rely on synchronized uniqueTid variable).
bool WorkerThreadInfo::isIdle() { return uniqueTid == UNIQUE_TID_UNDEF; }

// Terminated.
bool WorkerThreadInfo::isTerminated() { return routine == threadExitLoop; }
void WorkerThreadInfo::setTerminated() {
  setRoutine(threadExitLoop, nullptr, true);
}

// Support for detachable.
bool WorkerThreadInfo::isDetached() { return detached; }

pthread_t WorkerThreadInfo::getPthread() {
  assert(thread != (pthread_t)0 && "uninitialized WorkerThreadInfo");
  return thread;
}

void WorkerThreadInfo::setPthread(pthread_t newThread) {
  assert(newThread != (pthread_t)0 && "uninitialized WorkerThreadInfo");
  thread = newThread;
}

///////////////////////////////////////////////////////////////////////////////
// Synchronization methods needed for pthread_create & worker loop).

void WorkerThreadInfo::setUniqueTidAndSignalWorkerLoop(
    WorkerThreadPthreadType newUniqueTid) {
  int rc = pthread_mutex_lock(&mutexForWorkerLoop);
  assert(!rc && "failed to acquire worker loop lock");
  // Set new uniqueTid here as it is used as a test in the matching wait
  // condition.
  uniqueTid = newUniqueTid;
  // Signal and release lock.
  rc = pthread_cond_signal(&conditionForWorkerLoop);
  assert(!rc && "failed to signal on worker loop condition");
  rc = pthread_mutex_unlock(&mutexForWorkerLoop);
  assert(!rc && "failed to release worker loop lock");
}

void WorkerThreadInfo::waitInWorkerLoop() {
  int rc = pthread_mutex_lock(&mutexForWorkerLoop);
  assert(!rc && "failed to acquire worker loop lock");
  // UniqueTid is preserved into the thread pool info while the thread
  // is working on it. Once it is done, its uTid is either set to "undef" or a
  // new uniqueTid. Check for that here.
  while (isIdle()) {
    rc = pthread_cond_wait(&conditionForWorkerLoop, &mutexForWorkerLoop);
    assert(!rc && "failed to wait on worker loop condition");
  }
  rc = pthread_mutex_unlock(&mutexForWorkerLoop);
  assert(!rc && "failed to release worker loop lock");
}

///////////////////////////////////////////////////////////////////////////////
// Synchronization methods needed for pthread_join & worker loop).

// Store value_ptr in uniqueTid -> value_ptr and signal threads waiting in a
// pthread_join that the value is now available. ResetRoutine to keep data in
// sync.
void WorkerThreadInfo::signalForJoinAndResetRoutine(ThreadPool *threadPool,
                                                    void *value_ptr) {
  unsigned long long key = (unsigned long long)uniqueTid;
  unsigned long long value = (unsigned long long)value_ptr;
  int rc = pthread_mutex_lock(&mutexForJoin);
  assert(!rc && "failed to acquire join lock");
  // Store value ptr in map if joinable.
  assert(uniqueTidToRoutineReturnValueMap.count(key) == 0 &&
         "expected no entries");
  if (!isDetached()) {
    uniqueTidToRoutineReturnValueMap[key] = value;
    DP(3,
       printf("  %lu %ld map: %lu -> %lu\n", uniqueTid, globalTid, key, value));
  }
  // Reset the thread worker info to indicate that that thread is now
  // available to new tasks. Must be done here as otherwise a joining thread
  // may believe it still has to wait for another signal that will never
  // comes.
  resetRoutine();

  // Indicate that the thread associated with gTid is available.
  threadPool->setThreadAvailable(globalTid);

  // Signal and release lock.
  rc = pthread_cond_signal(&conditionForJoin);
  assert(!rc && "failed to signal on join condition");
  rc = pthread_mutex_unlock(&mutexForJoin);
  assert(!rc && "failed to release join lock");
}

// Since signalForJoinAndResetRoutine is the mutex that reset the routine, we
// want to "set" the detachable flag in that same mutex so that there are no
// race condition between a thread completing and a thread setting the detach
// bit.
int WorkerThreadInfo::setDetached(WorkerThreadPthreadType detachingUTid) {
  int failureCode;
  int rc = pthread_mutex_lock(&mutexForJoin);
  assert(!rc && "failed to acquire join lock");
  if (detachingUTid == uniqueTid) {
    DP(3, printf("  %lu %ld setDetached: success\n", uniqueTid, globalTid));
    detached = true;
    failureCode = 0;
  } else {
    DP(3, printf("  %lu %ld setDetached: failure, now processing %lu\n",
                 detachingUTid, globalTid, uniqueTid));
    detached = true;
    failureCode = 1;
  }
  rc = pthread_mutex_unlock(&mutexForJoin);
  assert(!rc && "failed to release join lock");
  return failureCode;
}

bool WorkerThreadInfo::waitInJoin(WorkerThreadPthreadType joiningUTid,
                                  void **value_ptr) {
  assert(value_ptr && "expected pointer");
  unsigned long long key = (unsigned long long)joiningUTid;
  *value_ptr = nullptr; // Not needed, to keep things tidy.
  int rc = pthread_mutex_lock(&mutexForJoin);
  assert(!rc && "failed to acquire join lock");
  // UniqueTid is preserved into the thread pool info while the thread
  // is working on it. Once it is done, its uTid is either set to "undef" or a
  // new uniqueTid. Check for that here.
  while (uniqueTid == joiningUTid) {
    rc = pthread_cond_wait(&conditionForJoin, &mutexForJoin);
    assert(!rc && "failed to wait on join condition");
  }
  // Get the value since we can fetch it now
  int64_t count = uniqueTidToRoutineReturnValueMap.count(key);
  bool hasValue = (count > 0);
  if (hasValue) {
    assert(count == 1 && "expected one entry only");
    uint64_t value = uniqueTidToRoutineReturnValueMap[key];
    uniqueTidToRoutineReturnValueMap.erase(key);
    DP(3, printf("  %lu %ld unmap: %lu -> %lu\n", joiningUTid, globalTid, key,
                 value));
    *value_ptr = (void *)value;
  }
  rc = pthread_mutex_unlock(&mutexForJoin);
  assert(!rc && "failed to release join lock");
  return hasValue;
}

#if DEBUG
WorkerThreadRoutineType WorkerThreadInfo::getRoutine() { return routine; }

/* static */ int64_t WorkerThreadInfo::getGlobalTid(void *pthread) {
  return getGlobalTid((WorkerThreadPthreadType)pthread);
}
#endif

///////////////////////////////////////////////////////////////////////////////
// ThreadPool methods
///////////////////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////////////////
// Methods to construct and register threads into the pool.

// Constructor.
ThreadPool::ThreadPool(int64_t n) {
  maxPoolSize = n < MAX_POOL_SIZE ? n : MAX_POOL_SIZE;
  assert(maxPoolSize > 0 && "assumed at least one thread for a given pool");
  // Init pool info with its global id.
  for (int64_t gTid = 0ll; gTid < maxPoolSize; ++gTid)
    pool[gTid] = WorkerThreadInfo(gTid);
  poolSize.store(0ll);
  threadBusyStatus.store(0ull);
  availableThreadNum.store(0ll);
  // No need for memory fence as default atomic store use full consistency.
}

///////////////////////////////////////////////////////////////////////////////
// Method used for an existing thread to register into the pool.
//
// Return 1 on error (pool was already full), and typically remain in this
// function looking for new work.

int ThreadPool::enterThreadPool() {
  // Get the thread's global thread id and grow the pool size.
  int64_t gTid = poolSize.fetch_add(1ll);
  if (gTid >= maxPoolSize) {
    DP(1, printf("  0 %ld enterThreadPool: Thread pool is full.\n", gTid));
    return EAGAIN;
  }
  assert(gTid >= 0 && "expected positive global thread id");
  WorkerThreadInfo *threadInfo = &pool[gTid];
  pthread_t thread = ::pthread_self();
  threadInfo->setPthread(thread);
  memoryFence();
  incrementNumberOfAvailableThreads();
  // Start waiting for tasks to be executed.
  DP(3, printf("  %ld enterThreadPool: start work loop\n", gTid));
  while (true) {
    threadInfo->waitInWorkerLoop();
    if (!threadInfo->isIdle()) {
      // Check if we are requested to terminate loop.
      if (threadInfo->isTerminated())
        break;
      // Have a function: call and cleanup.
      callRoutineAndCleanup(threadInfo);
    }
  }
  DP(2, printf("  0 %ld WorkerLoop: Stopping loop.\n", gTid));
  threadInfo->resetRoutine();
  // Success.
  return 0;
}

///////////////////////////////////////////////////////////////////////////////
// Methods implementing pthread library calls.

// Provide the same functionality as pthread.h. The pthread_t value returned
// correspond to the uniqueTid, which is unique and never reused.
//
// Currently only support the detachstate attribute.

int ThreadPool::pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                               void *(*start_routine)(void *), void *arg) {
  // Determine if thread attribute is joinable or detached.
  bool detached = false;
  int detachstate;
  if (attr && !pthread_attr_getdetachstate(attr, &detachstate))
    detached = detachstate == PTHREAD_CREATE_DETACHED;
  // Attempt to find a free thread, while there are some; cap at 100
  // attempts.
  int64_t attempt = 0;
  for (; attempt < 100 && getNumberOfAvailableThreads() > 0; ++attempt) {
    // Search for a free thread in the pool.
    int64_t size = poolSize.load();
    for (int64_t gTid = 0; gTid < size; ++gTid) {
      if (wasThreadAvailableBeforeBeingMarkedBusy(gTid)) {
        // First set the work descriptor associated with call.
        WorkerThreadPthreadType uTid =
            pool[gTid].setRoutine(start_routine, arg, detached);
        DP(3, printf("  %lu %ld pthread_create: Set fct %llx (%d iter)\n", uTid,
                     gTid, (long long int)start_routine, (int)attempt));
        // Then return the new uniqueTid as the pthread value. Ensure
        // that the routine fields are seen before thread.
        memoryFence();
        *thread = (pthread_t)uTid;
        // Return 0 on success.
        return 0;
      }
    }
  }
  // Failed to find any available free threads.
  DP(1, printf("  pthread_create: No threads available (%ld try, avail "
               "%ld out of %ld pool threads).\n",
               attempt, getNumberOfAvailableThreads(), poolSize.load()));
  *thread = (pthread_t)UNIQUE_TID_UNDEF; // Not necessary, just to keep tidy.
  return EAGAIN;
}

// Return the pthread_t, namely the uniqueTid associated with this
// thread.
pthread_t ThreadPool::pthread_self() {
  WorkerThreadPthreadType uTid = threadSpecificUniqueTid;
  assert(uTid != UNIQUE_TID_UNDEF && "unspecified unique thread id");
  return (pthread_t)uTid;
}

int ThreadPool::pthread_equal(pthread_t t1, pthread_t t2) {
  return ((WorkerThreadPthreadType)t1) == ((WorkerThreadPthreadType)t2);
}

int ThreadPool::pthread_detach(pthread_t thread) {
  WorkerThreadPthreadType uTid = (WorkerThreadPthreadType)thread;
  // Wait until thread is done with uTid.
  int64_t gTid = WorkerThreadInfo::getGlobalTid(uTid);
  if (!validGlobalTid(gTid)) {
    // Corrupted thread parameter., could not find the corresponding info.
    DP(1,
       printf("  %lu %ld pthread_detach: Could not find thread\n", uTid, gTid));
    return ESRCH;
  }
  WorkerThreadInfo *threadInfo = &pool[gTid];
  int rc = threadInfo->setDetached(uTid);
  if (rc) {
    // Detach for a thread that completed just after we detached.
    DP(1, printf("  %lu %ld pthread_detach: Thread competed already.\n", uTid,
                 gTid));
    return ESRCH;
  }
  return 0; // Success.
}

int ThreadPool::pthread_join(pthread_t thread, void **value_ptr) {
  assert(value_ptr && "expected a nonnull value_ptr");
  // Simulated pthread_t thread value is uniqueTid.
  WorkerThreadPthreadType uTid = (WorkerThreadPthreadType)thread;
  // Wait until thread is done with uTid.
  int64_t gTid = WorkerThreadInfo::getGlobalTid(uTid);
  if (!validGlobalTid(gTid)) {
    // Corrupted thread parameter., could not find the corresponding info.
    DP(1,
       printf("  %lu %ld pthread_join: Could not find thread\n", uTid, gTid));
    return ESRCH;
  }
  if (uTid == threadSpecificUniqueTid) {
    // A deadlock was detected or the value of thread specifies the calling
    // thread.
    DP(1, printf("  %lu %ld pthread_join: Calling join on self\n", uTid, gTid));
    return EDEADLK;
  }

  // Wait for the worker thread to complete the task.
  DP(3, printf("  %lu %ld pthread_join: joining with pthread_t %lu\n", uTid,
               gTid, uTid));
  bool hasValue = pool[gTid].waitInJoin(uTid, value_ptr);
  if (!hasValue) {
    // Thread completed the work but did not leave a value_ptr, thus signaling
    // that this was not a joinable thread. Or that the thread was already
    // joined once and thus the map entry was already removed.
    DP(1, printf("  %lu %ld pthread_join: No return value\n", uTid, gTid));
    return EINVAL;
  }
  DP(3, printf("  %lu %ld pthread_join: joined with pthread_t %ld\n", uTid,
               gTid, uTid));
  // Success.
  return 0;
}

// Snapshot on an upper bound on the number of available threads. Can change
// at anytime up (or possibly down).
int64_t ThreadPool::getThreadPoolSize() { return poolSize.load(); }
int64_t ThreadPool::getMaxThreadPoolSize() { return maxPoolSize; }

///////////////////////////////////////////////////////////////////////////////
// Support function for the worker loop function.

// Method used by the worker loop to indicate the completion of a routine.
void ThreadPool::callRoutineAndCleanup(WorkerThreadInfo *threadInfo) {
  // Call routine.
  DP(3, printf("  %lu %ld call&cleanup: Call 0x%llx with pthread %lu\n",
               threadInfo->getUniqueThreadId(), threadInfo->getGlobalThreadId(),
               (unsigned long long)threadInfo->getRoutine(),
               threadInfo->getUniqueThreadId()));
  void *value_ptr = threadInfo->callRoutine();
  DP(3, printf("  %lu %ld call&cleanup: Return 0x%llx with value %ld\n",
               threadInfo->getUniqueThreadId(), threadInfo->getGlobalThreadId(),
               (unsigned long long)threadInfo->getRoutine(),
               (long long)value_ptr));

  // Capture return value if not detached. Wakeup the thread performing a
  // join, if one is waiting.  Still perform this operation for detached (aka
  // non joinable) tasks since a thread might be mistakenly performing a join
  // and we don't want them to deadlock, but rather them reporting an error.
  threadInfo->signalForJoinAndResetRoutine(this, value_ptr);
}

// Atomic-Or operation to set a bit of the thread status variable. The bit is
// the one corresponding to the gTid thread. If that bit is flipped (aka it
// was zero), then we found an available thread. If that bit is unchanged (aka
// it was already 1), then the thread was busy and remains seen as busy.
bool ThreadPool::wasThreadAvailableBeforeBeingMarkedBusy(int64_t gTid) {
  // Attempt to flip bit to busy
  uint64_t mask = ((uint64_t)1) << ((uint64_t)gTid);
  uint64_t oldStatus = threadBusyStatus.fetch_or(mask);
  if ((oldStatus & mask) == (uint64_t)0) {
    // Thread was available.
    decrementNumberOfAvailableThreads();
    return true;
  }
  return false;
}

// Flip the bit associated with gTid back to zero.
void ThreadPool::setThreadAvailable(int64_t gTid) {
  // Increment threads available before flipping the bits.
  incrementNumberOfAvailableThreads();
  // Fip bit to zero.
  uint64_t mask = ((uint64_t)1) << ((uint64_t)gTid);
  threadBusyStatus.fetch_xor(mask, std::memory_order_release);
}

// Snapshot of how many threads are free, aka available for work. Can change
// at anytime up or down.
int64_t ThreadPool::getNumberOfAvailableThreads() {
  return availableThreadNum.load();
}

void ThreadPool::incrementNumberOfAvailableThreads() {
  // Use release consistency here because this protect stores above the call.
  int64_t newNum = availableThreadNum.fetch_add(1ll) + 1;
  assert(newNum <= poolSize.load() && "should be larger than pool size");
}

void ThreadPool::decrementNumberOfAvailableThreads() {
  // Use release consistency here because this protect stores above the call.
  int64_t newNum = availableThreadNum.fetch_sub(1ll) - 1;
  assert(newNum >= 0 && "should never become negative");
}

bool ThreadPool::validGlobalTid(int64_t gTid) {
  return gTid >= 0 && gTid < maxPoolSize;
}

// Translate the ThreadPool returned pthread_t, which is really a Unique thread
// id into the underlying pthread_t of the actual pthread that
// performs/performed the work.
pthread_t ThreadPool::getUnderlyingPthread(pthread_t thread) {
  WorkerThreadPthreadType uTid = (WorkerThreadPthreadType)thread;
  int64_t gTid = WorkerThreadInfo::getGlobalTid(uTid);
  assert(validGlobalTid(gTid) && "invalid unique thread id");
  return pool[gTid].getPthread();
}

/////////////////////////////////////////////////////////////////////////////////
// Public Interface implementation (defined in kmp_pthread.h)
/////////////////////////////////////////////////////////////////////////////////

// Global variables for thread pool
static ThreadPool *threadPool = nullptr;
static pthread_mutex_t mutexThreadPoolInit = PTHREAD_MUTEX_INITIALIZER;

/////////////////////////////////////////////////////////////////////////////////
// Support

// Get thread limit from it or env. Protected by a mutex.
static void getThreadLimit(int &threadLimit, const char *envVar) {
  if (threadLimit == 0) {
    assert(envVar && "expected thread_limit or env_var");
    char *varStr = std::getenv(envVar);
    assert(varStr && "When no thread limit are explicitly provided, expect a "
                     "define env_var");
    int rc = sscanf(varStr, "%d", &threadLimit);
    assert(rc == 1 && "failed to scan env_var");
  }
  assert(threadLimit > 0 && "expected positive thread limit");
}

// Set thread limit to the env. Protected by a mutex.
static void setThreadLimit(int threadLimit, const char *envVar) {
  assert(threadLimit > 0 && "expected a positive thread limit");
  assert(envVar && "expected thread_limit or env_var");
  char varStr[50];
  snprintf(varStr, sizeof(varStr), "%d", threadLimit);
  int rc = setenv(envVar, varStr, 1);
  assert(!rc && "failed to set environment var env_var");
}

// Init protected by a mutex (when we actually have to do the init).
// Return true when we actually perform a new initialization.
bool init(int threadLimit, const char *envVar) {
  // Test if initalized
  if (threadPool)
    return false;
  // Grab lock and test again.
  bool newInit = false;
  int rc = pthread_mutex_lock(&mutexThreadPoolInit);
  assert(!rc && "failed to acquire worker loop lock");
  if (!threadPool) {
    // Still not initialized, now initialize inside the lock.
    getThreadLimit(threadLimit, envVar);
    threadPool = new ThreadPool(threadLimit);
    assert(threadPool && "failed to allocate thread pool");
    int actualLimit = threadPool->getMaxThreadPoolSize();
    if (envVar && threadLimit != actualLimit)
      setThreadLimit(actualLimit, envVar);
    printf("Use thread pool of size %d\n", actualLimit);
    fflush(stdout);
    newInit = true;
  }
  rc = pthread_mutex_unlock(&mutexThreadPoolInit);
  assert(!rc && "failed to release worker loop lock");
  return newInit;
}

static void *registerWorker(void *t) {
  int64_t rc = threadPool->enterThreadPool();
  return (void *)rc;
}

/////////////////////////////////////////////////////////////////////////////////
// Exported functions

extern "C" void pool_pthread_become_worker(int thread_limit,
                                           const char *env_var) {
  init(thread_limit, env_var);
  threadPool->enterThreadPool();
}

extern "C" int pool_pthread_create_all_workers(int thread_limit,
                                               const char *env_var) {
  bool newInit = init(thread_limit, env_var);
  thread_limit = threadPool->getMaxThreadPoolSize();
  if (newInit) {
    // Create the actual pthread and register them to the pool.
    for (int64_t i = 0; i < thread_limit; ++i) {
      pthread_t thread;
      int rc = pthread_create(&thread, nullptr, registerWorker, (void *)i);
      assert(!rc && "Pthread pool creation failure");
    }
  }
  return thread_limit;
}

extern int pool_pthread_wait_until_fully_populated() {
  // At this time, use busy waiting.
  while (threadPool /* is initialized */ &&
         threadPool->getThreadPoolSize() < threadPool->getMaxThreadPoolSize()) {
    // Busy waiting, could update to something else.
    // hi alex std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return threadPool->getMaxThreadPoolSize();
}

extern "C" int pool_pthread_create(pthread_t *thread,
                                   const pthread_attr_t *attr,
                                   void *(*start_routine)(void *), void *arg) {
  assert(threadPool && "uninitialized thread pool");
  return threadPool->pthread_create(thread, attr, start_routine, arg);
}

extern "C" pthread_t pool_pthread_self() {
  assert(threadPool && "uninitialized thread pool");
  return threadPool->pthread_self();
}

extern "C" int pool_pthread_equal(pthread_t t1, pthread_t t2) {
  assert(threadPool && "uninitialized thread pool");
  return threadPool->pthread_equal(t1, t2);
}

extern "C" int pool_pthread_detach(pthread_t thread) {
  assert(threadPool && "uninitialized thread pool");
  return threadPool->pthread_detach(thread);
}

extern "C" int pool_pthread_join(pthread_t thread, void **value_ptr) {
  assert(threadPool && "uninitialized thread pool");
  return threadPool->pthread_join(thread, value_ptr);
}

// Unimplemented

extern "C" void pool_pthread_exit(void *value_ptr) {
  assert(false && "pthread_exit is not implemented");
}

extern "C" int pool_pthread_kill(pthread_t thread, int sig) {
  assert(false && "pthread_kill is not implemented");
}

extern "C" int pool_pthread_cancel(pthread_t thread) {
  assert(false && "pthread_cancel is not implemented");
}

extern void pool_pthread_test_cancel() {
  assert(false && "pthread_test_cancel is not implemented");
}

extern "C" int pool_pthread_setcancelstate(int state, int *oldstate) {
  assert(false && "pthread_setcancelstate is not implemented");
}
extern "C" int pool_pthread_setcanceltype(int type, int *oldtype) {
  assert(false && "pthread_setcanceltype is not implemented");
}

// Pass through
int pool_pthread_getschedparam(pthread_t thread, int *policy,
                               struct sched_param *param) {
  assert(threadPool && "uninitialized thread pool");
  pthread_t underlyingThread = threadPool->getUnderlyingPthread(thread);
  return pthread_getschedparam(underlyingThread, policy, param);
}

int pool_pthread_setschedparam(pthread_t thread, int policy,
                               const struct sched_param *param) {
  assert(threadPool && "uninitialized thread pool");
  pthread_t underlyingThread = threadPool->getUnderlyingPthread(thread);
  return pthread_setschedparam(underlyingThread, policy, param);
}
