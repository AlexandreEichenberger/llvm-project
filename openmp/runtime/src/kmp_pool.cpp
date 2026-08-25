#include <atomic>
#include <cassert>
#include <errno.h>
#include <functional>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#if USE_SIMPLE_MAP == 0
#include <unordered_map>
#endif

#include "kmp_debug.h"
#include "kmp_environment.h"
#include "kmp_pool.h"
#include "kmp_str.h"

/////////////////////////////////////////////////////////////////////////////////
// Local Symbols / Variables.
/////////////////////////////////////////////////////////////////////////////////

// Thread local's unique thread ID is used to retrieve the
// WorkerThreadPthreadType associated with a given task. This value is set to
// U when the worker thread W start to work on the routine R associated with the
// uniqueTid U. The field is reset to UNDEF once the routine R has completed its
// work. But the initial value is given EXTERNAL THREAD (so that any threads not
// in the pool have this value). The UNDEF/actual UniqueThreadId values are
// reserved for threads that actually entered the working pool.

thread_local WorkerThreadPthreadType threadSpecificUniqueTid =
    UNIQUE_TID_EXTERNAL_THREAD;

// Independent functions not tied to a class which are needed to implement
// functionality.
extern void *threadExitLoop(void *t) { return nullptr; };

/////////////////////////////////////////////////////////////////////////////////
// Debug and Helper functions
/////////////////////////////////////////////////////////////////////////////////

#define memoryFence() std::atomic_thread_fence(std::memory_order_seq_cst)

template <typename KEY, typename VALUE, int N, KEY EMPTY>
SimpleMap<KEY, VALUE, N, EMPTY>::SimpleMap() {
  init();
}

// Establish size/keys/values, then empty the map. Kept separate from the
// constructor because the enclosing ThreadPool is allocated with malloc: no
// constructor runs for it, so this is what makes the map usable. Must only be
// called on a fresh map, as it drops (rather than frees) any grown arrays.
template <typename KEY, typename VALUE, int N, KEY EMPTY>
void SimpleMap<KEY, VALUE, N, EMPTY>::init() {
  size = N;
  keys = &localKeys[0];
  values = &localValues[0];
  clear();
}

template <typename KEY, typename VALUE, int N, KEY EMPTY>
SimpleMap<KEY, VALUE, N, EMPTY>::~SimpleMap() {
  if (size > N) {
    // Was grown one or more times, free dynamic arrays.
    assert(keys != localKeys && "expected dynamic keys");
    assert(values != localValues && "expected dynamic values");
    free(keys);
    free(values);
  }
}

template <typename KEY, typename VALUE, int N, KEY EMPTY>
void SimpleMap<KEY, VALUE, N, EMPTY>::clear() {
  for (int64_t k = 0; k < size; ++k)
    keys[k] = EMPTY;
}

template <typename KEY, typename VALUE, int N, KEY EMPTY>
void SimpleMap<KEY, VALUE, N, EMPTY>::resize() {
  int64_t newSize = size + N;
  KEY *newKeys = (KEY *)malloc(newSize * sizeof(KEY));
  assert(newKeys && "failed to allocate new keys");
  VALUE *newValues = (VALUE *)malloc(newSize * sizeof(VALUE));
  assert(newValues && "failed to allocate new values");
  // Copy old keys & values.
  for (int64_t k = 0; k < size; ++k) {
    newKeys[k] = keys[k];
    newValues[k] = values[k];
  }
  // Reset new keys.
  for (int64_t k = size; k < newSize; ++k)
    newKeys[k] = EMPTY;
  // Establish new keys/values.
  if (size > N) {
    assert(keys != localKeys && "expected dynamic keys");
    assert(values != localValues && "expected dynamic values");
    free(keys);
    free(values);
  }
  size = newSize;
  keys = newKeys;
  values = newValues;
}

template <typename KEY, typename VALUE, int N, KEY EMPTY>
int64_t SimpleMap<KEY, VALUE, N, EMPTY>::count(KEY key) {
  int64_t n = 0;
  for (int64_t k = 0; k < size; ++k)
    if (keys[k] == key)
      ++n;
  return n;
}

template <typename KEY, typename VALUE, int N, KEY EMPTY>
void SimpleMap<KEY, VALUE, N, EMPTY>::add(KEY key, VALUE value) {
  for (int64_t k = 0; k < size; ++k)
    if (keys[k] == EMPTY) {
      keys[k] = key;
      values[k] = value;
      return;
    }
  // Not enough space, grow arrays and save in first new element.
  int64_t oldSize = size;
  resize();
  keys[oldSize] = key;
  values[oldSize] = value;
}

template <typename KEY, typename VALUE, int N, KEY EMPTY>
VALUE SimpleMap<KEY, VALUE, N, EMPTY>::erase(KEY key) {
  for (int64_t k = 0; k < size; ++k)
    if (keys[k] == key) {
      keys[k] = EMPTY;
      return values[k];
    }
  assert(false && "did not find key to remove");
}

template <typename KEY, typename VALUE, int N, KEY EMPTY>
VALUE SimpleMap<KEY, VALUE, N, EMPTY>::get(KEY key) {
  for (int64_t k = 0; k < size; ++k)
    if (keys[k] == key)
      return values[k];
  assert(false && "did not find key to remove");
}

///////////////////////////////////////////////////////////////////////////////
// WorkerThreadInfo methods
///////////////////////////////////////////////////////////////////////////////

void WorkerThreadInfo::init(int64_t gTid) {
  assert(gTid >= 0ll && "expected nonnegative global thread id");
  globalTid = gTid;
  uniqueTid = UNIQUE_TID_UNDEF;
  // uniqueTSidSeed set to tuple <0, gTid, 1>.
  uniqueTidSeed = ((uint64_t)gTid << 1) + 1ull;
  routine = nullptr;
  parameter = nullptr;
  detached = false;
  pthread_mutex_init(&mutexForJoin, nullptr);
  pthread_cond_init(&conditionForJoin, nullptr);
  pthread_mutex_init(&mutexForWorkerLoop, nullptr);
  pthread_cond_init(&conditionForWorkerLoop, nullptr);
  // init() because the structure may have been allocated with malloc only.
  uniqueTidToRoutineReturnValueMap.init();
  nativeThread = (pthread_t)0;
}

///////////////////////////////////////////////////////////////////////////////
// Id management and getters.

int64_t WorkerThreadInfo::getGlobalThreadId() { return globalTid; }

/*static */ int64_t
WorkerThreadInfo::getGlobalTid(WorkerThreadPthreadType uniqueTid) {
  // UniqueTid defined so that its lower bits encode the global thread
  // id associated with that uniqueTid.
  // Shift by one to get rid of the rightmost "1" bit, then take the mod.
  return (uniqueTid >> 1) % MAX_POOL_SIZE;
}

WorkerThreadPthreadType WorkerThreadInfo::getUniqueThreadId() {
  return uniqueTid;
}

WorkerThreadPthreadType WorkerThreadInfo::getNextUniqueTid() {
  // Increment by MAX_POOL_SIZE to preserve the property of uniqueTid.
  uniqueTidSeed += (2 * MAX_POOL_SIZE);
  assert(uniqueTidSeed != UNIQUE_TID_UNDEF && "expected defined uniqueTd");
  assert(getGlobalTid(uniqueTidSeed) == globalTid && "mapping issue");
  assert(isPoolThread((pthread_t)uniqueTidSeed) && "mapping issue");
  return uniqueTidSeed;
}

/* static */ bool WorkerThreadInfo::isPoolThread(pthread_t thread) {
  return ((uint64_t)thread) & 0x1ull;
}

/* static */ bool WorkerThreadInfo::isPoolThread() {
  return threadSpecificUniqueTid != UNIQUE_TID_EXTERNAL_THREAD;
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
  KA_TRACE(5, ("  %llu %lld setRoutine: Fct 0x%llx\n",
               (unsigned long long)newUniqueTid, (long long)globalTid,
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
  // ThreadPool::pthread_self() can find the right pthread id (namely a
  // uniqueTid).
  threadSpecificUniqueTid = uniqueTid;
  return routine(parameter);
}

// Reset the work descriptor to indicate that this thread is idle.
void WorkerThreadInfo::resetRoutine() {
  KA_TRACE(5, ("  %llu %lld resetRoutine: Fct 0x%llx\n",
               (unsigned long long)uniqueTid, (long long)globalTid,
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

pthread_t WorkerThreadInfo::getNativeThread() {
  assert(nativeThread != (pthread_t)0 &&
         "expected initialized WorkerThreadInfo");
  return nativeThread;
}

void WorkerThreadInfo::setNativeThread(pthread_t thread) {
  nativeThread = thread;
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
#if USE_SIMPLE_MAP
    uniqueTidToRoutineReturnValueMap.add(key, value);
#else
    uniqueTidToRoutineReturnValueMap[key] = value;
#endif
    KA_TRACE(10, ("  %llu %lld map: %llu -> %llu\n",
                  (unsigned long long)uniqueTid, (long long)globalTid,
                  (unsigned long long)key, (unsigned long long)value));
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
    KA_TRACE(10, ("  %llu %lld setDetached: success\n",
                  (unsigned long long)uniqueTid, (long long)globalTid));
    detached = true;
    failureCode = 0;
  } else {
    KA_TRACE(10, ("  %llu %lld setDetached: failure, now processing %llu\n",
                  (unsigned long long)detachingUTid, (long long)globalTid,
                  (unsigned long long)uniqueTid));
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
#if USE_SIMPLE_MAP
    uint64_t value = uniqueTidToRoutineReturnValueMap.erase(key);
#else
    uint64_t value = uniqueTidToRoutineReturnValueMap[key];
    uniqueTidToRoutineReturnValueMap.erase(key);
#endif
    KA_TRACE(10, ("  %llu %lld unmap: %llu -> %llu\n",
                  (unsigned long long)joiningUTid, (long long)globalTid,
                  (unsigned long long)key, (unsigned long long)value));
    *value_ptr = (void *)value;
  }
  rc = pthread_mutex_unlock(&mutexForJoin);
  assert(!rc && "failed to release join lock");
  return hasValue;
}

#if DEBUG
WorkerThreadRoutineType WorkerThreadInfo::getRoutine() { return routine; }
#endif

/* static */ int64_t WorkerThreadInfo::getGlobalTid(void *pthread) {
  return getGlobalTid((WorkerThreadPthreadType)pthread);
}

///////////////////////////////////////////////////////////////////////////////
// ThreadPool methods
///////////////////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////////////////
// Methods to construct and register threads into the pool.

void ThreadPool::init(int64_t n) {
  n = n < 0 ? MAX_POOL_SIZE : n; // negative (POOL_SIZE_DEFAULT) means max pool.
  maxPoolSize = n < MAX_POOL_SIZE ? n : MAX_POOL_SIZE; // cap at max pool.
  // Zero workers is a legal pool, not a mistake: it is what a thread limit of
  // one asks for, as that limit is spent entirely on the master. Every path
  // degrades correctly -- enterThreadPool rejects any volunteer as full,
  // pthread_create finds nothing available, and libomp requests no worker in
  // that configuration because it runs every region on the master alone.
  assert(maxPoolSize >= 0 && "expected a non-negative pool size");
  assert(__alignof__(pthread_t) > 1 &&
         "Support only implementations where pthread is not byte-aligned");
  // Init pool info with its global id.
  for (int64_t gTid = 0ll; gTid < maxPoolSize; ++gTid)
    pool[gTid].init(gTid);
  poolSize.store(0ll);
  threadBusyStatus.store(0ull);
  availableThreadNum.store(0ll);
  // Start out true for an empty pool, as no worker will ever run the
  // registration that sets this flag, yet every slot there is (none) is already
  // usable. Were it left false, pool_pthread_wait_until_fully_populated would
  // spin forever waiting for a worker that is not coming.
  allWorkersRegistered.store(maxPoolSize == 0);
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
    KA_TRACE(1, ("  0 %lld enterThreadPool: Thread pool is full.\n",
                 (long long)gTid));
    return EAGAIN;
  }
  assert(gTid >= 0 && "expected positive global thread id");
  // This thread now belong to the worker pool; so threadSpecificUniqueTid goes
  // from UNIQUE_TID_EXTERNAL_THREAD to UNIQUE_TID_UNDEF.
  assert(threadSpecificUniqueTid == UNIQUE_TID_EXTERNAL_THREAD &&
         "expected external thread here");
  threadSpecificUniqueTid = UNIQUE_TID_UNDEF;
  WorkerThreadInfo *threadInfo = &pool[gTid];
  pthread_t nativeThread = ::pthread_self(); // from pthread.h library.
  threadInfo->setNativeThread(nativeThread);
  memoryFence();
  // This thread's slot is now published, so this thread is registered. Until the
  // pool is fully populated nothing decrements availableThreadNum, so the count
  // returned here is the number of registered workers. Compare it against
  // maxPoolSize, as all maxPoolSize slots are filled by worker threads: the
  // master registers in neither mode, it only either creates the workers
  // (create_all_workers) or waits for them to volunteer. That is also why the
  // pool is sized at OMP_THREAD_LIMIT minus one, the master's share of the limit
  // (see init); this test keys off the number of slots either way.
  if (incrementNumberOfAvailableThreads() == maxPoolSize) {
    // Last worker to register, so the pool is now fully populated: release
    // whoever waits in pool_pthread_wait_until_fully_populated.
    memoryFence();
    allWorkersRegistered.store(true);
  }
  // Start waiting for tasks to be executed.
  KA_TRACE(10, ("  %lld enterThreadPool: start work loop\n", (long long)gTid));
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
  KA_TRACE(5, ("  0 %lld WorkerLoop: Stopping loop.\n", (long long)gTid));
  // Thread is exiting the worker loop, becomes an external thread again.
  threadInfo->resetRoutine();
  threadSpecificUniqueTid = UNIQUE_TID_EXTERNAL_THREAD;
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
    // Search for a free thread in the pool. Bound the scan by maxPoolSize and not
    // by poolSize alone: poolSize is claimed with a fetch_add that enterThreadPool
    // only afterwards rejects as full, so a process that donates more threads than
    // the pool holds leaves poolSize above maxPoolSize. Slots at or past
    // maxPoolSize were never initialized, and at MAX_POOL_SIZE would be past the
    // end of pool[], so scanning them could hand work to a thread that does not
    // exist. Over-donation is easy to arrive at by accident, as the pool holds one
    // fewer thread than the thread limit it was sized from (see init).
    int64_t size = poolSize.load();
    if (size > maxPoolSize)
      size = maxPoolSize;
    for (int64_t gTid = 0; gTid < size; ++gTid) {
      if (hasThreadFlippedToBusy(gTid)) {
        // First set the work descriptor associated with call.
        WorkerThreadPthreadType uTid =
            pool[gTid].setRoutine(start_routine, arg, detached);
        KA_TRACE(10, ("  %llu %lld pthread_create: Set fct %llx (%d iter)\n",
                      (unsigned long long)uTid, (long long)gTid,
                      (long long)start_routine, (int)attempt));
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
  KA_TRACE(1, ("  pthread_create: No threads available (%lld try, avail "
               "%lld out of %lld pool threads).\n",
               (long long)attempt, (long long)getNumberOfAvailableThreads(),
               (long long)poolSize.load()));
  *thread = (pthread_t)UNIQUE_TID_UNDEF; // Not necessary, just to keep tidy.
  return EAGAIN;
}

// Return the pthread_t, namely the uniqueTid associated with this
// thread.
pthread_t ThreadPool::pthread_self() {
  assert(WorkerThreadInfo::isPoolThread() && "expected pool worker thread");
  WorkerThreadPthreadType uTid = threadSpecificUniqueTid;
  assert(uTid != UNIQUE_TID_UNDEF && "unspecified unique thread id");
  return (pthread_t)uTid;
}

int ThreadPool::pthread_equal(pthread_t t1, pthread_t t2) {
  return ((WorkerThreadPthreadType)t1) == ((WorkerThreadPthreadType)t2);
}

int ThreadPool::pthread_detach(pthread_t thread) {
  assert(WorkerThreadInfo::isPoolThread(thread) &&
         "expected pool worker thread");
  WorkerThreadPthreadType uTid = (WorkerThreadPthreadType)thread;
  // Wait until thread is done with uTid.
  int64_t gTid = WorkerThreadInfo::getGlobalTid(uTid);
  if (!validGlobalTid(gTid)) {
    // Corrupted thread parameter., could not find the corresponding info.
    KA_TRACE(1, ("  %llu %lld pthread_detach: Could not find thread\n",
                 (unsigned long long)uTid, (long long)gTid));
    return ESRCH;
  }
  WorkerThreadInfo *threadInfo = &pool[gTid];
  int rc = threadInfo->setDetached(uTid);
  if (rc) {
    // Detach for a thread that completed just after we detached.
    KA_TRACE(1, ("  %llu %lld pthread_detach: Thread competed already.\n",
                 (unsigned long long)uTid, (long long)gTid));
    return ESRCH;
  }
  return 0; // Success.
}

int ThreadPool::pthread_join(pthread_t thread, void **value_ptr) {
  assert(value_ptr && "expected a nonnull value_ptr");
  assert(WorkerThreadInfo::isPoolThread(thread) &&
         "expected pool worker thread");
  // Simulated pthread_t thread value is uniqueTid.
  WorkerThreadPthreadType uTid = (WorkerThreadPthreadType)thread;
  // Wait until thread is done with uTid.
  int64_t gTid = WorkerThreadInfo::getGlobalTid(uTid);
  if (!validGlobalTid(gTid)) {
    // Corrupted thread parameter., could not find the corresponding info.
    KA_TRACE(1, ("  %llu %lld pthread_join: Could not find thread\n",
                 (unsigned long long)uTid, (long long)gTid));
    return ESRCH;
  }
  if (uTid == threadSpecificUniqueTid) {
    // A deadlock was detected or the value of thread specifies the calling
    // thread.
    KA_TRACE(1, ("  %llu %lld pthread_join: Calling join on self\n",
                 (unsigned long long)uTid, (long long)gTid));
    return EDEADLK;
  }

  // Wait for the worker thread to complete the task.
  KA_TRACE(10, ("  %llu %lld pthread_join: joining with pthread_t %llu\n",
                (unsigned long long)uTid, (long long)gTid,
                (unsigned long long)uTid));
  bool hasValue = pool[gTid].waitInJoin(uTid, value_ptr);
  if (!hasValue) {
    // Thread completed the work but did not leave a value_ptr, thus signaling
    // that this was not a joinable thread. Or that the thread was already
    // joined once and thus the map entry was already removed.
    KA_TRACE(1, ("  %llu %lld pthread_join: No return value\n",
                 (unsigned long long)uTid, (long long)gTid));
    return EINVAL;
  }
  KA_TRACE(10, ("  %llu %lld pthread_join: joined with pthread_t %lld\n",
                (unsigned long long)uTid, (long long)gTid,
                (unsigned long long)uTid));
  // Success.
  return 0;
}

// Snapshot on an upper bound on the number of available threads. Can change
// at anytime up (or possibly down).
int64_t ThreadPool::getThreadPoolSize() { return poolSize.load(); }
int64_t ThreadPool::getMaxThreadPoolSize() { return maxPoolSize; }
bool ThreadPool::areAllWorkersRegistered() {
  return allWorkersRegistered.load();
}

///////////////////////////////////////////////////////////////////////////////
// Support function for the worker loop function.

// Method used by the worker loop to indicate the completion of a routine.
void ThreadPool::callRoutineAndCleanup(WorkerThreadInfo *threadInfo) {
  // Call routine.
  KA_TRACE(10, ("  %llu %lld call&cleanup: Call 0x%llx with pthread %llu\n",
                (unsigned long long)threadInfo->getUniqueThreadId(),
                (long long)threadInfo->getGlobalThreadId(),
                (unsigned long long)threadInfo->getRoutine(),
                (unsigned long long)threadInfo->getUniqueThreadId()));
  void *value_ptr = threadInfo->callRoutine();
  KA_TRACE(10, ("  %llu %lld call&cleanup: Return 0x%llx with value %lld\n",
                (unsigned long long)threadInfo->getUniqueThreadId(),
                (long long)threadInfo->getGlobalThreadId(),
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
bool ThreadPool::hasThreadFlippedToBusy(int64_t gTid) {
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
  // Completion path of a dispatched routine, so the pool must be fully populated
  // by now. enterThreadPool relies on that to read availableThreadNum as a count
  // of registered workers, so check it rather than leave it unstated.
  assert(allWorkersRegistered.load() &&
         "work completed before the pool was fully populated");
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

int64_t ThreadPool::incrementNumberOfAvailableThreads() {
  // Use release consistency here because this protect stores above the call.
  int64_t newNum = availableThreadNum.fetch_add(1ll) + 1;
  assert(newNum <= poolSize.load() && "should be larger than pool size");
  return newNum;
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
// id into the native pthread_t of the actual pthread that
// performs/performed the work.
pthread_t ThreadPool::getNativeThread(pthread_t thread) {
  WorkerThreadPthreadType uTid = (WorkerThreadPthreadType)thread;
  int64_t gTid = WorkerThreadInfo::getGlobalTid(uTid);
  assert(validGlobalTid(gTid) && "invalid unique thread id");
  return pool[gTid].getNativeThread();
}

/////////////////////////////////////////////////////////////////////////////////
// Public Interface implementation (defined in kmp_pthread.h)
/////////////////////////////////////////////////////////////////////////////////

// Global variables for thread pool
static ThreadPool *threadPool = nullptr;
static pthread_mutex_t mutexThreadPoolInit = PTHREAD_MUTEX_INITIALIZER;

/////////////////////////////////////////////////////////////////////////////////
// Support

// Get thread limit from provided limit, or if <= 0 from env. Note that this is a
// thread limit and not a pool size: it counts the master, so the pool built from
// it holds one worker fewer (see init). Leaves threadLimit <= 0 when no value can
// be had, which init reads as "no limit was given". Call must be protected by a
// mutex.
static void getThreadLimit(int &threadLimit, const char *envVarName) {
  if (threadLimit <= 0 && envVarName) {
    char const *envVarValue = __kmp_env_get(envVarName);
    if (envVarValue != nullptr) {
      char const *msg = nullptr;
      kmp_uint64 value;
      __kmp_str_to_uint(envVarValue, &value, &msg);
      if (msg == nullptr) {
        KA_TRACE(5,
                 ("Get thread limit from env %s = %lld\n", envVarName, value));
        threadLimit = value;
      } else {
        KA_TRACE(1, ("Invalid thread limit from env %s: %s\n", envVarName,
                     envVarValue));
      }
      // __kmp_env_get hands back a copy that is ours to release.
      __kmp_env_free(&envVarValue);
    } else {
      KA_TRACE(5, ("Environment variable %s not set.\n", envVarName));
    }
  } else {
    KA_TRACE(1, ("Get thread limit argument: %d\n", threadLimit));
  }
}

// Set thread limit to the env. Call must be protected by a mutex.
static void setThreadLimit(int threadLimit, const char *envVarName) {
  assert(threadLimit > 0 && "expected a positive thread limit");
  assert(envVarName && "expected thread_limit or env_var");
  char varStr[20];
  snprintf(varStr, sizeof(varStr), "%d", threadLimit);
  __kmp_env_set(envVarName, varStr, 1);
  KA_TRACE(5, ("Set env var %s to %d\n", envVarName, threadLimit));
}

// Init protected by a mutex (when we actually have to do the init).
// Return true when we actually perform a new initialization.
bool init(int threadLimit, const char *envVarName) {
  // Test if initalized
  if (threadPool)
    return false;
  // Grab lock and test again.
  bool newInit = false;
  int rc = pthread_mutex_lock(&mutexThreadPoolInit);
  assert(!rc && "failed to acquire worker loop lock");
  if (!threadPool) {
    newInit = true;
    // Still not initialized, now initialize inside the lock. Build the pool
    // through a local, as threadPool is the variable that publishes it: the
    // fast path above reads threadPool without holding this mutex, so
    // threadPool must not become non-null before the pool is fully built.
    getThreadLimit(threadLimit, envVarName);
    ThreadPool *localThreadPool = (ThreadPool *)malloc(sizeof(ThreadPool));
    assert(localThreadPool && "failed to allocate thread pool");
    // The pool is one smaller than the thread limit, because the limit counts
    // the master and the pool holds only workers. libomp counts the master
    // towards OMP_THREAD_LIMIT: it initializes the thread count of the master's
    // contention group to one for the master alone and enforces
    // cg_nthreads + team_size <= cg_thread_limit, where team_size includes the
    // master too (kmp_runtime.cpp, __kmp_reserve_threads). The master meanwhile
    // registers in neither mode, it only creates the workers or waits for them to
    // volunteer. So a limit of N is exactly a master plus N-1 pool workers, and
    // sizing the pool at N would leave one worker parked for the whole run. A
    // limit we never learned (threadLimit <= 0) carries no N to subtract from, so
    // ask for the implementation maximum instead.
    localThreadPool->init(threadLimit > 0 ? threadLimit - 1 : POOL_SIZE_DEFAULT);
    int actualWorkers = localThreadPool->getMaxThreadPoolSize();
    // Publish the limit rather than the pool size, and derive it from the workers
    // we actually got rather than from what we asked for: init caps at
    // MAX_POOL_SIZE, so a larger request silently yields fewer workers, and
    // libomp has to be told the smaller number or it would form teams the pool
    // cannot staff. The +1 restores the master, i.e. the same convention the
    // value was read under, so that reading the variable back yields what we set.
    if (envVarName) // Has to set it because that is how OMP looks at it.
      setThreadLimit(actualWorkers + 1, envVarName);
    // Lead with the limit, the number everything outside this file speaks in; the
    // worker count is the internal half of the same statement.
    printf("Use thread pool with thread limit %d (%d workers plus the master)\n",
           actualWorkers + 1, actualWorkers);
    fflush(stdout);
    // Publish the fully built pool. The fence keeps every write above from
    // being reordered after the store below, so a thread that observes
    // threadPool non-null necessarily observes a fully built pool. The store
    // needs no atomic: it is a single aligned pointer-sized store, so no reader
    // can observe a torn value. Readers need no fence of their own either, but
    // only because they reach the pool's data exclusively through this pointer,
    // i.e. with an address dependency on this store. Reading pool state by a
    // route not derived from a load of threadPool, or spinning on threadPool
    // itself (a plain load, which the compiler may hoist out of the loop),
    // would need explicit acquire semantics instead.
    memoryFence();
    threadPool = localThreadPool;
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
// Exported functions: initialization of pool.

extern "C" bool pool_pthread_become_worker(int thread_limit,
                                           const char *env_var) {
  init(thread_limit, env_var);
  // Anything but success means the pool had no room for this thread, so say so
  // rather than return as if it had served: the caller is the only party that can
  // tell an intentional surplus from one thread too many. enterThreadPool has
  // already traced the rejection.
  return threadPool->enterThreadPool() == 0;
}

extern "C" int pool_pthread_create_all_workers(int thread_limit,
                                               const char *env_var) {
  bool newInit = init(thread_limit, env_var);
  // Workers, not the thread limit: the pool holds one fewer thread than the limit
  // and this loop creates the pool's threads, not the master.
  int numWorkers = threadPool->getMaxThreadPoolSize();
  if (newInit) {
    // Create the actual pthread and register them to the pool.
    for (int64_t i = 0; i < numWorkers; ++i) {
      pthread_t thread;
      int rc = pthread_create(&thread, nullptr, registerWorker, (void *)i);
      assert(!rc && "Pthread pool creation failure");
    }
  }
  // Report a thread limit, as the pool size is ours alone to know: add back the
  // master that the pool does not hold.
  return numWorkers + 1;
}

extern "C" int pool_pthread_wait_until_fully_populated(int thread_limit,
                                                       const char *env_var) {
  // Initialize the pool rather than assume someone else already did. In the
  // donation mode, where workers volunteer themselves through
  // pool_pthread_become_worker, this thread may well get here before any
  // volunteer has arrived, so there may be no pool to wait on yet. Whether we
  // win this init or a volunteer already did, threadPool is non-null and fully
  // built when init() returns, so waiting below is safe and the pool size is
  // known.
  init(thread_limit, env_var);
  assert(threadPool && "expected an initialized thread pool");
  // At this time, use busy waiting. Wait on the "all workers registered" flag and
  // not on the pool size: poolSize is incremented on entry to enterThreadPool,
  // before a worker publishes its slot, so waiting on it would let this thread
  // proceed while the last slot is still unusable. Note that if fewer workers
  // than the pool size ever register this waits forever, as it did before.
  while (!threadPool->areAllWorkersRegistered()) {
    // Busy waiting, could update to something else.
    // hi alex std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  // Report a thread limit, as the pool size is ours alone to know: add back the
  // master that the pool does not hold.
  return threadPool->getMaxThreadPoolSize() + 1;
}

/////////////////////////////////////////////////////////////////////////////////
// Exported functions: simulating pthreads.

// Fully implemented.

extern "C" pthread_t getNativeThreadButAssertIfPoolThread(pthread_t thread) {
  assert(!WorkerThreadInfo::isPoolThread(thread) &&
         "cannot handle pool thread call");
  return thread;
}

extern "C" pthread_t getPoolNativeThread(pthread_t thread) {
  if (!WorkerThreadInfo::isPoolThread(thread))
    return thread; // Have native thread, "thread" is native.
  assert(threadPool && "uninitialized thread pool");
  return threadPool->getNativeThread(thread);
}

extern "C" int pool_pthread_create(pthread_t *thread,
                                   const pthread_attr_t *attr,
                                   void *(*start_routine)(void *), void *arg) {
  assert(threadPool && "uninitialized thread pool");
  return threadPool->pthread_create(thread, attr, start_routine, arg);
}

extern "C" pthread_t pool_pthread_self() {
  if (!WorkerThreadInfo::isPoolThread())
    return pthread_self();
  assert(threadPool && "uninitialized thread pool");
  return threadPool->pthread_self();
}

extern "C" int pool_pthread_equal(pthread_t t1, pthread_t t2) {
  bool isPoolThread1 = WorkerThreadInfo::isPoolThread(t1);
  bool isPoolThread2 = WorkerThreadInfo::isPoolThread(t2);
  if (!isPoolThread1 && !isPoolThread2)
    return pthread_equal(t1, t2);
  if (isPoolThread1 && isPoolThread2) {
    assert(threadPool && "uninitialized thread pool");
    return threadPool->pthread_equal(t1, t2);
  }
  // We have one thread pool, one not. Could assert, or just state they are
  // different, or compare their native threads. Just state false for now.
  return false;
}

extern "C" int pool_pthread_detach(pthread_t thread) {
  if (!WorkerThreadInfo::isPoolThread(thread))
    return pthread_detach(thread);
  assert(threadPool && "uninitialized thread pool");
  return threadPool->pthread_detach(thread);
}

extern "C" int pool_pthread_join(pthread_t thread, void **value_ptr) {
  if (!WorkerThreadInfo::isPoolThread(thread))
    return pthread_join(thread, value_ptr);
  assert(threadPool && "uninitialized thread pool");
  return threadPool->pthread_join(thread, value_ptr);
}

// Unimplemented.

extern "C" void pool_pthread_exit(void *value_ptr) {
  if (!WorkerThreadInfo::isPoolThread()) {
    pthread_exit(value_ptr);
  }
  assert(false && "pool pthread_exit is not implemented");
}

extern "C" int pool_pthread_cancel(pthread_t thread) {
  if (!WorkerThreadInfo::isPoolThread(thread))
    return pthread_cancel(thread);
  assert(false && "pool pthread_cancel is not implemented");
}

// Pass through.

extern "C" int pool_pthread_getschedparam(pthread_t thread, int *policy,
                                          struct sched_param *param) {
  return pthread_getschedparam(getPoolNativeThread(thread), policy, param);
}

extern "C" int pool_pthread_setschedparam(pthread_t thread, int policy,
                                          const struct sched_param *param) {
  return pthread_setschedparam(getPoolNativeThread(thread), policy, param);
}
