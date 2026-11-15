#include "adder.h"

int64_t add(int64_t a, int64_t b) {
  return a + b;
}

int64_t scaled(int64_t value) {
  return add(value, 0) * SCALE;
}
