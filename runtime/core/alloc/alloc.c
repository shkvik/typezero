#include "runtime/core/alloc/alloc.h"
#include "runtime/core/output/output.h"
#include "runtime/core/parallel/parallel.h"
#include "runtime/core/tzrt/tzrt.h"
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <intrin.h>
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#define SEGMENT_SHIFT 22
#define SEGMENT_SIZE ((size_t)1 << SEGMENT_SHIFT)
#define UNIT_SHIFT 16
#define UNIT_SIZE ((size_t)1 << UNIT_SHIFT)
#define UNITS (SEGMENT_SIZE / UNIT_SIZE)
#define MAX_SMALL ((size_t)32 << 10)
#define HUGE_HEADER 64
#define HUGE_SPARES TZ_HEAP_HUGE_SPARES
#define HUGE_SPARE_UNITS 16
#define BINS TZ_HEAP_BINS

enum { KIND_SMALL = 1, KIND_HUGE = 2 };

typedef struct Page {
  void *free;
  char *bump;
  char *end;
  uint32_t used;
  uint32_t block_size;
  uint8_t bin;
  uint8_t units;
  uint8_t listed;
  struct Page *next, *prev;
} Page;

struct Heap;

#define WEAK_WORDS (SEGMENT_SIZE / 16 / 64)

typedef struct Segment {
  uint32_t kind;
  uint32_t huge_units;
  struct Heap *owner;
  void *os_base;
  size_t os_size;
  uint64_t free_units;
  struct Segment *next, *prev;
  uint64_t huge_weak;
  uint8_t first_unit[UNITS];
  Page pages[UNITS];
  uint64_t weak[WEAK_WORDS];
} Segment;

_Static_assert(sizeof(Segment) <= UNIT_SIZE, "segment metadata must fit in unit 0");
_Static_assert(offsetof(Segment, huge_weak) + sizeof(uint64_t) <= HUGE_HEADER,
               "the weak flag of a huge object must fit in its header");

#define ALL_UNITS_FREE (~(uint64_t)1)

typedef struct Heap {
  Page *bins[BINS];
  Segment *segments;
  Segment *spare;
  Segment *huge_spares[HUGE_SPARES];
  void *remote;
} Heap;

_Static_assert(sizeof(Heap) == sizeof(TzHeap), "Heap must match TzHeap");

#define tz_heap (*(Heap *)&tz_thread.heap)

static inline Segment *segment_of(const void *p) {
  return (Segment *)((uintptr_t)p & ~(uintptr_t)(SEGMENT_SIZE - 1));
}

void tz_heap_collect(void);

#if TZ_PARALLEL
TZ_NOINLINE void tz_free_remote(void *p) {
  Heap *owner = segment_of(p)->owner;
  void *head = __atomic_load_n(&owner->remote, __ATOMIC_RELAXED);
  do {
    *(void **)p = head;
  } while (!__atomic_compare_exchange_n(&owner->remote, &head, p, 1, __ATOMIC_RELEASE,
                                        __ATOMIC_RELAXED));
}
#endif

void tz_heap_collect(void) {
#if TZ_PARALLEL
  void *block = __atomic_exchange_n(&tz_heap.remote, NULL, __ATOMIC_ACQUIRE);
  while (block) {
    void *next = *(void **)block;
    tz_free(block);
    block = next;
  }
#endif
}

static inline uint64_t *weak_word(void *p, uint64_t *bit) {
  Segment *s = segment_of(p);
  if (s->kind == KIND_HUGE) {
    *bit = 1;
    return &s->huge_weak;
  }
  size_t index = ((uintptr_t)p - (uintptr_t)s) >> 4;
  *bit = (uint64_t)1 << (index & 63);

  return &s->weak[index >> 6];
}

void tz_weak_mark(void *p) {
  uint64_t bit;
  uint64_t *word = weak_word(p, &bit);
  __atomic_fetch_or(word, bit, __ATOMIC_RELEASE);
}

void tz_weak_unmark(void *p) {
  uint64_t bit;
  uint64_t *word = weak_word(p, &bit);
  __atomic_fetch_and(word, ~bit, __ATOMIC_RELEASE);
}

int tz_weak_marked(void *p) {
  uint64_t bit;
  uint64_t *word = weak_word(p, &bit);

  return (__atomic_load_n(word, __ATOMIC_ACQUIRE) & bit) != 0;
}
