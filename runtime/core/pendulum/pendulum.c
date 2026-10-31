#include "runtime/core/pendulum/pendulum.h"
#include "runtime/core/parallel/parallel.h"
#include "runtime/core/tzrt/tzrt.h"

#define WEIGHTS 16

TZ_THREAD_LOCAL TzWeight *tz_pendulum_weights[WEIGHTS];
TZ_THREAD_LOCAL int32_t tz_pendulum_count;

#if TZ_PARALLEL
void tz_trigger_hold(void);
#endif

TzHold tz_pendulum_hold(TzWeight *weight) {
  int32_t count = tz_pendulum_count;
  for (int32_t i = 0; i < count; ++i) {
    if (tz_pendulum_weights[i] == weight) {
      return TZ_HOLD_ALREADY;
    }
  }
  if (count == WEIGHTS) {
    return TZ_HOLD_FULL;
  }
  tz_pendulum_weights[count] = weight;
  tz_pendulum_count = count + 1;

  return TZ_HOLD_NEW;
}

int32_t tz_pendulum_holding(void) { return tz_pendulum_count; }

void tz_pendulum_swing(void) {
  if (!tz_pendulum_count) {
    return;
  }
#if TZ_PARALLEL
  int outside = tz_thread.worker && !tz_thread.job;
  if (outside) {
    tz_trigger_hold();
  }
#endif
  while (tz_pendulum_count) {
    TzWeight *weight = tz_pendulum_weights[--tz_pendulum_count];
    weight->release(weight);
  }
#if TZ_PARALLEL
  if (outside) {
    tz_ahead_settle_held();
  }
#endif
}
