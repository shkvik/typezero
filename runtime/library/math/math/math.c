#include "runtime/core/output/output.h"
#include "runtime/core/error/error.h"
#include "runtime/core/tzrt/tzrt.h"
#include <math.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <time.h>
#include <unistd.h>
#endif

TZ_HOT double tz_math_pow(double x, double y) {
  if (isnan(y) || (fabs(x) == 1 && isinf(y))) {
    return NAN;
  }

  return pow(x, y);
}

TZ_COLD TZ_NORETURN void tz_panic_zero_power(void) { tz_panicf("Division by zero"); }

TZ_HOT int64_t tz_math_pow_long(int64_t base, int64_t exponent) {
  if (exponent < 0) {
    if (base == 0) {
      tz_panic_zero_power();
      return 0;
    }
    if (base == 1 || base == -1) {
      return (exponent & 1) ? base : 1;
    }
    return 0;
  }
  uint64_t result = 1, factor = (uint64_t)base;
  for (uint64_t e = (uint64_t)exponent; e; e >>= 1) {
    if (e & 1) {
      result *= factor;
    }
    factor *= factor;
  }

  return (int64_t)result;
}

#define MATH1(name)                                                                                \
  TZ_HOT double tz_math_##name(double x) { return name(x); }

MATH1(cbrt)
MATH1(exp)
MATH1(expm1)
MATH1(log)
MATH1(log1p)
MATH1(log2)
MATH1(log10)
MATH1(sin)
MATH1(cos)
MATH1(tan)
MATH1(asin)
MATH1(acos)
MATH1(atan)
MATH1(sinh)
MATH1(cosh)
MATH1(tanh)
MATH1(asinh)
MATH1(acosh)
MATH1(atanh)

TZ_HOT double tz_math_atan2(double y, double x) { return atan2(y, x); }

double tz_math_hypot(const double *values, int64_t n) {
  double max = 0;
  int nan = 0;
  for (int64_t i = 0; i < n; ++i) {
    double v = fabs(values[i]);
    if (isnan(v)) {
      nan = 1;
    } else if (v > max) {
      max = v;
    }
  }
  if (isinf(max)) {
    return INFINITY;
  }
  if (nan) {
    return NAN;
  }
  if (max == 0) {
    return 0;
  }
  double sum = 0, compensation = 0;
  for (int64_t i = 0; i < n; ++i) {
    double scaled = fabs(values[i]) / max;
    double summand = scaled * scaled - compensation;
    double preliminary = sum + summand;
    compensation = (preliminary - sum) - summand;
    sum = preliminary;
  }

  return sqrt(sum) * max;
}

TZ_THREAD_LOCAL uint64_t tz_random_state[2];

static uint64_t splitmix64(uint64_t *x) {
  uint64_t z = (*x += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;

  return z ^ (z >> 31);
}

TZ_NOINLINE void tz_random_seed(void) {
  uint64_t seed = (uint64_t)(uintptr_t)&seed;
#ifdef _WIN32
  LARGE_INTEGER counter;
  QueryPerformanceCounter(&counter);
  seed ^= (uint64_t)counter.QuadPart ^ ((uint64_t)GetCurrentProcessId() << 32);
#else
  struct timespec now;
  clock_gettime(CLOCK_REALTIME, &now);
  seed ^= (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
  seed ^= (uint64_t)getpid() << 32;
#endif
  tz_random_state[0] = splitmix64(&seed);
  tz_random_state[1] = splitmix64(&seed);
  if ((tz_random_state[0] | tz_random_state[1]) == 0) {
    tz_random_state[0] = 1;
  }
}

TZ_HOT double tz_math_random(void) {
  if ((tz_random_state[0] | tz_random_state[1]) == 0) {
    tz_random_seed();
  }
  uint64_t s1 = tz_random_state[0];
  uint64_t s0 = tz_random_state[1];
  tz_random_state[0] = s0;
  s1 ^= s1 << 23;
  tz_random_state[1] = s1 ^ s0 ^ (s1 >> 17) ^ (s0 >> 26);

  return (double)((tz_random_state[1] + s0) >> 11) * (1.0 / 9007199254740992.0);
}
