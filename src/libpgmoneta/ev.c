/*
 * Copyright (C) 2026 The pgmoneta community
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this list
 * of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice, this
 * list of conditions and the following disclaimer in the documentation and/or other
 * materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors may
 * be used to endorse or promote products derived from this software without specific
 * prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR
 * TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/* pgmoneta */
#include <ev.h>
#include <logging.h>
#include <message.h>
#include <network.h>
#include <pgmoneta.h>
#include <shmem.h>
#include <utils.h>

/* system */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#if HAVE_LINUX
#if HAVE_IO_URING
#include <liburing.h>
#endif
#include <netdb.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#else
#include <sys/event.h>
#include <sys/time.h>
#include <sys/types.h>
#endif /* HAVE_LINUX */

static int (*loop_init)(void);
static int (*loop_start)(void);
static int (*loop_fork)(void);
static int (*loop_destroy)(void);

static int (*io_start)(struct io_watcher*);
static int (*io_stop)(struct io_watcher*);

static void signal_handler(int signum, siginfo_t* info, void* p);

static int (*periodic_init)(struct periodic_watcher*, int64_t, int64_t);
static int (*periodic_start)(struct periodic_watcher*);
static int (*periodic_stop)(struct periodic_watcher*);

#if HAVE_LINUX

#if HAVE_IO_URING
static int ev_io_uring_init(void);
static int ev_io_uring_destroy(void);
static int ev_io_uring_loop(void);
static int ev_io_uring_fork(void);
static int ev_io_uring_handler(struct io_uring_cqe*);
#if EXPERIMENTAL_FEATURE_RECV_MULTISHOT_ENABLED
static int ev_io_uring_setup_buffers(void);
#endif /* EXPERIMENTAL_FEATURE_RECV_MULTISHOT_ENABLED */

static int ev_io_uring_io_start(struct io_watcher*);
static int ev_io_uring_io_stop(struct io_watcher*);

static int ev_io_uring_periodic_init(struct periodic_watcher*, int64_t, int64_t);
static int ev_io_uring_periodic_start(struct periodic_watcher*);
static int ev_io_uring_periodic_stop(struct periodic_watcher*);
#endif /* HAVE_IO_URING */

static int ev_epoll_init(void);
static int ev_epoll_destroy(void);
static int ev_epoll_loop(void);
static int ev_epoll_fork(void);
static int ev_epoll_handler(void*);

static int ev_epoll_io_start(struct io_watcher*);
static int ev_epoll_io_stop(struct io_watcher*);
static int ev_epoll_io_handler(struct io_watcher*);

static int ev_epoll_periodic_init(struct periodic_watcher*, int64_t, int64_t);
static int ev_epoll_periodic_start(struct periodic_watcher*);
static int ev_epoll_periodic_stop(struct periodic_watcher*);
static int ev_epoll_periodic_handler(struct periodic_watcher*);

#else

static int ev_kqueue_init(void);
static int ev_kqueue_destroy(void);
static int ev_kqueue_loop(void);
static int ev_kqueue_fork(void);
static int ev_kqueue_handler(struct kevent*);

static int ev_kqueue_io_start(struct io_watcher*);
static int ev_kqueue_io_stop(struct io_watcher*);
static int ev_kqueue_io_handler(struct kevent*);

static int ev_kqueue_periodic_init(struct periodic_watcher*, int64_t, int64_t);
static int ev_kqueue_periodic_start(struct periodic_watcher*);
static int ev_kqueue_periodic_stop(struct periodic_watcher*);
static int ev_kqueue_periodic_handler(struct kevent*);

static int ev_kqueue_signal_start(struct signal_watcher*);
static int ev_kqueue_signal_stop(struct signal_watcher*);
static int ev_kqueue_signal_handler(struct kevent*);

#endif /* HAVE_LINUX */

static void init_watcher_message(struct io_watcher* watcher);
static void dispatch_signal_callbacks(void);
static int initialize_loop_backend(void);

/* context globals */

static struct event_loop* loop = NULL;
static bool context_is_set = false;
static _Atomic(struct signal_watcher*) signal_watchers[PGMONETA_NSIG] = {0};
static _Atomic(signal_cb) signal_callbacks[PGMONETA_NSIG] = {0};
static volatile sig_atomic_t signal_pending[PGMONETA_NSIG] = {0};

#if HAVE_LINUX

#if HAVE_IO_URING
static struct io_uring_params params; /* io_uring argument params */
static int ring_size;                 /* io_uring sqe ring_size */
#endif

static int epoll_flags; /* Flags for epoll instance creation */

#else

static int kqueue_flags; /* Flags for kqueue instance creation */

#endif /* HAVE_LINUX */

/**
 * Check if the event loop is being called from a child process after fork.
 * Returns true if the loop was forked and we're now in the child process.
 * Logs a warning message with function name and process info.
 */
static bool
event_loop_called_from_child(const char* fn)
{
   if (unlikely(!loop))
   {
      return false;
   }

   if (atomic_load(&loop->forked))
   {
      pgmoneta_log_warn("%s ignored in forked child process (pid=%d, parent loop owner pid=%d)",
                        fn, (int)getpid(), (int)loop->owner_pid);
      return true;
   }

   return false;
}

/* pgmoneta doesn't have vault, so no context switching needed */
static int
setup_ops(void)
{
   int backend_type = PGMONETA_EVENT_BACKEND_AUTO;
   int original_backend;
   const char* backend_name;

   // Determine backend type from configuration
   struct main_configuration* config = (struct main_configuration*)shmem;
   if (config)
   {
      backend_type = config->ev_backend;
   }

   original_backend = backend_type;

   if (backend_type == PGMONETA_EVENT_BACKEND_AUTO)
   {
      backend_type = DEFAULT_EVENT_BACKEND;
   }

#if HAVE_LINUX
#if HAVE_IO_URING
   if (backend_type == PGMONETA_EVENT_BACKEND_IO_URING)
   {
      loop_init = ev_io_uring_init;
      loop_fork = ev_io_uring_fork;
      loop_destroy = ev_io_uring_destroy;
      loop_start = ev_io_uring_loop;
      io_start = ev_io_uring_io_start;
      io_stop = ev_io_uring_io_stop;
      periodic_init = ev_io_uring_periodic_init;
      periodic_start = ev_io_uring_periodic_start;
      periodic_stop = ev_io_uring_periodic_stop;
      backend_name = "io_uring";
      goto log_backend;
   }
#else
   if (backend_type == PGMONETA_EVENT_BACKEND_IO_URING)
   {
      pgmoneta_log_warn("io_uring backend not available; falling back to epoll");
      backend_type = PGMONETA_EVENT_BACKEND_EPOLL;
   }
#endif /* HAVE_IO_URING */
   if (backend_type == PGMONETA_EVENT_BACKEND_EPOLL)
   {
      loop_init = ev_epoll_init;
      loop_fork = ev_epoll_fork;
      loop_destroy = ev_epoll_destroy;
      loop_start = ev_epoll_loop;
      io_start = ev_epoll_io_start;
      io_stop = ev_epoll_io_stop;
      periodic_init = ev_epoll_periodic_init;
      periodic_start = ev_epoll_periodic_start;
      periodic_stop = ev_epoll_periodic_stop;
      backend_name = "epoll";
      goto log_backend;
   }
#else
   if (backend_type == PGMONETA_EVENT_BACKEND_KQUEUE)
   {
      loop_init = ev_kqueue_init;
      loop_fork = ev_kqueue_fork;
      loop_destroy = ev_kqueue_destroy;
      loop_start = ev_kqueue_loop;
      io_start = ev_kqueue_io_start;
      io_stop = ev_kqueue_io_stop;
      periodic_init = ev_kqueue_periodic_init;
      periodic_start = ev_kqueue_periodic_start;
      periodic_stop = ev_kqueue_periodic_stop;
      backend_name = "kqueue";
      goto log_backend;
   }
#endif /* HAVE_LINUX */

   pgmoneta_log_error("Event backend: unsupported (%d)", backend_type);
   return PGMONETA_EVENT_RC_ERROR;

log_backend:
   if (loop != NULL)
   {
      loop->backend = backend_type;
   }
   // Log backend selection
   if (original_backend == PGMONETA_EVENT_BACKEND_IO_URING && backend_type == PGMONETA_EVENT_BACKEND_EPOLL)
   {
      pgmoneta_log_warn("Event backend: epoll (fallback from io_uring)");
   }
   else if (original_backend == PGMONETA_EVENT_BACKEND_AUTO)
   {
      pgmoneta_log_info("Event backend: %s (auto-selected)", backend_name);
   }
   else
   {
      pgmoneta_log_info("Event backend: %s", backend_name);
   }

   return PGMONETA_EVENT_RC_OK;
}

static int
initialize_loop_backend(void)
{
   int rc;

   rc = loop_init();
   if (rc == PGMONETA_EVENT_RC_OK)
   {
      return PGMONETA_EVENT_RC_OK;
   }

#if HAVE_LINUX
   struct main_configuration* config = (struct main_configuration*)shmem;
   if (config != NULL && config->ev_backend == PGMONETA_EVENT_BACKEND_IO_URING)
   {
      pgmoneta_log_warn("io_uring backend initialization failed; falling back to epoll");
      config->ev_backend = PGMONETA_EVENT_BACKEND_EPOLL;

      if (setup_ops())
      {
         return PGMONETA_EVENT_RC_ERROR;
      }

      return loop_init();
   }
#endif

   return rc;
}

struct event_loop*
pgmoneta_event_loop_init(void)
{
   loop = calloc(1, sizeof(struct event_loop));
   if (loop == NULL)
   {
      pgmoneta_log_fatal("calloc error: %s", strerror(errno));
      return NULL;
   }
   atomic_init(&loop->forked, false);
   loop->owner_pid = getpid();
   sigemptyset(&loop->sigset);

   if (!context_is_set)
   {
#if HAVE_LINUX
#if HAVE_IO_URING
      /* io_uring context */

#if EXPERIMENTAL_FEATURE_RECV_MULTISHOT_ENABLED
      ring_size = 128;
      params.cq_entries = 1024;
#else
      ring_size = 64;
      params.cq_entries = 128;
#endif /* EXPERIMENTAL_FEATURE_RECV_MULTISHOT_ENABLED */

      params.flags = 0;
      params.flags |= IORING_SETUP_CQSIZE; /* needed if I'm using cq_entries above */
      params.flags |= IORING_SETUP_DEFER_TASKRUN;
      params.flags |= IORING_SETUP_SINGLE_ISSUER;

#if EXPERIMENTAL_FEATURE_FAST_POLL_ENABLED
      params.flags |= IORING_FEAT_FAST_POLL;
#endif /* EXPERIMENTAL_FEATURE_FAST_POLL_ENABLED */
#if EXPERIMENTAL_FEATURE_USE_HUGE_ENABLED
      /* XXX: Maybe this could be interesting if we cache the rings and the buffers? */
      params.flags |= IORING_SETUP_NO_MMAP;
#endif /* EXPERIMENTAL_FEATURE_USE_HUGE_ENABLED */
#endif /* HAVE_IO_URING */

      /* epoll context */
      epoll_flags = EPOLL_CLOEXEC;
#else
      /* kqueue context */
      kqueue_flags = 0;
#endif /* HAVE_LINUX */

      if (setup_ops())
      {
         pgmoneta_log_fatal("Failed to set event backend operations");
         goto error;
      }

      if (initialize_loop_backend())
      {
         pgmoneta_log_fatal("Failed to initiate loop");
         goto error;
      }

      context_is_set = true;
   }
   else if (initialize_loop_backend())
   {
      pgmoneta_log_fatal("Failed to initiate loop");
      goto error;
   }

   return loop;

error:
   free(loop);
   loop = NULL;

   return NULL;
}

int
pgmoneta_event_loop_run(void)
{
   return loop_start();
}

int
pgmoneta_event_loop_fork(void)
{
   int rc;
   pgmoneta_log_trace("pgmoneta_event_loop_fork: loop=%p", loop);
   if (loop == NULL)
   {
      return PGMONETA_EVENT_RC_OK;
   }

   if (sigprocmask(SIG_UNBLOCK, &loop->sigset, NULL) == -1)
   {
      pgmoneta_log_fatal("sigprocmask error: %s", strerror(errno));
      return PGMONETA_EVENT_RC_FATAL;
   }

   /* no need to empty sigset */
   atomic_store(&loop->forked, true);
   rc = loop_fork();

   return rc;
}

bool
pgmoneta_event_loop_is_forked(void)
{
   return loop != NULL && atomic_load(&loop->forked);
}

int
pgmoneta_event_loop_destroy(void)
{
   int rc = PGMONETA_EVENT_RC_OK;

   if (unlikely(!loop))
   {
      return 0;
   }

   /* Free watcher message buffers */
   for (int i = 0; i < loop->events_nr; i++)
   {
      event_watcher_t* w = loop->events[i];
      if (w != NULL && (w->type == PGMONETA_EVENT_TYPE_MAIN || w->type == PGMONETA_EVENT_TYPE_WORKER))
      {
         struct io_watcher* watcher = (struct io_watcher*)w;
         if (watcher->msg != NULL)
         {
            free(watcher->msg->data);
            free(watcher->msg);
            watcher->msg = NULL;
         }
      }
   }

   if (!atomic_load(&loop->forked))
   {
      rc = loop_destroy();

#if HAVE_LINUX
      for (int i = 0; i < loop->events_nr; i++)
      {
         event_watcher_t* w = loop->events[i];

         if (w && w->type == PGMONETA_EVENT_TYPE_PERIODIC)
         {
            struct periodic_watcher* p = (struct periodic_watcher*)w;
            if (p->fd != -1)
            {
               pgmoneta_disconnect(p->fd);
               p->fd = -1;
            }
         }
      }
#endif
   }

   free(loop);
   loop = NULL;
   context_is_set = false;

   return rc;
}

void
pgmoneta_event_loop_start(void)
{
   atomic_store(&loop->running, true);
}

void
pgmoneta_event_loop_break(void)
{
   /* This function can be called even from interrupt handler, so we cannot
    * guarantee the loop exists at all times */
   if (unlikely(!loop))
   {
      return;
   }

   atomic_store(&loop->running, false);
}

bool
pgmoneta_event_loop_is_running(void)
{
   return atomic_load(&loop->running);
}

int
pgmoneta_event_accept_init(struct io_watcher* watcher, int listen_fd, io_cb cb)
{
   watcher->event_watcher.type = PGMONETA_EVENT_TYPE_MAIN;
   watcher->fds.main.listen_fd = listen_fd;
   watcher->fds.main.client_fd = -1;
   watcher->cb = cb;
   return PGMONETA_EVENT_RC_OK;
}

int
pgmoneta_event_worker_init(struct io_watcher* watcher, int rcv_fd, int snd_fd, io_cb cb)
{
   struct main_configuration* config = (struct main_configuration*)shmem;

   watcher->event_watcher.type = PGMONETA_EVENT_TYPE_WORKER;
   watcher->fds.worker.rcv_fd = rcv_fd;
   watcher->fds.worker.snd_fd = snd_fd;
   watcher->cb = cb;

   if (config != NULL && config->ev_backend == PGMONETA_EVENT_BACKEND_IO_URING)
   {
      init_watcher_message(watcher);
   }
   else
   {
      watcher->msg = NULL;
   }

   return PGMONETA_EVENT_RC_OK;
}

int
pgmoneta_io_start(struct io_watcher* watcher)
{
   if (event_loop_called_from_child("pgmoneta_io_start"))
   {
      return PGMONETA_EVENT_RC_OK;
   }
   assert(loop != NULL && watcher != NULL);
   if (unlikely(loop == NULL || watcher == NULL))
   {
      return PGMONETA_EVENT_RC_ERROR;
   }

   if (loop->events_nr >= MAX_EVENTS)
   {
      pgmoneta_log_warn("pgmoneta_io_start: MAX_EVENTS (%d) reached - cannot register new watcher (fd rcv=%d, snd=%d)",
                        MAX_EVENTS, watcher->fds.worker.rcv_fd, watcher->fds.worker.snd_fd);
      return PGMONETA_EVENT_RC_FATAL;
   }

   loop->events[loop->events_nr] = (event_watcher_t*)watcher;
   loop->events_nr++;

   return io_start(watcher);
}

int
pgmoneta_io_stop(struct io_watcher* watcher)
{
   if (event_loop_called_from_child("pgmoneta_io_stop"))
   {
      return PGMONETA_EVENT_RC_OK;
   }

   int i;

   assert(loop != NULL && watcher != NULL);

   for (i = 0; i < loop->events_nr; i++)
   {
      if (watcher == (struct io_watcher*)loop->events[i])
      {
         break;
      }
   }

   if (i >= loop->events_nr)
   {
      pgmoneta_log_warn("pgmoneta_io_stop: watcher not found in events list (fd rcv=%d, snd=%d, events_nr=%d) - possible double-stop",
                        watcher->fds.worker.rcv_fd, watcher->fds.worker.snd_fd, loop->events_nr);
      return PGMONETA_EVENT_RC_ERROR;
   }

   int rc = io_stop(watcher);
   if (rc != PGMONETA_EVENT_RC_OK)
   {
      pgmoneta_log_error("pgmoneta_io_stop: io_stop failed %d", rc);
      return rc;
   }

   if (watcher->msg != NULL)
   {
      free(watcher->msg->data);
      free(watcher->msg);
      watcher->msg = NULL;
   }

   for (int j = i; j < loop->events_nr - 1; j++)
   {
      loop->events[j] = loop->events[j + 1];
   }
   loop->events_nr--;
   loop->events[loop->events_nr] = NULL;

   return PGMONETA_EVENT_RC_OK;
}

int
pgmoneta_periodic_init(struct periodic_watcher* watcher, periodic_cb cb, int64_t msec, int64_t repeat_ms)
{
   watcher->event_watcher.type = PGMONETA_EVENT_TYPE_PERIODIC;
   watcher->cb = cb;
   watcher->repeat_ms = repeat_ms;
   if (periodic_init(watcher, msec, repeat_ms))
   {
      pgmoneta_log_fatal("Failed to initiate timer event");
      return PGMONETA_EVENT_RC_FATAL;
   }
   return PGMONETA_EVENT_RC_OK;
}

int
pgmoneta_periodic_start(struct periodic_watcher* watcher)
{
   if (event_loop_called_from_child("pgmoneta_periodic_start"))
   {
      return PGMONETA_EVENT_RC_OK;
   }
   assert(loop != NULL && watcher != NULL);
   if (unlikely(loop == NULL || watcher == NULL))
   {
      return PGMONETA_EVENT_RC_ERROR;
   }

   if (loop->events_nr >= MAX_EVENTS)
   {
      pgmoneta_log_warn("pgmoneta_periodic_start: MAX_EVENTS (%d) reached - cannot register periodic watcher",
                        MAX_EVENTS);
      return PGMONETA_EVENT_RC_FATAL;
   }

   loop->events[loop->events_nr] = (event_watcher_t*)watcher;
   loop->events_nr++;

   return periodic_start(watcher);
}

int __attribute__((unused))
pgmoneta_periodic_stop(struct periodic_watcher* watcher)
{
   int i;

   if (event_loop_called_from_child("pgmoneta_periodic_stop"))
   {
      return PGMONETA_EVENT_RC_OK;
   }
   assert(loop != NULL && watcher != NULL);

   for (i = 0; i < loop->events_nr; i++)
   {
      if (watcher == (struct periodic_watcher*)loop->events[i])
      {
         break;
      }
   }

   if (i >= loop->events_nr)
   {
      return PGMONETA_EVENT_RC_ERROR;
   }

   int rc = periodic_stop(watcher);
   if (rc != PGMONETA_EVENT_RC_OK)
   {
      pgmoneta_log_error("pgmoneta_periodic_stop: periodic_stop failed %d", rc);
      return rc;
   }

   for (int j = i; j < loop->events_nr - 1; j++)
   {
      loop->events[j] = loop->events[j + 1];
   }
   loop->events_nr--;
   loop->events[loop->events_nr] = NULL;

   return PGMONETA_EVENT_RC_OK;
}

int
pgmoneta_event_prep_submit_send(struct io_watcher* watcher, struct message* msg)
{
   if (unlikely(loop == NULL || watcher == NULL || msg == NULL))
   {
      return -1;
   }

#if HAVE_LINUX && HAVE_IO_URING
   if (loop->backend == PGMONETA_EVENT_BACKEND_IO_URING)
   {
      int sent_bytes = 0;
      struct io_uring_sqe* sqe = NULL;
      struct io_uring_cqe* cqe = NULL;
      int send_flags = 0;
      int ret;
      int cqe_res;

#if EXPERIMENTAL_FEATURE_RECV_MULTISHOT_ENABLED
      int bid = loop->bid;
      if (loop->br.cnt <= 0 || loop->br.buf == NULL || bid < 0 || bid >= loop->br.cnt)
      {
         pgmoneta_log_fatal("invalid buffer id: %d (count=%d)", bid, loop->br.cnt);
         return -1;
      }
      void* data = loop->br.buf + bid * DEFAULT_BUFFER_SIZE;
      msg->data = data;
#endif /* EXPERIMENTAL_FEATURE_RECV_MULTISHOT_ENABLED */

      ssize_t total_sent = 0;
      ssize_t to_send = msg->length;

      /*
       * Use the dedicated send_ring for sends.
       * This avoids CQE mixing issues where recv completions arrive on the
       * main ring while we're waiting for a send completion. With a separate
       * ring, we're guaranteed to only receive send CQEs here.
       */
      while (total_sent < to_send)
      {
         sqe = io_uring_get_sqe(&loop->ring_snd);
         if (!sqe)
         {
            pgmoneta_log_error("io_uring: no SQE available for send on send_ring");
            return -1;
         }

#if EXPERIMENTAL_FEATURE_ZERO_COPY_ENABLED
         /* XXX: Implement zero copy send (this has been shown to speed up a little some
          * workloads, but the implementation is still problematic). */
         io_uring_prep_send_zc(sqe, watcher->fds.worker.snd_fd,
                               (char*)msg->data + total_sent,
                               to_send - total_sent,
                               send_flags, 0);
#else
         send_flags |= MSG_NOSIGNAL;
         io_uring_prep_send(sqe, watcher->fds.worker.snd_fd,
                            (char*)msg->data + total_sent,
                            to_send - total_sent,
                            send_flags);
#endif /* EXPERIMENTAL_FEATURE_ZERO_COPY_ENABLED */

         io_uring_sqe_set_data(sqe, NULL);

         ret = io_uring_submit(&loop->ring_snd);
         if (ret < 0)
         {
            pgmoneta_log_error("io_uring send submit error: %s", strerror(-ret));
            return -1;
         }

         ret = io_uring_wait_cqe(&loop->ring_snd, &cqe);
         if (ret < 0)
         {
            pgmoneta_log_error("io_uring send wait error: %s", strerror(-ret));
            return -1;
         }

         /* Read cqe->res before calling io_uring_cqe_seen() to prevent the
          * completion from being reused before we read the result. */
         cqe_res = cqe->res;
         io_uring_cqe_seen(&loop->ring_snd, cqe);

         if (cqe_res < 0)
         {
            pgmoneta_log_debug("io_uring send error fd=%d: %s",
                               watcher->fds.worker.snd_fd, strerror(-cqe_res));
            return cqe_res;
         }

         if (cqe_res == 0)
         {
            /* Connection closed */
            pgmoneta_log_debug("io_uring send closed fd=%d after %zd/%zd bytes",
                               watcher->fds.worker.snd_fd, total_sent, to_send);
            break;
         }

         if (cqe_res > INT_MAX || total_sent > (ssize_t)(INT_MAX - cqe_res))
         {
            pgmoneta_log_error("io_uring send overflow: total=%zd cqe_res=%d", total_sent, cqe_res);
            return -EOVERFLOW;
         }
         total_sent += cqe_res;
      }

      if (total_sent > INT_MAX)
      {
         pgmoneta_log_error("io_uring send overflow: %zd", total_sent);
         sent_bytes = -EOVERFLOW;
      }
      else
      {
         sent_bytes = (int)total_sent;
      }

#if EXPERIMENTAL_FEATURE_RECV_MULTISHOT_ENABLED
      io_uring_buf_ring_add(loop->br.br,
                            data,
                            DEFAULT_BUFFER_SIZE,
                            bid,
                            DEFAULT_BUFFER_SIZE,
                            1);
      io_uring_buf_ring_advance(loop->br.br, 1);
#endif /* EXPERIMENTAL_FEATURE_RECV_MULTISHOT_ENABLED */

      return sent_bytes;
   }
#endif /* HAVE_LINUX && HAVE_IO_URING */

   /* If epoll or kqueue (or if io_uring not active), execute synchronous send */
   int rc = pgmoneta_io_send(watcher, msg);
   return (rc == PGMONETA_EVENT_RC_OK) ? (int)msg->length : -1;
}

int
pgmoneta_io_send(struct io_watcher* watcher, struct message* msg)
{
   if (unlikely(loop == NULL || watcher == NULL || msg == NULL))
   {
      return PGMONETA_EVENT_RC_ERROR;
   }

#if HAVE_LINUX && HAVE_IO_URING
   if (loop->backend == PGMONETA_EVENT_BACKEND_IO_URING)
   {
      int sent = pgmoneta_event_prep_submit_send(watcher, msg);
      if (sent >= 0)
      {
         return PGMONETA_EVENT_RC_OK;
      }
      return PGMONETA_EVENT_RC_ERROR;
   }
#endif

   int fd = watcher->fds.main.client_fd;
   if (fd < 0 && watcher->event_watcher.type == PGMONETA_EVENT_TYPE_WORKER)
   {
      fd = watcher->fds.worker.snd_fd;
   }
   if (fd < 0)
   {
      fd = watcher->fds.worker.snd_fd >= 0 ? watcher->fds.worker.snd_fd : watcher->fds.main.client_fd;
   }

   if (fd < 0)
   {
      pgmoneta_log_error("pgmoneta_io_send: invalid file descriptor");
      return PGMONETA_EVENT_RC_ERROR;
   }

   ssize_t total_sent = 0;
   ssize_t to_send = msg->length;
   char* buf = (char*)msg->data;

   while (total_sent < to_send)
   {
      ssize_t n = send(fd, buf + total_sent, to_send - total_sent, MSG_NOSIGNAL);
      if (n > 0)
      {
         total_sent += n;
      }
      else if (n == 0)
      {
         pgmoneta_log_debug("pgmoneta_io_send: connection closed on fd %d", fd);
         return PGMONETA_EVENT_RC_CONN_CLOSED;
      }
      else
      {
         if (errno == EINTR)
         {
            continue;
         }
         if (errno == EAGAIN || errno == EWOULDBLOCK)
         {
            fd_set write_fds;
            FD_ZERO(&write_fds);
            FD_SET(fd, &write_fds);
            struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
            int sret = select(fd + 1, NULL, &write_fds, NULL, &tv);
            if (sret > 0)
            {
               continue;
            }
            pgmoneta_log_error("pgmoneta_io_send: socket not writable on fd %d: %s", fd, strerror(errno));
            return PGMONETA_EVENT_RC_ERROR;
         }
         pgmoneta_log_error("pgmoneta_io_send: send error on fd %d: %s", fd, strerror(errno));
         return PGMONETA_EVENT_RC_ERROR;
      }
   }

   return PGMONETA_EVENT_RC_OK;
}

#if HAVE_LINUX
#if HAVE_IO_URING

static inline void __attribute__((unused))
ev_io_uring_rearm_receive(struct event_loop* loop, struct io_watcher* watcher)
{
   struct io_uring_sqe* sqe = io_uring_get_sqe(&loop->ring_rcv);
   if (!sqe)
   {
      pgmoneta_log_error("io_uring: no SQE available for rearm");
      return;
   }
   io_uring_sqe_set_data(sqe, watcher);
   io_uring_prep_recv_multishot(sqe, watcher->fds.worker.rcv_fd, NULL, 0, 0);
}

static int
ev_io_uring_init(void)
{
   int rc;
   struct io_uring_params send_params = {0};

   /* Initialize the main ring for receives */
   rc = io_uring_queue_init_params(ring_size, &loop->ring_rcv, &params);
   if (rc)
   {
      pgmoneta_log_fatal("io_uring_queue_init_params (recv ring) error: %s", strerror(-rc));
      return rc;
   }

   rc = io_uring_ring_dontfork(&loop->ring_rcv);
   if (rc)
   {
      pgmoneta_log_fatal("io_uring_ring_dontfork (recv ring) error: %s", strerror(-rc));
      io_uring_queue_exit(&loop->ring_rcv);
      return rc;
   }

   /* Initialize a separate ring for sends to avoid CQE mixing issues.
    * When waiting for a send CQE on a shared ring, recv CQEs may arrive first,
    * causing either lost data, stack overflow (if processed), or state corruption.
    * Using a separate ring guarantees we only get send CQEs when waiting for sends. */
   rc = io_uring_queue_init_params(64, &loop->ring_snd, &send_params);
   if (rc)
   {
      pgmoneta_log_fatal("io_uring_queue_init_params (send ring) error: %s", strerror(-rc));
      io_uring_queue_exit(&loop->ring_rcv);
      return rc;
   }

   rc = io_uring_ring_dontfork(&loop->ring_snd);
   if (rc)
   {
      pgmoneta_log_fatal("io_uring_ring_dontfork (send ring) error: %s", strerror(-rc));
      io_uring_queue_exit(&loop->ring_rcv);
      io_uring_queue_exit(&loop->ring_snd);
      return rc;
   }

#if EXPERIMENTAL_FEATURE_RECV_MULTISHOT_ENABLED
   rc = ev_io_uring_setup_buffers();
   if (rc)
   {
      pgmoneta_log_fatal("ev_io_uring_setup_buffers error");
      return rc;
   }
#endif /* EXPERIMENTAL_FEATURE_RECV_MULTISHOT_ENABLED */

   if (loop != NULL)
   {
      loop->backend = PGMONETA_EVENT_BACKEND_IO_URING;
   }

   return PGMONETA_EVENT_RC_OK;
}

static int
ev_io_uring_destroy(void)
{
#if EXPERIMENTAL_FEATURE_RECV_MULTISHOT_ENABLED
   if (loop->br.buf != NULL)
   {
      free(loop->br.buf);
      loop->br.buf = NULL;
   }
#endif /* EXPERIMENTAL_FEATURE_RECV_MULTISHOT_ENABLED */
   io_uring_queue_exit(&loop->ring_rcv);
   io_uring_queue_exit(&loop->ring_snd);
   return PGMONETA_EVENT_RC_OK;
}

static int
ev_io_uring_io_start(struct io_watcher* watcher)
{
   struct io_uring_sqe* sqe = io_uring_get_sqe(&loop->ring_rcv);
   struct message* msg = NULL;

   if (unlikely(!sqe))
   {
      pgmoneta_log_error("io_uring: no SQE available for recv/accept");
      return PGMONETA_EVENT_RC_ERROR;
   }

   io_uring_sqe_set_data(sqe, watcher);
   switch (watcher->event_watcher.type)
   {
      case PGMONETA_EVENT_TYPE_MAIN:
         io_uring_prep_multishot_accept(sqe, watcher->fds.main.listen_fd, NULL, NULL, 0);
         break;
      case PGMONETA_EVENT_TYPE_WORKER:
#if EXPERIMENTAL_FEATURE_RECV_MULTISHOT_ENABLED
         io_uring_prep_recv_multishot(sqe, watcher->fds.worker.rcv_fd, NULL, 0, 0); /* msg must be NULL */
         sqe->buf_group = 0;
         sqe->flags |= IOSQE_BUFFER_SELECT;
#else
         msg = pgmoneta_get_watcher_message(watcher);
         /* Use MESSAGE_PARSE_BUFFER_SIZE to leave headroom and prevent buffer
          * overflow when parsing message headers near the end of received data */
         io_uring_prep_recv(sqe, watcher->fds.worker.rcv_fd, msg->data, MESSAGE_PARSE_BUFFER_SIZE, 0);
#endif /* EXPERIMENTAL_FEATURE_RECV_MULTISHOT_ENABLED */
         break;
      default:
         pgmoneta_log_fatal("unknown event type: %d", watcher->event_watcher.type);
         exit(1);
   }
   return PGMONETA_EVENT_RC_OK;
}

static int
ev_io_uring_io_stop(struct io_watcher* target)
{
   int rc = PGMONETA_EVENT_RC_OK;
   struct io_uring_sqe* sqe;
   struct io_uring_cqe* cqe;
   struct __kernel_timespec ts = {.tv_sec = 2, .tv_nsec = 0};

   /* When io_stop is called it may never return to a loop
    * where sqes are submitted. Flush these sqes so the get call
    * doesn't return NULL. */
   for (int retries = 0; retries < 100; retries++)
   {
      sqe = io_uring_get_sqe(&loop->ring_rcv);
      if (sqe)
      {
         break;
      }
      pgmoneta_log_warn("sqe is full");
      io_uring_submit(&loop->ring_rcv);
   }
   if (!sqe)
   {
      pgmoneta_log_error("io_uring: no SQE available for cancel");
      return PGMONETA_EVENT_RC_ERROR;
   }

   io_uring_prep_cancel(sqe, (void*)target, 0);

   io_uring_submit_and_wait_timeout(&loop->ring_rcv, &cqe, 0, &ts, NULL);

   return rc;
}

static int
ev_io_uring_periodic_init(struct periodic_watcher* watcher, int64_t msec, int64_t repeat_ms)
{
   int64_t initial_ms = (msec == 0 && repeat_ms > 0) ? repeat_ms : msec;

   watcher->repeat_ms = repeat_ms;
   watcher->ts = (struct __kernel_timespec){
      .tv_sec = initial_ms / 1000,
      .tv_nsec = (initial_ms % 1000) * 1000000};
   return PGMONETA_EVENT_RC_OK;
}

static int
ev_io_uring_periodic_start(struct periodic_watcher* watcher)
{
   struct io_uring_sqe* sqe = io_uring_get_sqe(&loop->ring_rcv);
   if (!sqe)
   {
      pgmoneta_log_error("io_uring: no SQE available for periodic start");
      return PGMONETA_EVENT_RC_ERROR;
   }
   io_uring_sqe_set_data(sqe, watcher);
   if (watcher->repeat_ms > 0)
   {
      int64_t cur_ms = watcher->ts.tv_sec * 1000 + watcher->ts.tv_nsec / 1000000;
      if (cur_ms != watcher->repeat_ms)
      {
         io_uring_prep_timeout(sqe, &watcher->ts, 0, 0);
      }
      else
      {
         io_uring_prep_timeout(sqe, &watcher->ts, 0, IORING_TIMEOUT_MULTISHOT);
      }
   }
   else
   {
      io_uring_prep_timeout(sqe, &watcher->ts, 0, 0);
   }
   io_uring_submit(&loop->ring_rcv);
   return PGMONETA_EVENT_RC_OK;
}

static int
ev_io_uring_periodic_stop(struct periodic_watcher* watcher)
{
   struct io_uring_sqe* sqe;
   sqe = io_uring_get_sqe(&loop->ring_rcv);
   if (!sqe)
   {
      pgmoneta_log_error("io_uring: no SQE available for periodic stop");
      return PGMONETA_EVENT_RC_ERROR;
   }
   io_uring_prep_cancel64(sqe, (uint64_t)(uintptr_t)watcher, 0);
   io_uring_submit(&loop->ring_rcv);
   return PGMONETA_EVENT_RC_OK;
}

static int __attribute__((unused))
ev_io_uring_flush(void)
{
   int rc = PGMONETA_EVENT_RC_ERROR;
   unsigned int head;
   struct __kernel_timespec ts = {
      .tv_sec = 0,
      .tv_nsec = 100000LL, /* seems best with 100000LL ns for most loads */
   };

   struct io_uring_cqe* cqe;
   struct io_uring_sqe* sqe;
   int to_wait = 0;
   int events = 0;

retry:
   sqe = io_uring_get_sqe(&loop->ring_rcv);
   if (!sqe)
   {
      pgmoneta_log_warn("sqe is full, retrying...");
      io_uring_submit(&loop->ring_rcv);
      goto retry;
   }

   for (int i = 0; i < loop->events_nr; i++)
   {
      io_uring_prep_cancel(sqe, (void*)(loop->events[i]), 0);
      /* XXX: if used, delete event */
      to_wait++;
   }

   io_uring_submit_and_wait_timeout(&loop->ring_rcv, &cqe, to_wait, &ts, NULL);

   io_uring_for_each_cqe(&loop->ring_rcv, head, cqe)
   {
#ifdef DEBUG
      rc = cqe->res;
      if (rc < 0)
      {
         /* -EINVAL shouldn't happen */
         pgmoneta_log_trace("io_uring_prep_cancel rc: %s", strerror(-rc));
      }
#endif
      events++;
   }
   if (events)
   {
      io_uring_cq_advance(&loop->ring_rcv, events);
   }
   return rc;
}

/*
 * Based on: https://git.kernel.dk/cgit/liburing/tree/examples/proxy.c
 * (C) 2024 Jens Axboe <axboe@kernel.dk>
 */
static int
ev_io_uring_loop(void)
{
   int rc = PGMONETA_EVENT_RC_ERROR;
   int events;
   int to_wait = 1; /* at first, wait for any 1 event */
   unsigned int head;
   struct io_uring_cqe* cqe = NULL;
   struct __kernel_timespec* ts = NULL;
   struct __kernel_timespec idle_ts = {
      .tv_sec = 0,
      .tv_nsec = 100000LL, /* seems best with 100000LL ns for most loads */
   };

   pgmoneta_event_loop_start();
   while (pgmoneta_event_loop_is_running())
   {
      ts = &idle_ts;

      io_uring_submit_and_wait_timeout(&loop->ring_rcv, &cqe, to_wait, ts, NULL);
      dispatch_signal_callbacks();

      if (*loop->ring_rcv.cq.koverflow)
      {
         pgmoneta_log_fatal("io_uring overflow %u", *loop->ring_rcv.cq.koverflow);
         pgmoneta_event_loop_break();
         return PGMONETA_EVENT_RC_FATAL;
      }
      if (*loop->ring_rcv.sq.kflags & IORING_SQ_CQ_OVERFLOW)
      {
         pgmoneta_log_fatal("io_uring overflow");
         pgmoneta_event_loop_break();
         return PGMONETA_EVENT_RC_FATAL;
      }

      events = 0;
      io_uring_for_each_cqe(&loop->ring_rcv, head, cqe)
      {
         rc = ev_io_uring_handler(cqe);
         if (rc)
         {
            pgmoneta_event_loop_break();
            break;
         }
         events++;
      }

      if (events)
      {
         io_uring_cq_advance(&loop->ring_rcv, events);
      }
   }

   return rc;
}

static int
ev_io_uring_fork(void)
{
   return 0;
}

static int
ev_io_uring_handler(struct io_uring_cqe* cqe)
{
   int rc = 0;
   event_watcher_t* watcher;
   struct io_watcher* io;
   struct periodic_watcher* per;
   struct message* msg = NULL;

   if (atomic_load(&loop->forked))
   {
      return PGMONETA_EVENT_RC_OK;
   }

   watcher = io_uring_cqe_get_data(cqe);

#if EXPERIMENTAL_FEATURE_RECV_MULTISHOT_ENABLED
   loop->bid = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
#endif /* EXPERIMENTAL_FEATURE_RECV_MULTISHOT_ENABLED */

   /* Cancelled requests will trigger the handler, but have NULL data. */
   if (!watcher)
   {
      rc = cqe->res;
      if (rc == -ENOENT)
      {
         pgmoneta_log_trace("io_uring: cancelled operation not found");
         return PGMONETA_EVENT_RC_OK;
      }
      else if (rc == -ECANCELED)
      {
         pgmoneta_log_trace("io_uring: operation cancelled");
         return PGMONETA_EVENT_RC_OK;
      }
      else if (rc < 0)
      {
         pgmoneta_log_debug("io_uring: CQE with NULL watcher, res=%d: %s", rc, strerror(-rc));
      }
      return PGMONETA_EVENT_RC_OK;
   }

   /* This type of thing is not ideal, ideally I should have
    * only event_watcher_t pointers returning in cqe->user_data */
   switch (watcher->type)
   {
      case PGMONETA_EVENT_TYPE_PERIODIC:
         per = (struct periodic_watcher*)watcher;
         if (cqe->res == -ECANCELED)
         {
            pgmoneta_log_trace("io_uring: periodic timer cancelled");
            break;
         }
         per->cb();
         if (per->repeat_ms > 0 && !(cqe->flags & IORING_CQE_F_MORE))
         {
            per->ts = (struct __kernel_timespec){
               .tv_sec = per->repeat_ms / 1000,
               .tv_nsec = (per->repeat_ms % 1000) * 1000000};
            if (pgmoneta_event_loop_is_running())
            {
               ev_io_uring_periodic_start(per);
            }
         }
         break;
      case PGMONETA_EVENT_TYPE_MAIN:
         io = (struct io_watcher*)watcher;
         if (cqe->res < 0)
         {
            if (cqe->res == -ECANCELED)
            {
               pgmoneta_log_debug("io_uring: accept operation canceled");
            }
            else
            {
               pgmoneta_log_error("io_uring: accept error: %s", strerror(-cqe->res));
            }

            /* Do NOT rearm if the operation was canceled (e.g., during reload).
             * Rearming a canceled watcher can resurrect a stale fd and race with
             * the new bind during live reconfiguration. */
            if (pgmoneta_event_loop_is_running() && cqe->res != -ECANCELED)
            {
               ev_io_uring_io_start(io);
            }
            return PGMONETA_EVENT_RC_OK;
         }
         pgmoneta_log_trace("io_uring: accept fd %d", cqe->res);
         io->fds.main.client_fd = cqe->res;
         io->cb(io);

         if (!(cqe->flags & IORING_CQE_F_MORE))
         {
            pgmoneta_log_debug("io_uring: multishot accept ended: rearming");
            if (pgmoneta_event_loop_is_running())
            {
               ev_io_uring_io_start(io);
            }
         }
         break;
      case PGMONETA_EVENT_TYPE_WORKER:
         io = (struct io_watcher*)watcher;
         msg = pgmoneta_get_watcher_message(io);
         if (cqe->res <= 0)
         {
            if (cqe->res == 0)
            {
               pgmoneta_log_debug("io_uring: connection closed fd=%d", io->fds.worker.rcv_fd);
            }
            else
            {
               pgmoneta_log_debug("io_uring: recv error fd=%d: %s",
                                  io->fds.worker.rcv_fd, strerror(-cqe->res));
            }
            msg->length = 0;
            rc = PGMONETA_EVENT_RC_CONN_CLOSED;
            io->cb(io);
            /* Do NOT rearm after connection close or error */
         }
         else
         {
            msg->length = cqe->res;
            rc = PGMONETA_EVENT_RC_OK;
            io->cb(io);

            /* Only rearm if loop is still running and connection is good */
            if (pgmoneta_event_loop_is_running())
            {
               ev_io_uring_io_start(io);
            }
         }

         break;
      default:
         /* reaching here is a bug, do not recover */
         pgmoneta_log_fatal("BUG: Unknown event type: %d", watcher->type);
         return PGMONETA_EVENT_RC_FATAL;
   }
   return rc;
}

#if EXPERIMENTAL_FEATURE_RECV_MULTISHOT_ENABLED
static int
ev_io_uring_setup_buffers(void)
{
   int rc;
   int br_cnt = 1;
   int br_bgid = 0;
   int br_mask = DEFAULT_BUFFER_SIZE;
   int br_flags = 0;
   int bid = 0;

#if EXPERIMENTAL_FEATURE_USE_HUGE_ENABLED
   pgmoneta_log_fatal("io_uring use_huge not implemented");
   exit(1);
#endif /* EXPERIMENTAL_FEATURE_USE_HUGE_ENABLED */

   loop->br.br = NULL;
   loop->br.buf = NULL;
   loop->br.pending_send = false;
   loop->br.cnt = 0;

   loop->br.br = io_uring_setup_buf_ring(&loop->ring_rcv, br_cnt, br_bgid, br_flags, &rc);
   if (!loop->br.br)
   {
      pgmoneta_log_fatal("buffer ring register error %s", strerror(-rc));
      return PGMONETA_EVENT_RC_FATAL;
   }
   if (posix_memalign(&loop->br.buf, sysconf(_SC_PAGESIZE), 2 * DEFAULT_BUFFER_SIZE))
   {
      pgmoneta_log_fatal("posix_memalign error: %s", strerror(errno));
      return PGMONETA_EVENT_RC_FATAL;
   }
   io_uring_buf_ring_add(loop->br.br,
                         loop->br.buf,
                         DEFAULT_BUFFER_SIZE,
                         bid++,
                         br_mask,
                         loop->br.cnt++);

   io_uring_buf_ring_add(loop->br.br,
                         loop->br.buf + DEFAULT_BUFFER_SIZE,
                         DEFAULT_BUFFER_SIZE,
                         bid,
                         br_mask,
                         loop->br.cnt++);

   io_uring_buf_ring_advance(loop->br.br, loop->br.cnt);

   return PGMONETA_EVENT_RC_OK;
}
#endif /* EXPERIMENTAL_FEATURE_RECV_MULTISHOT_ENABLED */

#endif /* HAVE_IO_URING */

static int
ev_epoll_loop(void)
{
   int rc = PGMONETA_EVENT_RC_OK;
   int nfds;
   struct epoll_event events[MAX_EVENTS];
#if HAVE_EPOLL_PWAIT2
   struct timespec timeout_ts = {
      .tv_sec = 0,
      .tv_nsec = 10000000LL,
   };
#else
   int timeout = 10LL; /* ms */
#endif /* HAVE_EPOLL_PWAIT2 */

   pgmoneta_event_loop_start();
   while (pgmoneta_event_loop_is_running())
   {
#if HAVE_EPOLL_PWAIT2
      nfds = epoll_pwait2(loop->epollfd, events, MAX_EVENTS, &timeout_ts,
                          NULL);
#else
      nfds = epoll_pwait(loop->epollfd, events, MAX_EVENTS, timeout, NULL);
#endif

      if (nfds == -1)
      {
         if (errno == EINTR)
         {
            dispatch_signal_callbacks();
            continue;
         }
         pgmoneta_log_error("epoll_pwait error: %s", strerror(errno));
         rc = PGMONETA_EVENT_RC_ERROR;
         pgmoneta_event_loop_break();
         break;
      }

      dispatch_signal_callbacks();

      for (int i = 0; i < nfds; i++)
      {
         rc = ev_epoll_handler((void*)events[i].data.u64);
         if (rc)
         {
            pgmoneta_event_loop_break();
            break;
         }
      }
   }
   return rc;
}

static int
ev_epoll_init(void)
{
   epoll_flags = EPOLL_CLOEXEC;
   loop->epollfd = -1;
   loop->epollfd = epoll_create1(epoll_flags);
   if (loop->epollfd == -1)
   {
      pgmoneta_log_fatal("epoll_init error: %s", strerror(errno));
      return PGMONETA_EVENT_RC_FATAL;
   }
   if (loop != NULL)
   {
      loop->backend = PGMONETA_EVENT_BACKEND_EPOLL;
   }
   return PGMONETA_EVENT_RC_OK;
}

static int
ev_epoll_fork(void)
{
   if (loop->epollfd < 0)
   {
      return PGMONETA_EVENT_RC_OK;
   }

   if (close(loop->epollfd) < 0)
   {
      pgmoneta_log_error("close error: %s", strerror(errno));
      return PGMONETA_EVENT_RC_ERROR;
   }
   loop->epollfd = -1;
   return PGMONETA_EVENT_RC_OK;
}

static int
ev_epoll_destroy(void)
{
   if (loop->epollfd < 0)
   {
      return PGMONETA_EVENT_RC_OK;
   }

   if (close(loop->epollfd) < 0)
   {
      pgmoneta_log_error("close error: %s", strerror(errno));
      return PGMONETA_EVENT_RC_ERROR;
   }
   loop->epollfd = -1;
   return PGMONETA_EVENT_RC_OK;
}

static int
ev_epoll_handler(void* watcher)
{
   enum event_type type;

   if (atomic_load(&loop->forked))
   {
      return PGMONETA_EVENT_RC_OK;
   }

   type = ((event_watcher_t*)watcher)->type;
   if (type == PGMONETA_EVENT_TYPE_PERIODIC)
   {
      return ev_epoll_periodic_handler((struct periodic_watcher*)watcher);
   }
   return ev_epoll_io_handler((struct io_watcher*)watcher);
}

static int
ev_epoll_periodic_init(struct periodic_watcher* watcher, int64_t msec, int64_t repeat_ms)
{
   struct timespec now;
   struct itimerspec new_value;

   if (clock_gettime(CLOCK_MONOTONIC, &now) == -1)
   {
      pgmoneta_log_error("clock_gettime: %s", strerror(errno));
      return PGMONETA_EVENT_RC_ERROR;
   }

   watcher->repeat_ms = repeat_ms;

   new_value.it_value.tv_sec = msec / 1000;
   new_value.it_value.tv_nsec = (msec % 1000) * 1000000;

   if (repeat_ms > 0)
   {
      new_value.it_interval.tv_sec = repeat_ms / 1000;
      new_value.it_interval.tv_nsec = (repeat_ms % 1000) * 1000000;
   }
   else
   {
      new_value.it_interval.tv_sec = 0;
      new_value.it_interval.tv_nsec = 0;
   }

   if (msec == 0 && repeat_ms > 0)
   {
      new_value.it_value.tv_sec = repeat_ms / 1000;
      new_value.it_value.tv_nsec = (repeat_ms % 1000) * 1000000;
   }

   /* no need to set it to non-blocking due to TFD_NONBLOCK */
   watcher->fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
   if (watcher->fd == -1)
   {
      pgmoneta_log_error("timerfd_create: %s", strerror(errno));
      return PGMONETA_EVENT_RC_ERROR;
   }

   if (timerfd_settime(watcher->fd, 0, &new_value, NULL) == -1)
   {
      pgmoneta_log_error("timerfd_settime");
      close(watcher->fd);
      watcher->fd = -1;
      return PGMONETA_EVENT_RC_ERROR;
   }
   return PGMONETA_EVENT_RC_OK;
}

static int
ev_epoll_periodic_start(struct periodic_watcher* watcher)
{
   struct epoll_event event;
   event.events = EPOLLIN;
   event.data.u64 = (uint64_t)watcher;
   if (epoll_ctl(loop->epollfd, EPOLL_CTL_ADD, watcher->fd, &event) == -1)
   {
      pgmoneta_log_fatal("epoll_ctl error: %s", strerror(errno));
      return PGMONETA_EVENT_RC_FATAL;
   }
   return PGMONETA_EVENT_RC_OK;
}

static int
ev_epoll_periodic_stop(struct periodic_watcher* watcher)
{
   if (epoll_ctl(loop->epollfd, EPOLL_CTL_DEL, watcher->fd, NULL) == -1)
   {
      pgmoneta_log_error("epoll_ctl error: %s", strerror(errno));
      return PGMONETA_EVENT_RC_ERROR;
   }

   pgmoneta_disconnect(watcher->fd);
   watcher->fd = -1;

   return PGMONETA_EVENT_RC_OK;
}

static int
ev_epoll_periodic_handler(struct periodic_watcher* watcher)
{
   uint64_t exp;
   int nread = read(watcher->fd, &exp, sizeof(uint64_t));
   if (nread != sizeof(uint64_t))
   {
      pgmoneta_log_error("periodic_handler read: %s", strerror(errno));
      return PGMONETA_EVENT_RC_ERROR;
   }
   watcher->cb();
   return PGMONETA_EVENT_RC_OK;
}

static int
ev_epoll_io_start(struct io_watcher* watcher)
{
   enum event_type type = watcher->event_watcher.type;
   struct epoll_event event;
   int fd;

   event.data.u64 = (uintptr_t)watcher;

   switch (type)
   {
      case PGMONETA_EVENT_TYPE_MAIN:
         fd = watcher->fds.main.listen_fd;
         event.events = EPOLLIN;
         break;
      case PGMONETA_EVENT_TYPE_WORKER:
         fd = watcher->fds.worker.rcv_fd;
         /* XXX: lookup the possibility to add EPOLLET here */
         event.events = EPOLLIN;
         break;
      default:
         /* reaching here is a bug, do not recover */
         pgmoneta_log_fatal("BUG: Unknown event type: %d", type);
         exit(1);
   }

   if (epoll_ctl(loop->epollfd, EPOLL_CTL_ADD, fd, &event) == -1)
   {
      if (errno == EEXIST)
      {
         /* FD already exists, modify it instead */
         pgmoneta_log_debug("epoll_ctl: fd %d already exists, modifying instead", fd);
         if (epoll_ctl(loop->epollfd, EPOLL_CTL_MOD, fd, &event) == -1)
         {
            pgmoneta_log_error("epoll_ctl error when modifying fd %d : %s", fd, strerror(errno));
            return PGMONETA_EVENT_RC_FATAL;
         }
      }
      else
      {
         pgmoneta_log_error("epoll_ctl error when adding fd %d : %s", fd, strerror(errno));
         return PGMONETA_EVENT_RC_FATAL;
      }
   }

   return PGMONETA_EVENT_RC_OK;
}

static int
ev_epoll_io_stop(struct io_watcher* watcher)
{
   enum event_type type = watcher->event_watcher.type;
   int fd;

   switch (type)
   {
      case PGMONETA_EVENT_TYPE_MAIN:
         fd = watcher->fds.main.listen_fd;
         break;
      case PGMONETA_EVENT_TYPE_WORKER:
         fd = watcher->fds.worker.rcv_fd;
         break;
      default:
         /* reaching here is a bug, do not recover */
         pgmoneta_log_fatal("BUG: Unknown event type: %d", type);
         return PGMONETA_EVENT_RC_FATAL;
   }
   if (epoll_ctl(loop->epollfd, EPOLL_CTL_DEL, fd, NULL) == -1)
   {
      if (errno == EBADF || errno == ENOENT || errno == EINVAL)
      {
         pgmoneta_log_error("epoll_ctl error: %s", strerror(errno));
      }
      else
      {
         pgmoneta_log_fatal("epoll_ctl error: %s", strerror(errno));
         return PGMONETA_EVENT_RC_FATAL;
      }
   }
   return PGMONETA_EVENT_RC_OK;
}

static int
ev_epoll_io_handler(struct io_watcher* watcher)
{
   int client_fd = -1;
   enum event_type type = watcher->event_watcher.type;
   switch (type)
   {
      case PGMONETA_EVENT_TYPE_MAIN:
         client_fd = accept(watcher->fds.main.listen_fd, NULL, NULL);
         if (client_fd == -1)
         {
            if (errno != EAGAIN && errno != EWOULDBLOCK)
            {
               pgmoneta_log_error("accept error: %s", strerror(errno));
               return PGMONETA_EVENT_RC_ERROR;
            }
         }
         else
         {
            pgmoneta_log_trace("epoll: accept fd %d", client_fd);
            watcher->fds.main.client_fd = client_fd;
            watcher->cb(watcher);
         }
         break;
      case PGMONETA_EVENT_TYPE_WORKER:
         watcher->cb(watcher);
         break;
      default:
         /* shouldn't happen, do not recover */
         pgmoneta_log_fatal("BUG: Unknown event type: %d", type);
         return PGMONETA_EVENT_RC_FATAL;
   }
   return PGMONETA_EVENT_RC_OK;
}

#else

static int
ev_kqueue_loop(void)
{
   int rc = PGMONETA_EVENT_RC_OK;
   int nfds;
   struct kevent events[MAX_EVENTS];
   struct timespec timeout;
   timeout.tv_sec = 0;
   timeout.tv_nsec = 10000000; /* 10 ms */

   pgmoneta_event_loop_start();
   while (pgmoneta_event_loop_is_running())
   {
      nfds = kevent(loop->kqueuefd, NULL, 0, events, MAX_EVENTS, &timeout);
      if (nfds == -1)
      {
         if (errno == EINTR)
         {
            dispatch_signal_callbacks();
            continue;
         }

         pgmoneta_log_error("kevent error: %s", strerror(errno));
         rc = PGMONETA_EVENT_RC_ERROR;
         pgmoneta_event_loop_break();
         break;
      }
      dispatch_signal_callbacks();
      for (int i = 0; i < nfds; i++)
      {
         rc = ev_kqueue_handler(&events[i]);
         if (rc)
         {
            pgmoneta_event_loop_break();
            break;
         }
      }
   }
   return rc;
}

static int
ev_kqueue_init(void)
{
   loop->kqueuefd = -1;
   loop->kqueuefd = kqueue();
   if (loop->kqueuefd == -1)
   {
      pgmoneta_log_fatal("kqueue init error: %s", strerror(errno));
      return PGMONETA_EVENT_RC_FATAL;
   }
   if (loop != NULL)
   {
      loop->backend = PGMONETA_EVENT_BACKEND_KQUEUE;
   }
   return PGMONETA_EVENT_RC_OK;
}

static int
ev_kqueue_fork(void)
{
   if (loop->kqueuefd >= 0)
   {
      close(loop->kqueuefd);
      loop->kqueuefd = -1;
   }
   return PGMONETA_EVENT_RC_OK;
}

static int
ev_kqueue_destroy(void)
{
   if (loop->kqueuefd >= 0)
   {
      close(loop->kqueuefd);
      loop->kqueuefd = -1;
   }
   return PGMONETA_EVENT_RC_OK;
}

static int
ev_kqueue_handler(struct kevent* kev)
{
   if (atomic_load(&loop->forked))
   {
      return PGMONETA_EVENT_RC_OK;
   }

   switch (kev->filter)
   {
      case EVFILT_TIMER:
         return ev_kqueue_periodic_handler(kev);
      case EVFILT_READ:
      case EVFILT_WRITE:
         return ev_kqueue_io_handler(kev);
      default:
         /* shouldn't happen, do not recover */
         pgmoneta_log_fatal("BUG: Unknown filter in handler");
         return PGMONETA_EVENT_RC_FATAL;
   }
}

int __attribute__((unused))
ev_kqueue_signal_start(struct signal_watcher* watcher)
{
   struct kevent kev;

   EV_SET(&kev, watcher->signum, EVFILT_SIGNAL, EV_ADD, 0, 0, watcher);
   if (kevent(loop->kqueuefd, &kev, 1, NULL, 0, NULL) == -1)
   {
      pgmoneta_log_fatal("kevent error: %s", strerror(errno));
      return PGMONETA_EVENT_RC_FATAL;
   }
   return PGMONETA_EVENT_RC_OK;
}

static int __attribute__((unused))
ev_kqueue_signal_stop(struct signal_watcher* watcher)
{
   struct kevent kev;

   EV_SET(&kev, watcher->signum, EVFILT_SIGNAL, EV_DELETE, 0, 0, watcher);
   if (kevent(loop->kqueuefd, &kev, 1, NULL, 0, NULL) == -1)
   {
      pgmoneta_log_fatal("kevent error: %s", strerror(errno));
      return PGMONETA_EVENT_RC_FATAL;
   }
   return PGMONETA_EVENT_RC_OK;
}

static int __attribute__((unused))
ev_kqueue_signal_handler(struct kevent* kev)
{
   int rc = 0;
   struct signal_watcher* watcher = (struct signal_watcher*)kev->udata;
   watcher->cb();
   return rc;
}

static int
ev_kqueue_periodic_init(struct periodic_watcher* watcher, int64_t msec, int64_t repeat_ms)
{
   watcher->interval = (msec == 0 && repeat_ms > 0) ? repeat_ms : msec;
   watcher->repeat_ms = repeat_ms;
   return PGMONETA_EVENT_RC_OK;
}

static int
ev_kqueue_periodic_start(struct periodic_watcher* watcher)
{
   struct kevent kev;
   int flags = EV_ADD | EV_ENABLE;

   /* NOTE: kqueue timers repeat by default unless EV_ONESHOT is set */
   if (watcher->repeat_ms == 0 || (watcher->repeat_ms > 0 && watcher->interval != watcher->repeat_ms))
   {
      flags |= EV_ONESHOT;
   }

   EV_SET(&kev, (uintptr_t)watcher, EVFILT_TIMER, flags, NOTE_USECONDS,
          watcher->interval * 1000, watcher);
   if (kevent(loop->kqueuefd, &kev, 1, NULL, 0, NULL) == -1)
   {
      pgmoneta_log_error("kevent timer add: %s", strerror(errno));
      return PGMONETA_EVENT_RC_ERROR;
   }
   return PGMONETA_EVENT_RC_OK;
}

static int
ev_kqueue_periodic_stop(struct periodic_watcher* watcher)
{
   struct kevent kev;
   EV_SET(&kev, (uintptr_t)watcher, EVFILT_TIMER, EV_DELETE, 0, 0, NULL);
   if (kevent(loop->kqueuefd, &kev, 1, NULL, 0, NULL) == -1)
   {
      pgmoneta_log_error("kevent timer delete: %s", strerror(errno));
      return PGMONETA_EVENT_RC_ERROR;
   }

   return PGMONETA_EVENT_RC_OK;
}

static int
ev_kqueue_periodic_handler(struct kevent* kev)
{
   struct periodic_watcher* watcher = (struct periodic_watcher*)kev->udata;
   watcher->cb();
   if (watcher->repeat_ms > 0 && watcher->interval != watcher->repeat_ms)
   {
      watcher->interval = watcher->repeat_ms;
      if (pgmoneta_event_loop_is_running())
      {
         ev_kqueue_periodic_start(watcher);
      }
   }
   return PGMONETA_EVENT_RC_OK;
}

static int
ev_kqueue_io_start(struct io_watcher* watcher)
{
   enum event_type type = watcher->event_watcher.type;
   struct kevent kev;
   int filter;
   int fd;

   switch (type)
   {
      case PGMONETA_EVENT_TYPE_MAIN:
         filter = EVFILT_READ;
         fd = watcher->fds.main.listen_fd;
         break;
      case PGMONETA_EVENT_TYPE_WORKER:
         filter = EVFILT_READ;
         fd = watcher->fds.worker.rcv_fd;
         break;
      default:
         /* shouldn't happen, do not recover */
         pgmoneta_log_fatal("Unknown event type: %d", type);
         return PGMONETA_EVENT_RC_FATAL;
   }

   int flags = EV_ADD | EV_ENABLE;
   if (type != PGMONETA_EVENT_TYPE_MAIN)
   {
      flags |= EV_CLEAR;
   }

   EV_SET(&kev, fd, filter, flags, 0, 0, watcher);

   if (kevent(loop->kqueuefd, &kev, 1, NULL, 0, NULL) == -1)
   {
      if (errno == EBADF)
      {
         /* File descriptor already closed */
         pgmoneta_log_debug("kevent: fd already closed: %s", strerror(errno));
      }
      else
      {
         pgmoneta_log_error("kevent error: %s", strerror(errno));
         return PGMONETA_EVENT_RC_ERROR;
      }
   }

   return PGMONETA_EVENT_RC_OK;
}

static int
ev_kqueue_io_stop(struct io_watcher* watcher)
{
   struct kevent kev;
   int filter = EVFILT_READ;

   EV_SET(&kev, watcher->fds.__fds[0], filter, EV_DELETE, 0, 0, NULL);
   if (kevent(loop->kqueuefd, &kev, 1, NULL, 0, NULL) == -1)
   {
      if (errno == EBADF || errno == ENOENT)
      {
         /* File descriptor already closed or event not found */
         pgmoneta_log_debug("%s: kevent delete on closed/invalid fd[0]: %s", __func__, strerror(errno));
      }
      else
      {
         pgmoneta_log_error("%s: kevent delete failed for fd[0]: %s", __func__, strerror(errno));
         return PGMONETA_EVENT_RC_ERROR;
      }
   }

   EV_SET(&kev, watcher->fds.__fds[1], filter, EV_DELETE, 0, 0, NULL);
   if (kevent(loop->kqueuefd, &kev, 1, NULL, 0, NULL) == -1)
   {
      if (errno == EBADF || errno == ENOENT)
      {
         /* File descriptor already closed or event not found */
         pgmoneta_log_debug("%s: kevent delete on closed/invalid fd[1]: %s", __func__, strerror(errno));
      }
      else
      {
         pgmoneta_log_error("%s: kevent delete failed for fd[1]: %s", __func__, strerror(errno));
         return PGMONETA_EVENT_RC_ERROR;
      }
   }

   return PGMONETA_EVENT_RC_OK;
}

static int
ev_kqueue_io_handler(struct kevent* kev)
{
   struct io_watcher* watcher = (struct io_watcher*)kev->udata;
   enum event_type type = watcher->event_watcher.type;
   int rc = PGMONETA_EVENT_RC_OK;

   switch (type)
   {
      case PGMONETA_EVENT_TYPE_MAIN:
         watcher->fds.main.client_fd = accept(watcher->fds.main.listen_fd, NULL, NULL);
         if (watcher->fds.main.client_fd == -1)
         {
            if (errno != EAGAIN && errno != EWOULDBLOCK)
            {
               pgmoneta_log_error("accept error: %s", strerror(errno));
               rc = PGMONETA_EVENT_RC_ERROR;
            }
         }
         else
         {
            watcher->cb(watcher);
         }
         break;
      case PGMONETA_EVENT_TYPE_WORKER:
         if (kev->flags & EV_EOF)
         {
            pgmoneta_log_debug("Connection closed on fd %d", watcher->fds.worker.rcv_fd);
            rc = PGMONETA_EVENT_RC_CONN_CLOSED;
         }
         else
         {
            watcher->cb(watcher);
         }
         break;
      default:
         pgmoneta_log_fatal("unknown event type: %d", type);
         return PGMONETA_EVENT_RC_FATAL;
   }
   return rc;
}

#endif /* HAVE_LINUX */

int
pgmoneta_signal_init(struct signal_watcher* watcher, signal_cb cb, int signum)
{
   watcher->event_watcher.type = PGMONETA_EVENT_TYPE_SIGNAL;
   watcher->signum = signum;
   watcher->cb = cb;
   return PGMONETA_EVENT_RC_OK;
}

int
pgmoneta_signal_start(struct signal_watcher* watcher)
{
   struct sigaction act;
   int signum = watcher->signum;

   if (event_loop_called_from_child("pgmoneta_signal_start"))
   {
      return PGMONETA_EVENT_RC_OK;
   }

   if (!loop)
   {
      pgmoneta_log_error("signal_start: loop is NULL");
      return PGMONETA_EVENT_RC_ERROR;
   }

   if (signum <= 0 || signum >= PGMONETA_NSIG)
   {
      pgmoneta_log_error("signal_start: invalid signum %d", signum);
      return PGMONETA_EVENT_RC_ERROR;
   }

   atomic_store_explicit(&signal_watchers[signum], watcher, memory_order_release);
   atomic_store_explicit(&signal_callbacks[signum], watcher->cb, memory_order_release);

   sigemptyset(&act.sa_mask);
   act.sa_sigaction = &signal_handler;
   act.sa_flags = SA_SIGINFO | SA_RESTART;
   if (sigaction(signum, &act, NULL) == -1)
   {
      pgmoneta_log_fatal("sigaction failed for signum %d", signum);
      return PGMONETA_EVENT_RC_ERROR;
   }
   if (sigaddset(&loop->sigset, signum) == -1)
   {
      pgmoneta_log_error("sigaddset error: %s", strerror(errno));
      return PGMONETA_EVENT_RC_ERROR;
   }

   return PGMONETA_EVENT_RC_OK;
}

int __attribute__((unused))
pgmoneta_signal_stop(struct signal_watcher* target)
{
   int rc = PGMONETA_EVENT_RC_OK;
   sigset_t tmp;

   if (event_loop_called_from_child("pgmoneta_signal_stop"))
   {
      return PGMONETA_EVENT_RC_OK;
   }

#ifdef DEBUG
   if (!target)
   {
      /* reaching here is a bug, do not recover */
      pgmoneta_log_fatal("BUG: target is NULL");
      exit(1);
   }
#endif

   sigemptyset(&tmp);
   sigaddset(&tmp, target->signum);
   if (loop && sigdelset(&loop->sigset, target->signum) == -1)
   {
      pgmoneta_log_error("sigdelset error: %s", strerror(errno));
      return PGMONETA_EVENT_RC_ERROR;
   }
   if (target->signum > 0 && target->signum < PGMONETA_NSIG)
   {
      signal_pending[target->signum] = 0;
      atomic_store_explicit(&signal_callbacks[target->signum], NULL, memory_order_release);
      atomic_store_explicit(&signal_watchers[target->signum], NULL, memory_order_release);
   }
#if !HAVE_LINUX
   /* XXX: FreeBSD catches SIGINT as soon as it is removed from
    * sigset. This could probably be improved */
   if (target->signum != SIGINT)
   {
#endif
      if (sigprocmask(SIG_UNBLOCK, &tmp, NULL) == -1)
      {
         pgmoneta_log_fatal("sigprocmask error: %s", strerror(errno));
         return PGMONETA_EVENT_RC_FATAL;
      }
#if !HAVE_LINUX
   }
#endif

   return rc;
}

static void
signal_handler(int signum, siginfo_t* si __attribute__((unused)), void* p __attribute__((unused)))
{
   if (signum < 0 || signum >= PGMONETA_NSIG)
   {
      return;
   }

   signal_pending[signum] = 1;
}

static void
dispatch_signal_callbacks(void)
{
   signal_cb cb;

   for (int signum = 1; signum < PGMONETA_NSIG; signum++)
   {
      if (!signal_pending[signum])
      {
         continue;
      }

      signal_pending[signum] = 0;
      cb = atomic_load_explicit(&signal_callbacks[signum], memory_order_acquire);
      if (cb)
      {
         cb();
      }
   }
}

static void
init_watcher_message(struct io_watcher* watcher)
{
   if (watcher->msg == NULL)
   {
      watcher->msg = calloc(1, sizeof(struct message));
      if (watcher->msg == NULL)
      {
         pgmoneta_log_fatal("failed to allocate message");
         exit(1);
      }
      watcher->msg->data = calloc(1, DEFAULT_BUFFER_SIZE);
      if (watcher->msg->data == NULL)
      {
         pgmoneta_log_fatal("failed to allocate message buffer");
         free(watcher->msg);
         watcher->msg = NULL;
         exit(1);
      }
      watcher->msg->length = 0;
      watcher->msg->kind = 0;
   }
}

struct message*
pgmoneta_get_watcher_message(struct io_watcher* watcher)
{
   init_watcher_message(watcher);
   return watcher->msg;
}
