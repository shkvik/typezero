#include "runtime/library/process/signals/signals.h"
#include "runtime/core/closure/closure.h"
#include "runtime/core/group/group.h"
#include "runtime/core/io/io.h"
#include "runtime/core/output/output.h"
#include <stdlib.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

enum { SIGNAL_HUP = 1, SIGNAL_INT = 2, SIGNAL_TERM = 15, SIGNAL_BREAK = 21, SIGNAL_COUNT = 32 };

typedef struct {
  TzWork work;
  int32_t signal;
} SignalWork;

TzFunction tz_signal_dispatch;
volatile int32_t tz_signal_watched[SIGNAL_COUNT];

static void signal_done(TzWork *w) {
  SignalWork *s = (SignalWork *)w;
  int32_t signal = s->signal;
  free(s);
  TzFunction run = tz_signal_dispatch;
  if (!run.info || !tz_signal_watched[signal]) {
    return;
  }
  tz_obj_retain(run.env);
  ((void (*)(TzObject *, int64_t))run.info->code)(run.env, signal);
  tz_obj_release(run.env);
}

#ifdef _WIN32

HANDLE tz_signal_port;

static int32_t signal_of(DWORD type) {
  switch (type) {
  case CTRL_C_EVENT:
    return SIGNAL_INT;
  case CTRL_BREAK_EVENT:
    return SIGNAL_BREAK;
  case CTRL_CLOSE_EVENT:
    return SIGNAL_HUP;
  default:
    return 0;
  }
}

static int signal_post(int32_t signal) {
  HANDLE port = tz_signal_port;
  if (!signal || !port || !tz_signal_watched[signal]) {
    return 0;
  }
  SignalWork *s = (SignalWork *)calloc(1, sizeof(SignalWork));
  if (!s) {
    return 0;
  }
  s->work.done = signal_done;
  s->work.turns = 1;
  s->work.unref = 1;
  s->signal = signal;
  if (!PostQueuedCompletionStatus(port, 0, 0, (LPOVERLAPPED)&s->work)) {
    free(s);
    return 0;
  }

  return 1;
}

static BOOL WINAPI on_console(DWORD type) {
  int32_t signal = signal_of(type);
  if (signal_post(signal)) {
    if (signal == SIGNAL_HUP) {
      Sleep(5000);
    }
    return TRUE;
  }
  tz_out_rescue();

  return FALSE;
}

void tz_signal_init(void) { SetConsoleCtrlHandler(on_console, TRUE); }

void tz_signal_listen(const void *info, void *env, TzMaker make) {
  (void)make;
  if (!tz_signal_port) {
    tz_signal_port = tz_io_port_handle();
  }
  tz_obj_retain((TzObject *)env);
  TzFunction old = tz_signal_dispatch;
  tz_signal_dispatch.info = (const TzFunctionInfo *)info;
  tz_signal_dispatch.env = (TzObject *)env;
  tz_obj_release(old.env);
}

#else

void tz_signal_init(void) {}

void tz_signal_listen(const void *info, void *env, TzMaker make) {
  (void)make;
  tz_obj_retain((TzObject *)env);
  TzFunction old = tz_signal_dispatch;
  tz_signal_dispatch.info = (const TzFunctionInfo *)info;
  tz_signal_dispatch.env = (TzObject *)env;
  tz_obj_release(old.env);
}

#endif

void tz_signal_watch(int64_t signal, int32_t watched, TzMaker make) {
  (void)make;
  if (signal > 0 && signal < SIGNAL_COUNT) {
    tz_signal_watched[signal] = watched != 0;
  }
}

void tz_signal_shutdown(void) {
  TzFunction old = tz_signal_dispatch;
  tz_signal_dispatch.info = NULL;
  tz_signal_dispatch.env = NULL;
  tz_obj_release(old.env);
}
