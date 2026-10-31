#ifndef TZ_PENDULUM_H
#define TZ_PENDULUM_H

#include <stdint.h>

typedef struct TzWeight {
  void (*release)(struct TzWeight *self);
} TzWeight;

typedef enum { TZ_HOLD_NEW, TZ_HOLD_ALREADY, TZ_HOLD_FULL } TzHold;

TzHold tz_pendulum_hold(TzWeight *weight);
void tz_pendulum_swing(void);
int32_t tz_pendulum_holding(void);

#endif
