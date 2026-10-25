// The runtime's side of an AbortSignal (docs/async.md, section 7): what the
// waits of the runtime that a signal cancels (setTimeout and setInterval of
// node:timers/promises, readFile and writeFile of node:fs/promises, the
// questions and the interface of node:readline) listen to.
//
// AbortController and AbortSignal themselves are classes of tz's library,
// on EventTarget (src/lib/web.ts, docs/events.md, section 4). Each signal
// holds one of these, which the compiler passes where a call takes a
// `signal` (Sema::signalOption): the runtime checks whether it is aborted,
// and listens to its abort with a TzAbortListener. When the signal aborts,
// abortSignal() of web.ts marks this one aborted with the reason first, and
// its dispatch of the abort event runs the runtime's listeners here at the
// places they came among the listeners of the program: every listener, of
// the program or of the runtime, takes its number from one counter
// (tz_lib_order), and before the listener of the program numbered n, the
// runtime's listeners numbered below n run (tz_lib_signal_run). So an abort
// rejects what waits in the order Node's listeners would.
//
// The intrinsics of tz's library that cross to C (`function __tz_net_connect(
// ..., signal: AbortSignal | undefined)`, runtime/net.c and http.c) get the
// same: the runtime's side of the signal (CodeGen::emitExternal), which
// fetch and serve listen to with a TzAbortListener.

#include "tzrt.h"

#include <string.h>

extern TzErrorInfo tz_error_info;

static void release(void *object) { tz_instance_release((TzInstance *)object); }

// The type of the reason, for releasing it as a member does.
static const TzType tz_signal_reason_type = {TZ_T_UNKNOWN, sizeof(void *), NULL};

static TzStr ascii(const char *text) { return tz_str_from_ascii(text, strlen(text)); }

// The number of the last listener of a signal (see the top of the file).
int64_t tz_listen_order;

// `__tz_lib_order()`. The intrinsics take the maker of SystemError last,
// which these do not use.
double tz_lib_order(TzMaker make) {
  (void)make;
  return (double)++tz_listen_order;
}

// The runtime's side of a signal is a member of a group (group.c): the
// AbortSignal that holds it holds the reason too, and the reason may be an
// object of the program in the signal's group, which a reference from
// outside it would keep alive for good.
static void signal_drop(void *object) {
  TzSignal *s = (TzSignal *)object;
  // What the runtime listens with holds the signal, so no listener is left.
  if (s->reason)
    tz_value_release_in(s, &tz_signal_reason_type, &s->reason);
  tz_group_obj_free(s);
}

static const TzLayout tz_signal_layout = {signal_drop, NULL, 0, 0, "AbortSignal", 11, 0};

// `__tz_signal_new()`: the runtime's side of a new AbortSignal, a member of
// a group of its own with a count of 1.
TzSignal *tz_signal_new(TzMaker make) {
  (void)make;
  TzSignal *s = (TzSignal *)tz_group_alloc(sizeof(TzSignal));
  int64_t rc = s->rc;
  memset(s, 0, sizeof *s);
  s->rc = rc;
  s->layout = &tz_signal_layout;
  return s;
}

void tz_signal_listen(TzSignal *s, TzAbortListener *l) {
  l->signal = s;
  l->order = ++tz_listen_order;
  l->next = NULL;
  l->prev = s->tail;
  if (s->tail)
    s->tail->next = l;
  else
    s->head = l;
  s->tail = l;
}

static void unlink_listener(TzSignal *s, TzAbortListener *l) {
  if (l->prev)
    l->prev->next = l->next;
  else if (s->head == l)
    s->head = l->next;
  if (l->next)
    l->next->prev = l->prev;
  else if (s->tail == l)
    s->tail = l->prev;
  l->next = l->prev = NULL;
  l->signal = NULL;
}

void tz_signal_unlisten(TzAbortListener *l) {
  if (l->signal)
    unlink_listener(l->signal, l);
}

int32_t tz_signal_aborted(const void *signal) {
  return signal && ((const TzSignal *)signal)->aborted;
}

// `__tz_lib_signal_abort(native, reason)`: aborted, with the reason (an
// unknown, retained); before any listener runs.
void tz_lib_signal_abort(TzSignal *s, void *reason, TzMaker make) {
  (void)make;
  if (s->aborted)
    return;
  s->aborted = 1;
  if (reason) {
    // Its own now, inside its group if both are members.
    tz_ref_retain(reason);
    s->reason = reason;
    tz_group_store(s, reason);
  }
}

// `__tz_lib_signal_run(native, order)`: the runtime's listeners numbered
// below `order`, in the order they came; each is unlinked before it runs,
// and may unlink others.
void tz_lib_signal_run(TzSignal *s, double order, TzMaker make) {
  (void)make;
  tz_ref_retain(s);
  while (s->head && (double)s->head->order < order) {
    TzAbortListener *l = s->head;
    unlink_listener(s, l);
    l->run(l);
  }
  release(s);
}

// What rejects a fetch its signal aborted, as node rejects with the reason
// itself: the reason, owned, if it is an Error; else the TypeError that
// `throw` of it gives (tz throws only Errors).
void *tz_unknown_error(void *u);

void *tz_signal_reason(const void *signal) {
  void *reason = ((const TzSignal *)signal)->reason;
  if (reason)
    tz_ref_retain(reason); // an object of the program, which may be a member
  return tz_unknown_error(reason);
}

// `new AbortError(undefined, { cause: signal.reason })` of Node: what
// rejects what a signal cancelled. The cause of an Error of tz is an Error:
// a reason that is no Error is not kept.
void *tz_abort_error(const void *signal) {
  TzStr text = ascii("The operation was aborted");
  void *error = tz_error_info.make(TZ_ERROR, text.obj, text.meta);
  TzStr *name = (TzStr *)((char *)error + tz_error_info.name_offset);
  tz_str_release_value(*name);
  *name = ascii("AbortError");
  void *reason = ((const TzSignal *)signal)->reason;
  if (reason && (((TzInstance *)reason)->layout->flags & TZ_LAYOUT_ERROR)) {
    tz_ref_retain(reason); // an object of the program, which may be a member
    *(void **)((char *)error + tz_error_info.cause_offset) = reason;
  }
  return error;
}
