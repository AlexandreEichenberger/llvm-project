#ifndef THREAD_POOL_HPP
#define THREAD_POOL_HPP 1

#include <atomic>
#include <functional>
#include <pthread.h>
#include <stdint.h>

/////////////////////////////////////////////////////////////////////////////////
// Simple Map.
/////////////////////////////////////////////////////////////////////////////////

#define USE_SIMPLE_MAP 1
#if USE_SIMPLE_MAP
// Because of linking errors with unordered map, implemented a simple one here.
template <typename KEY, typename VALUE, int N, KEY EMPTY> struct SimpleMap {
  SimpleMap();
  ~SimpleMap();
  void clear();
  int64_t count(KEY key);
  void add(KEY key, VALUE value); // Assert when map is full.
  VALUE erase(KEY key); // Return erased value; assert if not found.
  VALUE get(KEY key); // Assert if not found.
private:
  void resize();

  int64_t size;
  KEY *keys;
  VALUE *values;
  // Cached array for normal (non-growing) cases.
  KEY localKeys[N];
  VALUE localValues[N];
};
#endif

/////////////////////////////////////////////////////////////////////////////////
// Global types.
/////////////////////////////////////////////////////////////////////////////////

using WorkerThreadRoutineType = void *(*)(void *);
using WorkerThreadArgType = void *;
using WorkerThreadPthreadType = uint64_t;

/////////////////////////////////////////////////////////////////////////////////
// Info kept for each thread in the thread pool.
/////////////////////////////////////////////////////////////////////////////////

// Maximum pool size, so that we may use static allocation of thread pool
// info. Largest suported value is currently 64, as we use a single 64
// bitvector to indicate the available/busy status of each thread in the pool.
#define MAX_POOL_SIZE 64ll

// Global thread id are in the range [0..MAX_POOL_SIZE) at most; undefined
// value is set to -1
#define GLOBAL_TID_UNDEF -1ll
// Unique thread id will start at 2* MAX_POOL_SIZE; so all numbers between 0 and
// 2*MAX_POOL_SIZE-1 can be used for special values. UNDEF is used when a given
// pool thread is not working. EXTERNAL_THREAD is used for any threads that do
// not belong to the working pool. Lowest bit should be set to identify this as
// a worker pool id.
#define UNIQUE_TID_UNDEF ((WorkerThreadPthreadType)1)
#define UNIQUE_TID_EXTERNAL_THREAD ((WorkerThreadPthreadType)3)

struct ThreadPool;

struct WorkerThreadInfo {
  WorkerThreadInfo() = default;
  WorkerThreadInfo(int64_t gTid) { init(gTid); };
  void init(int64_t gTid);

  // Global thread id for this specific instance.
  int64_t getGlobalThreadId();
  // Global thread id for a given uniqueTid (static method).
  static int64_t getGlobalTid(WorkerThreadPthreadType uniqueTid);

  // Unique thread Id.
  WorkerThreadPthreadType getUniqueThreadId();
  WorkerThreadPthreadType getNextUniqueTid();
  static bool isPoolThread(); // Is self a pool thread or not.
  static bool isPoolThread(pthread_t thread); // Is thread a pool thread or not.

  // Called by the parent thread P who detected an idle worker thread W and want
  // to initiate a new routine for thread W.
  WorkerThreadPthreadType setRoutine(WorkerThreadRoutineType newRoutine,
                                     WorkerThreadArgType newParameter,
                                     bool newDetached);
  // Call the routine to execute work.
  void *callRoutine();
  // Reset the work descriptor after work is completed.
  void resetRoutine();

  // Status & info.
  bool isIdle();
  bool isTerminated();
  void setTerminated();
  bool isDetached();
  pthread_t getNativeThread();
  void setNativeThread(pthread_t thread);

  // Synchronization methods for getting new work.
  void setUniqueTidAndSignalWorkerLoop(WorkerThreadPthreadType newUniqueTid);
  void waitInWorkerLoop();
  // Synchronization methods for providing result of subroutine.
  void signalForJoinAndResetRoutine(ThreadPool *threadPool, void *value_ptr);
  int setDetached(WorkerThreadPthreadType detachingUTid);
  bool waitInJoin(WorkerThreadPthreadType joiningUTid, void **value_ptr);

  // Debug.
  WorkerThreadRoutineType getRoutine();
  static int64_t getGlobalTid(void *pthread);

private:
  ///////////////////////////////////////////////////////////////////////////////
  // Private Data

  // Global thread id (globalTid) is used to uniquely identify a worker thread
  // in the worker pool. It has a value is in range [0..poolSize). Set in its
  // constructor, it then the remain constant for the duration of the object's
  // lifetime.
  int64_t globalTid = GLOBAL_TID_UNDEF;

  // The Unique Thread Id (uniqueTid) value uniquely identify a routine created
  // by pthread_create. Its value is unique and never reused. A uniqueTid is
  // constructed so as to embed the globalID that was assigned to that routine
  // in it. See uniqueTidSeed, getNextUniqueTid() and getGlobalTid(uniqueTid)
  // for details on the mapping.
  //
  // This value is set when a parent thread is setting the work for an idle
  // thread (pthread_create calling setRoutine). It is reset when the work is
  // completed by the working thread (Worker Loop calling resetRoutine).
  //
  // UniqueTid is a syncronization variable between threads. Namely
  // UniqueTid U associated with routine R is used by a joining thread J
  // to determine if the worker thread W is still working on completing R or
  // not. The joining thread J (executing pthread_join) needs to determine if
  // thread W has completed R. J knows (by looking at uniqueTid U) which thread
  // was assigned to execute R, namely thread W. If W's uniqueTid is still U,
  // this mean the work is still ongoing. If W's uniqueTid is not U, then W is
  // either idle (UNDEF) or is working on a new routine associated
  // with a distinct uniqueTid. Thus in these latter both cases, J will know
  // that the routine R is completed.
  //
  // The unique thread id internal fields are as follow:
  //
  // <unique number : global thread id : 1>
  //
  // where:
  // o  Unique number is a per-worker unique id.
  // o  Global thread id is the id of this thread, so that we may easily
  //    identify the worker associated with a given unique thread id.
  // o  Rightmost bit set to "1" to identify this thread_t as a pool pthread_t.
  //    Because thread_t is an opaque pointer to a "struct pthread" data
  //    structure, and that struct pthread must be naturally aligned to at least
  //    8 bytes (and often more, like 32 bytes), we know that the opaque address
  //    returned as pthread_t must have its lower 8 bit set to zero. Thus by
  //    having a "1" in the lowest bit of every uniqueTid, we can unambiguously
  //    determine that a given pthread_t is a native (lowest bit 0) or a pool
  //    unique thread id (lowest bit 1).
  //
  // Init: In constructor to UNDEF.
  // Write:
  // o In setUniqueTidAndSignalWorkerLoop to indicate that a new routine has
  //   been assigned to this thread. This is called in the context of a
  //   pthread_create.
  // o In resetRoutine (value UNDEF) to indicate that the routine has been
  //   completed. This is called in the context of a worker loop.
  // Read: In many places, but most significantly in waitInJoin to determine if
  //   the return value of the routine is ready to be consumed. This is executed
  //   in the context of a pthread_join.
  volatile WorkerThreadPthreadType uniqueTid = UNIQUE_TID_UNDEF;

  // Seed for the next uniqueTid; seeded with globalTid and incremented
  // by a multiple MAX_POOL_SIZE so that effectively each thread in the pool
  // assigns unique numbers without conflicts.
  //
  // Init: In constructor to globalTid.
  // Read/Write: In getNewUniqueThreadId/pthread_create (by multiple threads).
  //   Since only one thread is guaranteed to see another thread as free,
  //   only one thread at a time can update that value; thus no atomics are
  //   needed here.
  WorkerThreadPthreadType uniqueTidSeed = UNIQUE_TID_UNDEF;

  // The routine variable contains the function pointer and argument of the work
  // to be done by a given thread. It is set during when a parent thread is
  // setting the work (pthread_create calling SetRoutine).
  WorkerThreadRoutineType routine = nullptr;
  WorkerThreadArgType parameter = nullptr;

  volatile bool detached = false;

  // Synchronization for pthread_join.
  pthread_mutex_t mutexForJoin = PTHREAD_MUTEX_INITIALIZER;
  pthread_cond_t conditionForJoin = PTHREAD_COND_INITIALIZER;

  // Synchronization for pthread_create / worker loop
  pthread_mutex_t mutexForWorkerLoop = PTHREAD_MUTEX_INITIALIZER;
  pthread_cond_t conditionForWorkerLoop = PTHREAD_COND_INITIALIZER;
#if USE_SIMPLE_MAP
  SimpleMap<uint64_t, uint64_t, MAX_POOL_SIZE, 0>
      uniqueTidToRoutineReturnValueMap;
#else
  std::unordered_map<uint64_t, uint64_t> uniqueTidToRoutineReturnValueMap;
#endif

  // Pthread_t value of this actual thread (standard pthread_self value)
  pthread_t nativeThread = (pthread_t)0;
};

/////////////////////////////////////////////////////////////////////////////////
// Thread pool data structure (one per pool).
/////////////////////////////////////////////////////////////////////////////////

struct ThreadPool {
  ThreadPool(int64_t n = 0 /* <=0 default to MAX_POOL_SIZE */) { init(n); }
  void init(int64_t n = 0 /* <=0 default to MAX_POOL_SIZE */);

  // Registering a thread into the pool, get current pool size.
  int enterThreadPool();
  int64_t getThreadPoolSize();
  int64_t getMaxThreadPoolSize();

  // Pthread interface.
  int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                     void *(*start_routine)(void *), void *arg);
  pthread_t pthread_self();
  int pthread_equal(pthread_t t1, pthread_t t2);
  int pthread_detach(pthread_t thread);
  int pthread_join(pthread_t thread, void **value_ptr);

  // Infra
  void setThreadAvailable(int64_t gTid); // Releasing a thread.
  pthread_t getNativeThread(pthread_t thread); // Translate pthread_t.

private:
  void callRoutineAndCleanup(WorkerThreadInfo *threadInfo);
  bool hasThreadFlippedToBusy(int64_t gTid);
  int64_t getNumberOfAvailableThreads();
  void incrementNumberOfAvailableThreads();
  void decrementNumberOfAvailableThreads();
  bool validGlobalTid(int64_t gTid);

  ///////////////////////////////////////////////////////////////////////////////
  // Data

  // Maximum number of threads in the pool, set during constructor. Must be
  // smaller than MAX_POOL_SIZE (64) because we are using a single 64-bit
  // status at this time.
  //
  // Write: By constructor (once, by a single thread).
  // Read: By registerThread (by multiple threads, after constructor).
  int64_t maxPoolSize = 0;

  // Number of threads that have currently registered in the pool. Can have up
  // to pool size threads.
  //
  // Init: 0 in constructor.
  // Read/Write: By registerThread (incremented, by multiple threads).
  // Read: In pthread_create to scan available threads, getThreadPoolSize, and
  //   debug/asserts (by multiple threads).
  std::atomic_int64_t poolSize;

  // Each bit t (t<poolSize) in threadBusyStatus represents whether a pool
  // thread with gTid == t is available (bit == 0) or busy (bit == 1). A given
  // bit is set to 1 when pthread_create found a free thread. A given bit is
  // reset to 0 when the task assigned to that thread is completed. Reset must
  // occur after all bookkeeping with the terminating task is complete, as this
  // is the synchronization variable that is looked at before reassigning a
  // thread to a given task.
  //
  // Init: 0 in constructor (once by a single thread).
  // Read/Write:
  // o  In pthread_create to find a free thread (fetch_or by multiple threads).
  // o  In registerRoutineCompletion to signal that the thread is available for
  //    new tasks (fetch_xor by multiple threads).
  std::atomic_uint64_t threadBusyStatus; /* init 0 */

  // Number of available threads for new tasks. Keep track of bits that are 0 in
  // threadBusyStatus. Since we are using individual atomic operations,
  // availableThreadNum may be temporarily smaller than the actual number of
  // free thread in the pool as indicated by threadBusyStatus.
  //
  // Init: 0 in constructor (once by a single thread).
  // Read/Write:
  // o Incremented in registerThread and setThreadAvailable (multiple threads).
  // o Decremented in hasThreadFlippedToBusy/pthread_create (multiple threads).
  // Read: In pthread_create to determine if threads are available (multiple
  //   threads).
  std::atomic_int64_t availableThreadNum; /* init 0 */

  // The actual thread pool info data structure (with one entry per thread).
  WorkerThreadInfo pool[MAX_POOL_SIZE];
};

#endif
