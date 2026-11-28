#include "runtime/core/unicode/unicode.h"
#include <stddef.h>
#include <stdint.h>

typedef struct {
  uint32_t start, end;
  uint32_t stride;
  int32_t delta;
} CaseRun;

typedef struct {
  uint32_t cp;
  uint32_t count;
  uint32_t to[3];
} CaseSpecial;

typedef struct {
  uint32_t start, end;
} CodeRange;

#include "runtime/core/unicode/unicode_tables.h"

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

static int in_ranges(const CodeRange *ranges, size_t n, uint32_t cp) {
  size_t lo = 0, hi = n;
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    if (ranges[mid].end < cp) {
      lo = mid + 1;
    } else if (ranges[mid].start > cp) {
      hi = mid;
    } else {
      return 1;
    }
  }

  return 0;
}

