#include "runtime/library/async_hooks/async_hooks/async_hooks.h"
#include "runtime/core/group/group.h"
#include "runtime/core/parallel/parallel.h"
#include "runtime/core/publish/publish.h"

void *tz_context_current(TzMaker make) {
  (void)make;
  TzObject *context = (TzObject *)tz_thread.context;
  if (context) {
    tz_ref_retain(context);
  }

  return context;
}

void *tz_context_enter(void *frame) {
  tz_publish_object(frame);
  void *outer = tz_thread.context;
  tz_thread.context = frame;

  return outer;
}

void tz_context_leave(void *outer) { tz_thread.context = outer; }
