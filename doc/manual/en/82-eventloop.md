\newpage

## Event Loop

Here you find a concise developer-focused description of how the event loop implemented in `ev.c` and `ev.h` operates. This includes architecture, backends, watcher mechanics, lifecycle, role separation, callback pipelines, and planned enhancements.

**High-Level Architecture**

The event loop sits in a continuous wait cycle, monitoring sources of events (I/O readiness, timer expirations, or delivered signals), dispatching the appropriate handler when an event occurs, and calling a user-registered callback.

* **Backends** abstract the OS-specific wait or asynchronous I/O mechanism used. Supported backends include `io_uring`, `epoll`, and `kqueue`.
* **Watchers** encapsulate interest in one type of event (I/O, signal, or periodic) and carry the callback and context required.
* **Lifecycle routines** manage setup, execution, interruption, and teardown of the loop.
* **Process isolation**: Each process has its own event loop, ensuring a clear distinction between the main process (accepting connections and handling periodic timers/signals) and worker processes (handling individual client sessions).

**Configuration**

The event loop backend can be selected in `pgmoneta.conf` under the `[pgmoneta]` section using the `ev_backend` setting:

```ini
ev_backend = auto
```

Supported options:

* `auto`: Automatically select the platform default backend for the operating system (default). On Linux, selects `epoll`. On BSD/macOS systems, selects `kqueue`. `io_uring` is an opt-in backend that must be explicitly configured with `ev_backend = io_uring`.
* `io_uring`: Linux asynchronous I/O interface using kernel submission and completion queues via `liburing >= 2.5`. This is an opt-in backend. If the kernel does not support the required operations, `pgmoneta` falls back to `epoll`.
* `epoll`: Standard Linux I/O event notification facility via `epoll_create1(2)` and `epoll_wait(2)`.
* `kqueue`: Scalable kernel event notification mechanism available on FreeBSD, OpenBSD, and macOS via `kqueue(2)` and `kevent(2)`.

**Backends**

1. **io_uring (Linux liburing >= 2.5)**:
   * Provides modern kernel-level asynchronous I/O submission and completion rings (`ring_rcv` and `ring_snd`).
   * Eliminates system call overhead during steady-state network operations through ring buffers.
   * Signals are handled via POSIX `sigaction()` with atomic pending flags dispatched on each loop iteration.
   * Periodic timers utilize kernel timespec timeout entries (`IORING_OP_TIMEOUT`).

2. **epoll (Linux default)**:
   * Uses Linux edge/level-triggered `epoll(7)` monitoring.
   * Signals are handled via POSIX `sigaction()` with atomic pending flags dispatched on each loop iteration.
   * Periodic timers use Linux `timerfd_create(2)` and `timerfd_settime(2)` for millisecond resolution timeouts.

3. **kqueue (BSD/macOS default)**:
   * Native event notification filter mechanism on BSD and macOS.
   * Supports `EVFILT_READ` and `EVFILT_TIMER` within a single kernel queue structure; signals are handled via POSIX `sigaction()` with atomic pending flags dispatched on each loop iteration.

**Data Structures**

* `struct event_loop` centralizes all loop state:
  * `running`: Atomic boolean flag governing the loop execution.
  * `forked`: Atomic boolean flag marking whether the loop was inherited after a `fork()`.
  * `owner_pid`: PID of the process that created the loop.
  * `sigset`: Tracks signals intercepted by the loop.
  * `events`: Array of generic `event_watcher_t*` pointers representing active watchers.
  * Backend handles (`ring_rcv` / `ring_snd` for `io_uring`, `epollfd` for `epoll`, `kqueuefd` for `kqueue`).
  * `buffer`: Scratch buffer or buffer ring used to stage data transfers efficiently.

**Watcher Types and Responsibilities**

Every watcher embeds a small common header (`event_watcher_t`) containing its event type, enabling the loop to iterate over heterogeneous watcher arrays.

1. **I/O Watchers (`struct io_watcher`)**:
   Monitor file descriptors for read or write readiness.
   * *Main*: Monitors `listen_fd` for incoming connections on the server socket, invoking the accept callback to initialize new client sockets.
   * *Worker*: Monitors communication sockets (`rcv_fd` and `snd_fd`) for client request/response processing.
   * Provides helper routines `pgmoneta_event_accept_init`, `pgmoneta_event_worker_init`, `pgmoneta_io_start`, `pgmoneta_io_stop`, and `pgmoneta_io_send`.

2. **Signal Watchers (`struct signal_watcher`)**:
   Catch POSIX signals (`SIGTERM`, `SIGINT`, `SIGHUP`, etc.) and convert them into event loop notifications.
   * Handled via POSIX `sigaction()` with atomic pending flags dispatched on each loop iteration.
   * Initialized with `pgmoneta_signal_init`, registered with `pgmoneta_signal_start`, and deregistered with `pgmoneta_signal_stop`.

3. **Periodic Watchers (`struct periodic_watcher`)**:
   Trigger callbacks at fixed millisecond intervals or as one-shot timers.
   * Used for background tasks such as retention validation, Prometheus metrics cache cleanup, and idle connection checking.
   * Uses `timerfd` on Linux (for `epoll`), kernel timespecs (for `io_uring`), and `EVFILT_TIMER` (for `kqueue`).
   * Initialized with `pgmoneta_periodic_init(watcher, cb, msec, repeat_ms)`, and controlled via `pgmoneta_periodic_start` and `pgmoneta_periodic_stop`.

**Event Loop Lifecycle**

1. **Initialization (`pgmoneta_event_loop_init`)**:
   Allocates and initializes the `struct event_loop` instance, inspects `config->ev_backend`, detects system capabilities, and establishes backend handles (`io_uring`, `epoll`, or `kqueue`).
2. **Running (`pgmoneta_event_loop_run`)**:
   Enters the continuous polling loop. Waits for events from kernel notification mechanisms, invokes registered callbacks for active watchers, and checks the atomic `running` flag on each iteration.
3. **Breaking (`pgmoneta_event_loop_break`)**:
   Atomically resets the `running` flag to `false` and wakes up the wait cycle so that `pgmoneta_event_loop_run` can cleanly exit.
4. **Destruction (`pgmoneta_event_loop_destroy`)**:
   Stops all registered watchers, closes backend file descriptors (`epollfd`, `kqueuefd`, or `io_uring` rings), frees staging buffers, and deallocates the event loop structure.
5. **Fork Handling (`pgmoneta_event_loop_fork`)**:
   Called immediately after `fork()` in child processes to ensure that inherited file descriptors, signal masks, and backend handles from the parent process are closed or re-initialized, preventing state corruption across process boundaries.

**Main vs Worker I/O Watchers**

To simplify connection handling, `pgmoneta` uses a process model where worker processes handle client interactions. Both parent and child processes utilize the event loop abstraction:

* **Main Process**:
  1. Registers I/O watchers on listening sockets (`listen_fd`).
  2. Registers signal watchers for administrative signals (`SIGTERM`, `SIGINT`, `SIGHUP`).
  3. Registers periodic timers for background tasks (retention policy enforcement, metrics caching).
  4. Upon incoming client connections, accepts the connection and spawns worker processes.

* **Worker Process**:
  1. Calls `pgmoneta_event_loop_fork` to reset inherited loop state.
  2. Registers I/O watchers on client connection descriptors (`rcv_fd`, `snd_fd`).
  3. Dispatches incoming management or backup protocol packets to message handlers.

**Enhancements**

Several performance tuning mechanisms are defined as compile-time options for future evaluation:

* **Zero Copy** (`MSG_ZEROCOPY` via io_uring): Reduces CPU overhead by avoiding intermediate buffer copies between kernel and userspace.
* **Fast Poll** (`EPOLLET`): Edge-triggered epoll mode for high-throughput socket polling.
* **Huge Pages** (`IORING_SETUP_NO_MMAP`): Uses large page mappings for `io_uring` buffer rings.
* **Multishot Recv**: Single submission queue entry (SQE) delivering multiple receive completions to lower submission overhead.
* **IOVecs**: Scatter/gather vector I/O arrays for consolidated network operations.
