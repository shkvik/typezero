// Unicode case mapping for toUpperCase and toLowerCase, from the tables in
// unicode_tables.h (see gen_unicode_tables.mjs).

#include "runtime/core/tzrt/tzrt.h"

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

static const CaseRun *find_run(const CaseRun *runs, size_t n, uint32_t cp) {
  // The last run that starts at or before cp.
  size_t lo = 0, hi = n;
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    if (runs[mid].start <= cp) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo == 0) {
    return NULL;
  }
  const CaseRun *run = &runs[lo - 1];
  if (cp > run->end || (cp - run->start) % run->stride != 0) {
    return NULL;
  }

  return run;
}

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

// Maps `cp` to upper or lower case: one code point, or up to three (such as
// ß to SS). Returns how many it wrote to `out`.
int tz_case_map(uint32_t cp, int upper, uint32_t out[3]) {
  // A code point is either in the special list (sorted) or in the runs.
  const CaseSpecial *special = upper ? upper_special : lower_special;
  size_t lo = 0, hi = upper ? COUNT(upper_special) : COUNT(lower_special);
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    if (special[mid].cp < cp) {
      lo = mid + 1;
    } else if (special[mid].cp > cp) {
      hi = mid;
    } else {
      for (uint32_t k = 0; k < special[mid].count; ++k) {
        out[k] = special[mid].to[k];
      }
      return (int)special[mid].count;
    }
  }
  const CaseRun *run = upper ? find_run(upper_runs, COUNT(upper_runs), cp)
                             : find_run(lower_runs, COUNT(lower_runs), cp);
  out[0] = run ? (uint32_t)((int32_t)cp + run->delta) : cp;

  return 1;
}

int tz_is_cased(uint32_t cp) { return in_ranges(cased_ranges, COUNT(cased_ranges), cp); }

int tz_is_case_ignorable(uint32_t cp) {
  return in_ranges(case_ignorable_ranges, COUNT(case_ignorable_ranges), cp);
}

// WhiteSpace and LineTerminator of JavaScript, which trim() removes.
int tz_is_js_space(uint32_t unit) {
  switch (unit) {
  case 0x09:
  case 0x0A:
  case 0x0B:
  case 0x0C:
  case 0x0D:
  case 0x20:
  case 0xA0:
  case 0x1680:
  case 0x2028:
  case 0x2029:
  case 0x202F:
  case 0x205F:
  case 0x3000:
  case 0xFEFF:
    return 1;
  default:
    return unit >= 0x2000 && unit <= 0x200A;
  }
}
