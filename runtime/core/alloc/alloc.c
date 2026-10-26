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

typedef struct Segment {
  uint32_t kind;
  uint32_t huge_units;
  struct Heap *owner;
  void *os_base;
  size_t os_size;
  uint64_t free_units;
  struct Segment *next, *prev;
  uint8_t first_unit[UNITS];
  Page pages[UNITS];
} Segment;

_Static_assert(sizeof(Segment) <= UNIT_SIZE, "segment metadata must fit in unit 0");

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

static void *os_alloc_aligned(size_t size, void **base, size_t *reserved) {
  size_t total = size + SEGMENT_SIZE;
#ifdef _WIN32
  char *p = (char *)VirtualAlloc(NULL, total, MEM_RESERVE, PAGE_READWRITE);
  if (!p) {
    return NULL;
  }
  char *aligned = (char *)(((uintptr_t)p + SEGMENT_SIZE - 1) & ~(uintptr_t)(SEGMENT_SIZE - 1));
  if (!VirtualAlloc(aligned, size, MEM_COMMIT, PAGE_READWRITE)) {
    VirtualFree(p, 0, MEM_RELEASE);
    return NULL;
  }
  *base = p;
  *reserved = total;

  return aligned;
#else
  char *p = (char *)mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) {
    return NULL;
  }
  char *aligned = (char *)(((uintptr_t)p + SEGMENT_SIZE - 1) & ~(uintptr_t)(SEGMENT_SIZE - 1));
  if (aligned > p) {
    munmap(p, (size_t)(aligned - p));
  }
  size_t tail = (size_t)(p + total - (aligned + size));
  if (tail) {
    munmap(aligned + size, tail);
  }
  *base = aligned;
  *reserved = size;

  return aligned;
#endif
}

static void os_free(void *base, size_t reserved) {
#ifdef _WIN32
  (void)reserved;
  VirtualFree(base, 0, MEM_RELEASE);
#else
  munmap(base, reserved);
#endif
}

TZ_NORETURN static void out_of_memory(size_t size) {
  tz_panicf("out of memory allocating %zu bytes", size);
}

static inline unsigned highest_bit(size_t x) {
#ifdef _MSC_VER
  unsigned long index;
  _BitScanReverse64(&index, (unsigned long long)x);

  return (unsigned)index;
#else

  return 63u - (unsigned)__builtin_clzll((unsigned long long)x);
#endif
}

static inline unsigned bin_of(size_t size) {
  if (size <= 64) {
    return size == 0 ? 0 : (unsigned)((size - 1) >> 3);
  }
  unsigned b = highest_bit(size - 1);

  return 8 + (b - 6) * 4 + (unsigned)(((size - 1) >> (b - 2)) & 3);
}

static inline size_t bin_size(unsigned bin) {
  if (bin < 8) {
    return ((size_t)bin + 1) * 8;
  }
  unsigned b = 6 + (bin - 8) / 4, sub = (bin - 8) % 4;

  return ((size_t)1 << b) + ((size_t)sub + 1) * ((size_t)1 << (b - 2));
}

static inline unsigned bin_units(unsigned bin) {
  size_t bytes = bin_size(bin) * 8;

  return bytes <= UNIT_SIZE ? 1 : (unsigned)((bytes + UNIT_SIZE - 1) / UNIT_SIZE);
}

static inline Segment *segment_of(const void *p) {
  return (Segment *)((uintptr_t)p & ~(uintptr_t)(SEGMENT_SIZE - 1));
}

static void segment_link(Segment *s) {
  s->prev = NULL;
  s->next = tz_heap.segments;
  if (tz_heap.segments) {
    tz_heap.segments->prev = s;
  }
  tz_heap.segments = s;
}

static void segment_unlink(Segment *s) {
  if (s->prev) {
    s->prev->next = s->next;
  } else {
    tz_heap.segments = s->next;
  }
  if (s->next) {
    s->next->prev = s->prev;
  }
}

static Segment *segment_new(void) {
  Segment *s = tz_heap.spare;
  if (s) {
    tz_heap.spare = NULL;
  } else {
    void *base;
    size_t reserved;
    s = (Segment *)os_alloc_aligned(SEGMENT_SIZE, &base, &reserved);
    if (!s) {
      out_of_memory(SEGMENT_SIZE);
    }
    s->kind = KIND_SMALL;
    s->owner = &tz_heap;
    s->os_base = base;
    s->os_size = reserved;
  }
  s->free_units = ALL_UNITS_FREE;
  segment_link(s);

  return s;
}

static unsigned find_units(const Segment *s, unsigned n) {
  uint64_t mask = n >= 64 ? ~(uint64_t)0 : (((uint64_t)1 << n) - 1);
  for (unsigned u = 1; u + n <= UNITS; ++u) {
    if (((s->free_units >> u) & mask) == mask) {
      return u;
    }
  }

  return 0;
}

static void bin_push(Page *page) {
  Page **head = &tz_heap.bins[page->bin];
  page->prev = NULL;
  page->next = *head;
  if (*head) {
    (*head)->prev = page;
  }
  *head = page;
  page->listed = 1;
}

static void bin_remove(Page *page) {
  if (page->prev) {
    page->prev->next = page->next;
  } else {
    tz_heap.bins[page->bin] = page->next;
  }
  if (page->next) {
    page->next->prev = page->prev;
  }
  page->listed = 0;
}

static Page *page_new(unsigned bin) {
  unsigned units = bin_units(bin);
  Segment *s = tz_heap.segments;
  unsigned first = 0;
  for (; s; s = s->next) {
    if ((first = find_units(s, units)) != 0) {
      break;
    }
  }
  if (!s) {
    s = segment_new();
    first = 1;
  }
  uint64_t mask = (units >= 64 ? ~(uint64_t)0 : (((uint64_t)1 << units) - 1)) << first;
  s->free_units &= ~mask;
  for (unsigned u = first; u < first + units; ++u) {
    s->first_unit[u] = (uint8_t)first;
  }

  Page *page = &s->pages[first];
  size_t block = bin_size(bin);
  char *start = (char *)s + (size_t)first * UNIT_SIZE;
  page->free = NULL;
  page->bump = start;
  page->end = start + ((size_t)units * UNIT_SIZE / block) * block;
  page->used = 0;
  page->block_size = (uint32_t)block;
  page->bin = (uint8_t)bin;
  page->units = (uint8_t)units;
  bin_push(page);

  return page;
}

static void page_release(Page *page) {
  bin_remove(page);
  Segment *s = segment_of(page);
  unsigned first = (unsigned)(page - s->pages);
  uint64_t mask = (page->units >= 64 ? ~(uint64_t)0 : (((uint64_t)1 << page->units) - 1)) << first;
  s->free_units |= mask;
  if (s->free_units != ALL_UNITS_FREE) {
    return;
  }
  segment_unlink(s);
  if (!tz_heap.spare) {
    tz_heap.spare = s;
  } else {
    os_free(s->os_base, s->os_size);
  }
}

static void *huge_alloc(size_t size) {
  size_t total = (HUGE_HEADER + size + UNIT_SIZE - 1) & ~(UNIT_SIZE - 1);
  if (total < size) {
    out_of_memory(size);
  }
  size_t units = total / UNIT_SIZE;
  for (unsigned i = 0; i < HUGE_SPARES; ++i) {
    Segment *s = tz_heap.huge_spares[i];
    if (s && s->huge_units >= units && s->huge_units <= 2 * units) {
      tz_heap.huge_spares[i] = NULL;
      return (char *)s + HUGE_HEADER;
    }
  }
  void *base;
  size_t reserved;
  Segment *s = (Segment *)os_alloc_aligned(total, &base, &reserved);
  if (!s) {
    out_of_memory(size);
  }
  s->kind = KIND_HUGE;
  s->owner = NULL;
  s->huge_units = units <= UINT32_MAX ? (uint32_t)units : UINT32_MAX;
  s->os_base = base;
  s->os_size = reserved;

  return (char *)s + HUGE_HEADER;
}

void tz_heap_collect(void);

TZ_NOINLINE void *tz_alloc_slow(size_t size) {
  if (size > MAX_SMALL) {
    return huge_alloc(size);
  }
#if TZ_PARALLEL
  if (__atomic_load_n(&tz_heap.remote, __ATOMIC_RELAXED)) {
    tz_heap_collect();
    if (tz_heap.bins[bin_of(size)]) {
      return tz_alloc(size);
    }
  }
#endif
  unsigned bin = bin_of(size);
  Page *page = tz_heap.bins[bin];
  while (page && !page->free && page->bump >= page->end) {
    bin_remove(page);
    page = tz_heap.bins[bin];
  }
  if (!page) {
    page = page_new(bin);
  }
  void *block = page->free;
  if (block) {
    page->free = *(void **)block;
  } else {
    block = page->bump;
    page->bump += page->block_size;
  }
  ++page->used;

  return block;
}

TZ_HOT void *tz_alloc(size_t size) {
  if (size <= MAX_SMALL) {
    Page *page = tz_heap.bins[bin_of(size)];
    if (page) {
      void *block = page->free;
      if (block) {
        page->free = *(void **)block;
        ++page->used;
        return block;
      }
      if (page->bump < page->end) {
        block = page->bump;
        page->bump += page->block_size;
        ++page->used;
        return block;
      }
    }
  }

  return tz_alloc_slow(size);
}

static inline Page *page_of(Segment *s, const void *p) {
  unsigned unit = (unsigned)(((uintptr_t)p - (uintptr_t)s) >> UNIT_SHIFT);

  return &s->pages[s->first_unit[unit]];
}

TZ_NOINLINE void tz_free_slow(void *p) {
  Segment *s = segment_of(p);
  if (s->kind == KIND_HUGE) {
    if (s->huge_units <= HUGE_SPARE_UNITS) {
      for (unsigned i = 0; i < HUGE_SPARES; ++i) {
        if (!tz_heap.huge_spares[i]) {
          tz_heap.huge_spares[i] = s;
          return;
        }
      }
    }
    os_free(s->os_base, s->os_size);
    return;
  }
  Page *page = page_of(s, p);
  if (!page->listed) {
    bin_push(page);
  } else if (page->used == 0 && (page->next || page->prev)) {
    page_release(page);
  }
}

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

TZ_HOT void tz_free(void *p) {
  if (!p) {
    return;
  }
  Segment *s = segment_of(p);
  if (s->kind == KIND_SMALL) {
#if TZ_PARALLEL
    if (s->owner != &tz_heap) {
      tz_free_remote(p);
      return;
    }
#endif
    Page *page = page_of(s, p);
    *(void **)p = page->free;
    page->free = p;
    if (--page->used != 0 && page->listed) {
      return;
    }
  }
  tz_free_slow(p);
}

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
