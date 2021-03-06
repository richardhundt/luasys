/* Win32 */

#define WFD_READ	(FD_READ | FD_ACCEPT | FD_CLOSE)
#define WFD_WRITE	(FD_WRITE | FD_CONNECT | FD_CLOSE)


#include "win32iocp.c"
#include "win32thr.c"


EVQ_API int
evq_init (struct event_queue *evq)
{
  struct win32thr *wth = &evq->head;

  evq->ack_event = CreateEvent(NULL, FALSE, FALSE, NULL);
  if (evq->ack_event == NULL)
    return -1;

  wth->signal = CreateEvent(NULL, FALSE, FALSE, NULL);
  if (wth->signal == NULL) {
    CloseHandle(evq->ack_event);
    return -1;
  }
  wth->handles[0] = wth->signal;
  wth->evq = evq;

  InitCriticalSection(&wth->cs);

  if (is_WinNT) {
    evq->iocp.h = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
  }

  evq->now = sys_milliseconds();
  return 0;
}

EVQ_API void
evq_done (struct event_queue *evq)
{
  win32thr_poll(evq);

  if (is_WinNT) {
    CloseHandle(evq->iocp.h);
    win32iocp_done(evq);
  }

  CloseHandle(evq->ack_event);
  CloseHandle(evq->head.signal);
  DeleteCriticalSection(&evq->head.cs);
}

EVQ_API int
evq_add (struct event_queue *evq, struct event *ev)
{
  const unsigned int ev_flags = ev->flags;
  struct win32thr *wth = &evq->head;

  ev->wth = wth;

  if (ev_flags & EVENT_SIGNAL)
    return signal_add(evq, ev);

  if (ev_flags & EVENT_WINMSG) {
    evq->win_msg = ev;
    evq->nevents++;
    return 0;
  }

  if ((ev_flags & (EVENT_SOCKET | EVENT_SOCKET_ACC_CONN)) == EVENT_SOCKET
   && evq->iocp.h) {
    if (!CreateIoCompletionPort((HANDLE) ev->fd, evq->iocp.h, 0, 0)
     && GetLastError() != ERROR_INVALID_PARAMETER)  /* already assosiated */
      return -1;

    ev->flags |= EVENT_AIO;
    evq->iocp.n++;
    evq->nevents++;

    if (pSetFileCompletionNotificationModes
     && pSetFileCompletionNotificationModes((HANDLE) ev->fd, 3))
      ev->flags |= EVENT_AIO_SKIP;

    win32iocp_set(ev, ev_flags);
    return 0;
  }

  while (wth->n >= NEVENT - 1)
    if (!(wth = wth->next)) break;

  if (wth)
    win32thr_sleep(wth);
  else {
    wth = win32thr_init(evq);
    if (!wth) return -1;
  }

  return win32thr_add(wth, ev);
}

EVQ_API int
evq_add_dirwatch (struct event_queue *evq, struct event *ev, const char *path)
{
  const DWORD flags = FILE_NOTIFY_CHANGE_FILE_NAME
   | FILE_NOTIFY_CHANGE_DIR_NAME
   | FILE_NOTIFY_CHANGE_ATTRIBUTES
   | FILE_NOTIFY_CHANGE_SIZE
   | FILE_NOTIFY_CHANGE_LAST_WRITE
   | FILE_NOTIFY_CHANGE_CREATION
   | FILE_NOTIFY_CHANGE_SECURITY;
  const unsigned int filter = (ev->flags >> EVENT_EOF_SHIFT_RES)
   ? FILE_NOTIFY_CHANGE_LAST_WRITE : flags;
  HANDLE fd;

  ev->flags &= ~EVENT_EOF_MASK_RES;

  {
    void *os_path = utf8_to_filename(path);
    if (!os_path)
      return -1;

    fd = is_WinNT
     ? FindFirstChangeNotificationW(os_path, FALSE, filter)
     : FindFirstChangeNotificationA(os_path, FALSE, filter);

    free(os_path);
  }
  if (fd == NULL || fd == INVALID_HANDLE_VALUE)
    return -1;

  ev->fd = fd;
  return evq_add(evq, ev);
}

EVQ_API int
evq_set_timeout (struct event *ev, const msec_t msec)
{
  struct win32thr *wth = ev->wth;
  struct event_queue *evq = wth->evq;

  if (!(ev->flags & EVENT_ACTIVE))
    win32thr_sleep(wth);

  if (ev->tq) {
    if (ev->tq->msec == msec) {
      timeout_reset(ev, evq->now);
      return 0;
    }
    timeout_del(ev);
  }

  return (msec == TIMEOUT_INFINITE) ? 0
   : timeout_add(ev, msec, evq->now);
}

EVQ_API int
evq_add_timer (struct event_queue *evq, struct event *ev, const msec_t msec)
{
  ev->wth = &evq->head;
  if (!evq_set_timeout(ev, msec)) {
    evq->nevents++;
    return 0;
  }
  return -1;
}

EVQ_API int
evq_del (struct event *ev, const int reuse_fd)
{
  struct win32thr *wth = ev->wth;
  const unsigned int ev_flags = ev->flags;

  (void) reuse_fd;

  if (ev_flags & (EVENT_TIMER | EVENT_AIO | EVENT_SIGNAL | EVENT_WINMSG)) {
    struct event_queue *evq = wth->evq;

    if (ev->tq) timeout_del(ev);

    ev->wth = NULL;
    evq->nevents--;

    if (ev_flags & EVENT_AIO) {
      if (ev_flags & EVENT_PENDING)
        win32iocp_cancel(ev, (EVENT_READ | EVENT_WRITE));
      evq->iocp.n--;
      return 0;
    }

    if (ev_flags & EVENT_SIGNAL)
      return signal_del(ev);

    if (ev_flags & EVENT_WINMSG)
      evq->win_msg = NULL;

    /* EVENT_TIMER */
    return 0;
  }

  if (!(ev_flags & EVENT_ACTIVE))
    win32thr_sleep(wth);

  return win32thr_del(wth, ev);
}

EVQ_API int
evq_modify (struct event *ev, unsigned int flags)
{
  const unsigned int ev_flags = ev->flags;

  if (ev_flags & EVENT_AIO) {
    if (ev_flags & EVENT_PENDING)
      win32iocp_cancel(ev, (ev_flags & ~flags));
    return win32iocp_set(ev, flags);
  } else {
    struct win32thr *wth = ev->wth;
    int event = 0;

    if (!(ev_flags & EVENT_ACTIVE))
      win32thr_sleep(wth);

    if (flags & EVENT_READ)
      event = WFD_READ;
    if (flags & EVENT_WRITE)
      event |= WFD_WRITE;

    if (WSAEventSelect((int) ev->fd, wth->handles[ev->w.index], event)
     == SOCKET_ERROR)
      return -1;
  }
  return 0;
}

EVQ_API int
evq_wait (struct event_queue *evq, msec_t timeout)
{
  struct event *ev_ready = NULL;
  struct win32thr *wth = &evq->head;
  struct win32thr *threads = wth->next;
  CRITICAL_SECTION *head_cs = &wth->cs;
  HANDLE head_signal = wth->signal;
  int n = wth->n;
  int sig_ready = 0;
  DWORD wait_res;

  if (threads && win32thr_poll(evq) && evq_is_empty(evq))
    return 0;

  if (timeout != 0L) {
    timeout = timeout_get(wth->tq, timeout, evq->now);
    if (timeout == 0L) {
      ev_ready = timeout_process(wth->tq, NULL, evq->now);
      goto end;
    }
  }

  if (is_WinNT) {
    if (!iocp_is_empty(evq))
      ev_ready = win32iocp_process(evq, NULL, 0L);

    if (ev_ready) {
      evq->ev_ready = ev_ready;
      timeout = 0L;
    } else {
      /* head_signal is resetted by IOCP WSARecv/WSASend */
      EnterCriticalSection(head_cs);
      if (evq->sig_ready)
        timeout = 0L;
      LeaveCriticalSection(head_cs);
    }
  }

  sys_vm_leave();
  wait_res = MsgWaitForMultipleObjects(n + 1, wth->handles, FALSE, timeout,
   (evq->win_msg ? QS_ALLEVENTS : 0));
  sys_vm_enter();

  evq->now = sys_milliseconds();

  ev_ready = evq->ev_ready;

  if (wait_res == WAIT_TIMEOUT) {
    if (ev_ready) goto end;
    if (!wth->tq && !evq->sig_ready)
      return EVQ_TIMEOUT;
  }
  if (wait_res == (DWORD) (WAIT_OBJECT_0 + n + 1)) {
    struct event *ev = evq->win_msg;
    if (ev && !(ev->flags & EVENT_ACTIVE)) {
      ev->flags |= EVENT_ACTIVE;
      ev->next_ready = ev_ready;
      ev_ready = ev;
    }
    goto end;
  }
  if (wait_res == WAIT_FAILED)
    return EVQ_FAILED;

  timeout = evq->now;
  if (!iocp_is_empty(evq))
    ev_ready = win32iocp_process(evq, ev_ready, timeout);

  wth->idx = wait_res;

  if (threads) {
    EnterCriticalSection(head_cs);
    ResetEvent(head_signal);

    threads = evq->wth_ready;
    evq->wth_ready = NULL;

    sig_ready = evq->sig_ready;
    evq->sig_ready = 0;
    LeaveCriticalSection(head_cs);

    if (wait_res == (DWORD) (WAIT_OBJECT_0 + n))
      wth = threads;
    else
      wth->next_ready = threads;
  } else {
    wth->next_ready = NULL;

    if (evq->sig_ready) {
      EnterCriticalSection(head_cs);
      ResetEvent(head_signal);

      sig_ready = evq->sig_ready;
      evq->sig_ready = 0;
      LeaveCriticalSection(head_cs);
    }
  }

  if (sig_ready)
    ev_ready = signal_process_interrupt(evq, sig_ready, ev_ready, timeout);

  for (; wth; wth = wth->next_ready) {
    HANDLE *hp;  /* event handles */
    const int idx = wth->idx;
    int i;

    wth->state = WTHR_SLEEP;

    if (wth->tq) {
      if (idx == WAIT_TIMEOUT) {
        ev_ready = timeout_process(wth->tq, ev_ready, timeout);
        continue;
      }
    }

    hp = &wth->handles[idx];
    n = wth->n;
    i = idx;
    if (i >= n) continue;  /* some events deleted? */

    /* Traverse array of events */
    for (; ; ) {
      WSANETWORKEVENTS ne;
      struct event *ev = wth->events[i];
      const int ev_flags = ev->flags;
      unsigned int res = 0;

      if (!(ev_flags & EVENT_SOCKET)) {
        if (ev_flags & EVENT_PID) {
          DWORD status;
          GetExitCodeProcess(ev->fd, &status);
          res = (status << EVENT_EOF_SHIFT_RES);
        } else
          ResetEvent(ev->fd);  /* all events must be manual-reset */
        res |= EVENT_READ_RES;
      } else if (!WSAEnumNetworkEvents((int) ev->fd, *hp, &ne)) {
        if ((ev_flags & EVENT_READ)
         && (ne.lNetworkEvents & WFD_READ))
          res = EVENT_READ_RES;
        if ((ev_flags & EVENT_WRITE)
         && (ne.lNetworkEvents & WFD_WRITE))
          res |= EVENT_WRITE_RES;
        if (ne.lNetworkEvents & FD_CLOSE)
          res |= EVENT_EOF_RES;
      }

      if (res) {
        ev->flags |= EVENT_ACTIVE | res;
        ev->next_ready = ev_ready;
        ev_ready = ev;

        if (ev_flags & EVENT_ONESHOT) {
          win32thr_del(wth, ev);
          --i, --n, --hp;
        } else if (ev->tq && !(ev_flags & EVENT_TIMEOUT_MANUAL))
          timeout_reset(ev, timeout);
      }

      /* skip inactive events */
      do {
        if (++i == n)
          goto end_thread;
      } while (WaitForSingleObject(*(++hp), 0) != WAIT_OBJECT_0);
    }
 end_thread:
    ((void) 0);  /* avoid warning */
  }

  /* always check window messages */
  {
    struct event *ev = evq->win_msg;

    if (ev && GetQueueStatus(QS_ALLEVENTS)) {
      ev->next_ready = ev_ready;
      ev_ready = ev;
    }
  }
  if (!ev_ready)
    return (wait_res == WAIT_TIMEOUT && !sig_ready)
     ? EVQ_TIMEOUT : 0;
 end:
  evq->ev_ready = ev_ready;
  return 0;
}

