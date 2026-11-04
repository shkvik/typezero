#include "runtime/core/error/error.h"
#include "runtime/core/parallel/parallel.h"

void tz_set_failed(void *error) { tz_failed = error; }

void *tz_take_failed(void) {
  void *error = tz_failed;
  tz_failed = NULL;

  return error;
}
