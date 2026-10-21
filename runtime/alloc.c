// The memory allocator of the runtime: every string, array and buffer comes
// from here. It follows the design of mimalloc, simplified for one thread:
//
//   segment  4 MiB of address space from the OS, aligned to 4 MiB. Unit 0
//            holds the segment's metadata, units 1..63 (64 KiB each) hold
//            pages.
//   page     a run of units holding blocks of one size class. A page hands
//            out never-used blocks by bumping a pointer, then reuses freed
//            blocks from its free list.
//   huge     an object above 32 KiB gets its own OS allocation, also aligned
//            to 4 MiB, with a small header.
//
// Because segments are aligned, tz_free finds the page of any block by
// masking its address, without being told its size. Allocating or freeing a
// small block is a handful of instructions in the common case, which matters
// for strings: a program creates millions of them, and the C library's
// malloc costs several times more per call.
//
// Parallel async will give each thread its own heap (the `bins` below);
// a block freed by another thread will then go through an atomic list of its
// page, as in mimalloc.

#include "tzrt.h"

#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <intrin.h>
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#define SEGMENT_SHIFT 22
#define SEGMENT_SIZE ((size_t)1 << SEGMENT_SHIFT) // 4 MiB
#define UNIT_SHIFT 16
#define UNIT_SIZE ((size_t)1 << UNIT_SHIFT) // 64 KiB
#define UNITS (SEGMENT_SIZE / UNIT_SIZE)   // 64
#define MAX_SMALL ((size_t)32 << 10)       // larger objects are huge
#define HUGE_HEADER 64                     // keeps huge blocks 64-byte aligned
// Size classes: multiples of 8 up to 64, then four per power of two.
#define BINS 44

enum { KIND_SMALL = 1, KIND_HUGE = 2 };

typedef struct Page {
  void *free;     // freed blocks, linked through their first word
  char *bump;     // the next block never handed out
  char *end;      // the end of the page's blocks
  uint32_t used;  // blocks handed out and not freed
  uint32_t block_size;
  uint8_t bin;
  uint8_t units;  // the length of the run of units
  uint8_t listed; // in its bin's list, which holds pages with room
  struct Page *next, *prev;
} Page;

typedef struct Segment {
  uint32_t kind;
  uint32_t unused;
  void *os_base; // what to give back to the OS
  size_t os_size;
  uint64_t free_units; // bit u set: unit u holds no page
  struct Segment *next, *prev;
  uint8_t first_unit[UNITS]; // for each unit of a page, the page's first unit
  Page pages[UNITS];         // indexed by the first unit of a page
} Segment;

_Static_assert(sizeof(Segment) <= UNIT_SIZE, "segment metadata must fit in unit 0");

// Units 1..63 free, unit 0 taken by the metadata.
#define ALL_UNITS_FREE (~(uint64_t)1)

static struct {
  Page *bins[BINS];   // pages with room, per size class
  Segment *segments;  // small segments, those with free units first
  Segment *spare;     // one empty segment kept for reuse
} heap;

//===----------------------------------------------------------------------===//
// The operating system
//===----------------------------------------------------------------------===//

// `size` bytes of zeroed memory aligned to SEGMENT_SIZE.
static void *os_alloc_aligned(size_t size, void **base, size_t *reserved) {
  size_t total = size + SEGMENT_SIZE;
#ifdef _WIN32
  // Reserve enough address space to find an aligned start in it, and commit
  // only the aligned part. The rest stays reserved, which costs no memory.
  char *p = (char *)VirtualAlloc(NULL, total, MEM_RESERVE, PAGE_READWRITE);
  if (!p)
    return NULL;
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
  if (p == MAP_FAILED)
    return NULL;
  char *aligned = (char *)(((uintptr_t)p + SEGMENT_SIZE - 1) & ~(uintptr_t)(SEGMENT_SIZE - 1));
  // Give back the unaligned head and the tail.
  if (aligned > p)
    munmap(p, (size_t)(aligned - p));
  size_t tail = (size_t)(p + total - (aligned + size));
  if (tail)
    munmap(aligned + size, tail);
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

//===----------------------------------------------------------------------===//
// Size classes
//===----------------------------------------------------------------------===//

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
  if (size <= 64)
    return size == 0 ? 0 : (unsigned)((size - 1) >> 3);
  unsigned b = highest_bit(size - 1); // size - 1 is in [2^b, 2^(b+1))
  return 8 + (b - 6) * 4 + (unsigned)(((size - 1) >> (b - 2)) & 3);
}

static inline size_t bin_size(unsigned bin) {
  if (bin < 8)
    return ((size_t)bin + 1) * 8;
  unsigned b = 6 + (bin - 8) / 4, sub = (bin - 8) % 4;
  return ((size_t)1 << b) + ((size_t)sub + 1) * ((size_t)1 << (b - 2));
}

// Units per page: enough for at least eight blocks.
static inline unsigned bin_units(unsigned bin) {
  size_t bytes = bin_size(bin) * 8;
  return bytes <= UNIT_SIZE ? 1 : (unsigned)((bytes + UNIT_SIZE - 1) / UNIT_SIZE);
}

static inline Segment *segment_of(const void *p) {
  return (Segment *)((uintptr_t)p & ~(uintptr_t)(SEGMENT_SIZE - 1));
}

//===----------------------------------------------------------------------===//
// Segments and pages
//===----------------------------------------------------------------------===//

static void segment_link(Segment *s) {
  s->prev = NULL;
  s->next = heap.segments;
  if (heap.segments)
    heap.segments->prev = s;
  heap.segments = s;
}

static void segment_unlink(Segment *s) {
  if (s->prev)
    s->prev->next = s->next;
  else
    heap.segments = s->next;
  if (s->next)
    s->next->prev = s->prev;
}

static Segment *segment_new(void) {
  Segment *s = heap.spare;
  if (s) {
    heap.spare = NULL;
  } else {
    void *base;
    size_t reserved;
    s = (Segment *)os_alloc_aligned(SEGMENT_SIZE, &base, &reserved);
    if (!s)
      out_of_memory(SEGMENT_SIZE);
    s->kind = KIND_SMALL;
    s->os_base = base;
    s->os_size = reserved;
  }
  s->free_units = ALL_UNITS_FREE;
  segment_link(s);
  return s;
}

// The first run of `n` free units in `s`, or 0.
static unsigned find_units(const Segment *s, unsigned n) {
  uint64_t mask = n >= 64 ? ~(uint64_t)0 : (((uint64_t)1 << n) - 1);
  for (unsigned u = 1; u + n <= UNITS; ++u)
    if (((s->free_units >> u) & mask) == mask)
      return u;
  return 0;
}

static void bin_push(Page *page) {
  Page **head = &heap.bins[page->bin];
  page->prev = NULL;
  page->next = *head;
  if (*head)
    (*head)->prev = page;
  *head = page;
  page->listed = 1;
}

static void bin_remove(Page *page) {
  if (page->prev)
    page->prev->next = page->next;
  else
    heap.bins[page->bin] = page->next;
  if (page->next)
    page->next->prev = page->prev;
  page->listed = 0;
}

static Page *page_new(unsigned bin) {
  unsigned units = bin_units(bin);
  Segment *s = heap.segments;
  unsigned first = 0;
  for (; s; s = s->next)
    if ((first = find_units(s, units)) != 0)
      break;
  if (!s) {
    s = segment_new();
    first = 1;
  }
  uint64_t mask = (units >= 64 ? ~(uint64_t)0 : (((uint64_t)1 << units) - 1)) << first;
  s->free_units &= ~mask;
  for (unsigned u = first; u < first + units; ++u)
    s->first_unit[u] = (uint8_t)first;

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

// Gives the units of an empty page back to its segment, and an empty segment
// back to the OS (keeping one for reuse).
static void page_release(Page *page) {
  bin_remove(page);
  Segment *s = segment_of(page);
  unsigned first = (unsigned)(page - s->pages);
  uint64_t mask = (page->units >= 64 ? ~(uint64_t)0 : (((uint64_t)1 << page->units) - 1)) << first;
  s->free_units |= mask;
  if (s->free_units != ALL_UNITS_FREE)
    return;
  segment_unlink(s);
  if (!heap.spare)
    heap.spare = s;
  else
    os_free(s->os_base, s->os_size);
}

//===----------------------------------------------------------------------===//
// Allocation
//===----------------------------------------------------------------------===//

static void *huge_alloc(size_t size) {
  size_t total = (HUGE_HEADER + size + UNIT_SIZE - 1) & ~(UNIT_SIZE - 1);
  if (total < size)
    out_of_memory(size);
  void *base;
  size_t reserved;
  Segment *s = (Segment *)os_alloc_aligned(total, &base, &reserved);
  if (!s)
    out_of_memory(size);
  s->kind = KIND_HUGE;
  s->os_base = base;
  s->os_size = reserved;
  return (char *)s + HUGE_HEADER;
}

static void *alloc_slow(unsigned bin) {
  // Pages at the head of the list without room are full: drop them from it
  // until one has room, or start a new page.
  Page *page = heap.bins[bin];
  while (page && !page->free && page->bump >= page->end) {
    bin_remove(page);
    page = heap.bins[bin];
  }
  if (!page)
    page = page_new(bin);
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

void *tz_alloc(size_t size) {
  if (size > MAX_SMALL)
    return huge_alloc(size);
  unsigned bin = bin_of(size);
  Page *page = heap.bins[bin];
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
  return alloc_slow(bin);
}

void tz_free(void *p) {
  if (!p)
    return;
  Segment *s = segment_of(p);
  if (s->kind == KIND_HUGE) {
    os_free(s->os_base, s->os_size);
    return;
  }
  unsigned unit = (unsigned)(((uintptr_t)p - (uintptr_t)s) >> UNIT_SHIFT);
  Page *page = &s->pages[s->first_unit[unit]];
  *(void **)p = page->free;
  page->free = p;
  --page->used;
  if (!page->listed) {
    bin_push(page); // it has room again
  } else if (page->used == 0 && (page->next || page->prev)) {
    // Empty, and not the only page of its size: give it back. The last one
    // stays, so that allocating and freeing one object does not churn.
    page_release(page);
  }
}
