.. _donated_threads:

Provisioning Workers From Donated Threads
=========================================

.. contents::
   :local:

Overview
--------

When ``libomp`` is built with ``LIBOMP_USE_DONATED_THREADS=ON``, it never calls
``pthread_create``. The application donates threads it already owns by calling
``kmp_donate_thread`` from each one; a donated thread parks until the runtime
asks for a worker, then serves as one until the runtime shuts down.

This is for hosts that cap thread creation administratively and hand an
application a fixed thread budget instead of letting it ask for more. On such a
host ``pthread_create`` returns ``EAGAIN``, which ``libomp`` treats as fatal, so
a runtime that cannot create threads is the difference between running and not
running at all.

The feature is a third implementation of an interface ``libomp`` already has:
``__kmp_create_worker`` and ``__kmp_reap_worker``, which already exist separately
for Unix and for Windows. Nothing above that boundary is aware of it.

The option is offered on Linux, macOS and z/OS and defaults to ``OFF``.
Requesting it on any other platform is a configure-time error. The
implementation file is compiled everywhere regardless, with its body under ``#if
KMP_USE_DONATED_THREADS`` and an ``#else`` stub, so ``kmp_donate_thread`` is
declared in ``omp.h`` and exported unconditionally on every platform: the ABI
does not vary by configuration.

Using it
--------

.. code-block:: c

   #include <omp.h>

   /* Contribute the calling thread to the OpenMP runtime, to be used as a
      worker. Does not return until the runtime shuts down.

      Returns 0 on success, ENOMEM if no donation slot is left, and ENOSYS if
      this build does not provision workers from donated threads. */
   extern int kmp_donate_thread(void);

The contract for the application is one line:

  Set ``KMP_DEVICE_THREAD_LIMIT`` to ``1 + K``, where ``K`` is the number of
  threads you will donate, then donate them.

The ``1`` is the thread that encounters the parallel region: it runs the region
too, so it does not need a donor. With ``R`` such threads entering OpenMP,
donate ``K = limit - R`` -- extra ones consume budget but create nothing.

Donate before the first OpenMP call, and donate all of them. A donated thread
touches no runtime state before it parks, so it may donate itself before the
runtime has initialized; but the runtime sizes itself once, from what has been
donated by then. Donations that arrive later are not used.

No other configuration is required. Two settings are worth stating, though:

* ``KMP_BLOCKTIME=infinite`` must not be used. It would leave every donated
  thread spinning for the life of the process. The default of 200 ms is what
  makes a parked donor cost memory rather than cycles.
* ``KMP_INIT_AT_FORK=FALSE`` is recommended, because ``fork`` is not supported
  (see below).

What is supported
-----------------

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Feature
     - Note
   * - ``parallel``, nested ``parallel``, ``for``, ``sections``, ``single``
     - Above the thread layer; unaffected.
   * - Tasks, ``taskloop``, ``taskgroup``, dependences
     - Unaffected. Hidden helper tasks run on the encountering thread instead of
       on a hidden helper team.
   * - Barriers, reductions, locks, atomics
     - Unaffected.
   * - Hot teams and the runtime's internal thread pool
     - Unaffected, and they *reduce* demand: a released worker returns to the
       runtime's free list and is reused without another donation.
   * - ``omp_set_num_threads``, ``num_threads``
     - Honoured, then clamped by ``KMP_DEVICE_THREAD_LIMIT``.
   * - Multiple application threads entering OpenMP
     - Supported. The obligation is arithmetic only: donate ``limit - R``.
   * - ``teams`` / league
     - Supported. ``OMP_THREAD_LIMIT`` restarts per league primary, but the
       device limit still binds all of them together.
   * - OMPT, OMPD, debuggers
     - ``ds_thread`` holds a real ``pthread_t``, so these see true thread
       identities.
   * - ``pthread_self()`` inside the runtime
     - Always truthful. There is no restriction on which thread may encounter a
       parallel region.

What is not supported
---------------------

Each of these is a deliberate simplification, not an omission.

.. list-table::
   :header-rows: 1
   :widths: 28 34 38

   * - Not supported
     - Why
     - Behavior at the boundary
   * - Creating any OS thread
     - That is the premise.
     - With the feature on, the library contains no call to ``pthread_create``
       on the worker path.
   * - Hidden helper threads
     - They need OS threads, and on Linux they default to eight of them.
     - Forced off during initialization, regardless of
       ``LIBOMP_USE_HIDDEN_HELPER_TASK`` or
       ``LIBOMP_NUM_HIDDEN_HELPER_THREADS``. Hidden helper tasks still execute,
       on the encountering thread.
   * - Returning a donated thread *early*
     - The runtime has no per-thread exit: its worker loop ends only when the
       whole runtime is shutting down.
     - ``kmp_donate_thread`` does not return until then. The application must
       not terminate a donated thread while it is still serving -- doing so runs
       the thread-specific-data destructor from inside the runtime's own worker
       loop. Once ``kmp_donate_thread`` **has** returned the thread carries none
       of the runtime's per-thread state and may be exited, joined or reused
       freely; see `Giving the thread back`_.
   * - Re-initializing the runtime and continuing in parallel
     - Reached by ``omp_pause_resource(omp_pause_hard)``, by ``__kmpc_end``
       under ``KMP_IGNORE_MPPEND=0``, or by the library destructor. Every
       donated thread is released at once and none are re-donated.
     - The next generation runs correctly but **serially**, with one warning.
       Not an abort.
   * - Using OpenMP in a ``fork()`` child
     - Only the forking thread survives, so the table names threads that no
       longer exist.
     - Two configurations, and both are closed. By default ``libomp`` registers
       ``fork`` handlers: they quiesce the table's lock across the ``fork`` and
       mark every donation unusable in the child, so the child refuses further
       donations and its regions serialize, with one warning. Under
       ``KMP_INIT_AT_FORK=FALSE`` there are no handlers at all, and a child must
       not touch OpenMP -- which is already true independently of this feature,
       because the child inherits a runtime that believes it is initialized.
   * - ``OMP_STACKSIZE`` / ``KMP_STACKSIZE``
     - A donated thread arrives with the stack its application gave it.
     - One-time warning if set. The runtime still *reports* the true stack,
       because it inspects it on the donated thread itself -- but reporting a
       stack is not sizing one. Measure the donor's stack against what the
       OpenMP regions need; a shortfall appears as a crash inside OpenMP code
       rather than as an error.
   * - More than ``KMP_MAX_DONATED_THREADS`` donations
     - The table is a fixed, statically allocated array.
     - ``kmp_donate_thread`` returns ``ENOMEM``; the caller decides.
   * - Donating threads on Windows
     - The feature needs POSIX threads. Windows itself keeps working unchanged,
       with the feature compiled out.
     - ``LIBOMP_USE_DONATED_THREADS`` is refused at configure time;
       ``kmp_donate_thread`` returns ``ENOSYS``.
   * - A Fortran spelling of ``kmp_donate_thread``
     - Not needed yet. The entry point takes no arguments, which is what keeps
       adding one cheap later.
     - C and C++ only.

Giving the thread back
----------------------

A ``pthread_create``\ d worker never had to give anything back: it died, and the
OS reclaimed everything the runtime had put on it. A donated thread returns to
the application instead, so ``kmp_donate_thread`` restores what serving as a
worker installed before it returns:

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Installed while serving
     - On return
   * - The gtid, in the thread-specific key whose destructor re-enters the
       runtime
     - Put back to whatever the thread had -- unset, for a donor that had never
       made an OpenMP call, which is the case that stops the destructor from
       running at all
   * - The thread's affinity mask
     - Restored. A mask that cannot be read or written is left alone rather than
       failing the donation
   * - x87 control word and ``MXCSR``
     - Restored. The x87 *status* word is not: it has no store/load pair, and
       the SSE exception flags come back with ``MXCSR``
   * - Asynchronous cancellation
     - Never installed on a donated thread in the first place

The restore happens while the runtime is still up, before the thread is reported
gone, because both the key and the affinity allocator stop working once teardown
has finished with them.

A thread that has been handed back is therefore an ordinary application thread
again: it may exit, be joined, be reused for something else, or make OpenMP calls
of its own, in which case it registers as a fresh root. What it may not do is
donate itself again -- a donation is used at most once, and later ones are
refused with ``EAGAIN``.

When the budget is short
------------------------

A shortfall degrades rather than aborts. This matters because on the platforms
this feature targets, the remedy for a shortfall is an administrative change and
a reboot, so aborting would turn a capacity problem into an outage.

Initialization sizes the device limit to what the application has actually
donated. A request for more threads than that is clamped, exactly as
``KMP_DEVICE_THREAD_LIMIT`` is normally clamped, and the region runs with a
smaller team. If nothing is available at all -- which is the state after a
runtime teardown -- the limit becomes 1 and regions serialize. In every case the
program keeps running and keeps producing correct results; it simply stops
running in parallel.

Two limits on that:

* ``num_threads(N) strict`` turns any shortfall into a hard error by design, so
  a strict region still fails loudly. That is what ``strict`` means.
* The clamp is one-way and process-wide. Once narrowed, the process does not
  widen again.

The one case initialization cannot judge is a program that has donated *nothing*
when the runtime initializes, because "will donate shortly" and "will never
donate" look identical. There, the runtime waits for a donation at fork time,
and gives up with a diagnostic naming both counts -- how many workers were
requested and how many threads were donated -- if none arrives.

Build configuration
-------------------

.. code-block:: console

   $ cmake -DLIBOMP_USE_DONATED_THREADS=ON ...

``KMP_MAX_DONATED_THREADS`` (default 256) sets the table's capacity. Every entry
is allocated whether used or not, so this is a memory-versus-ceiling trade of
about 130 to 150 bytes per entry, depending on how large the platform's
``pthread_mutex_t`` and ``pthread_cond_t`` are: an entry carries one of each, so
that the runtime can wake one donated thread without disturbing the others.

What the feature asks of a platform
-----------------------------------

Only POSIX threads, and only the part of them that every conforming
implementation has. ``KMP_USE_DONATED_THREADS`` is therefore spelled as
``KMP_OS_UNIX`` rather than as a list of operating systems, so a new port brings
the feature with it. The constraints that keep that true are worth naming,
because each of them is a place where the obvious code would not have been
portable:

* ``pthread_t`` is never compared, printed, or used as a key. A donated thread's
  slot is found by the identity of the ``kmp_info_t *`` it was handed to. On
  z/OS ``pthread_t`` is a structure, so ``==`` would not compile and
  ``pthread_equal`` would not be usable in the scan.
* The wait for donations is taken against ``gettimeofday``, the clock
  ``pthread_cond_timedwait`` uses when it cannot be told otherwise.
  ``pthread_condattr_setclock`` is absent on both Darwin and z/OS, and
  ``KMP_NOW`` is the timestamp counter rather than a wall clock on x86. The wait
  is sliced so that a step of the realtime clock costs one slice rather than the
  whole budget.
* ``struct timespec`` is filled in field by field. POSIX fixes the names of
  ``tv_sec`` and ``tv_nsec`` but neither their order nor that they are the only
  members, so an aggregate initializer would rely on a layout no standard
  promises.
* Asynchronous cancellation is not enabled on a donated thread. This is required
  anyway -- the thread goes back to the application and must not carry the
  runtime's cancel state with it -- but it also means the feature does not need
  ``PTHREAD_CANCEL_ASYNCHRONOUS`` to work as it does on Linux.
* The saved-and-restored per-thread state is the ``gtid`` and, on x86 only, the
  x87 control word and ``MXCSR``. Nothing else is architecture-specific, and the
  x86 half is under ``KMP_ARCH_X86 || KMP_ARCH_X86_64``.
* ``<pthread.h>`` is reached through ``kmp.h``, which is included first
  everywhere. Platforms that gate their thread declarations behind feature-test
  macros -- z/OS behind ``_XOPEN_SOURCE`` -- then need those macros set once, by
  the build, and not once per include site.

Notes for the curious
---------------------

The table needs nothing done to it before use, so it lives in ``.bss`` and has no
initialization step at all: an unclaimed entry is never read, and a claimed one
was made ready by the thread that claimed it. That is what lets a thread donate
itself before the runtime's own constructor has run, and it is why the feature
needs no ordering contract with the application. It is also why an entry's mutex
is created by its donor rather than written out as a static initializer per
entry, which on macOS -- where an all-zero ``pthread_mutex_t`` is not a valid one
-- would move the whole table out of ``.bss`` and into the binary.

A request is matched to a donation by scanning the table for the first parked
entry, and a worker being reaped is matched back to its own entry by the identity
of the ``kmp_info_t *`` it was handed -- never by comparing ``pthread_t``, which
is a structure on z/OS. One counter serves both scans and allocates the next
entry: it records how many entries have ever been claimed and only ever
increases, so the claimed entries are exactly the first *n*. No entry is ever
reused and only a parked one is handed out, so a donation serves at most one
request.

Locking is in two levels, and nothing holds one while taking the other. One mutex
covers the table -- that counter, each entry's state, and the announcement that a
donation has arrived. Beyond that, each donated thread has its own mutex and
condition variable, and they carry the two handoffs that concern only it: the
work handed to it, and the return value it hands back at shutdown. So donating,
requesting a worker, waking a specific donated thread, and waiting for one to
come back contend with each other only where they genuinely touch the same
words, and a donated thread is woken by name rather than by a broadcast that
every other parked donor has to wake up and dismiss.

The table's mutex is the innermost lock in the runtime: anything may be held
while taking it, and nothing is ever taken while holding it. That is what lets
``fork`` handle it the same way it already handles ``libomp``'s two bootstrap
locks -- acquired before the ``fork`` and released on both sides -- so the child
inherits it in a known state rather than possibly held by a thread that did not
survive. Every piece of table state is then guarded, with one exception: the
flag that records whether this generation's budget has been settled is read on
every ``parallel`` region, so it is read without the lock and re-tested under
it.
