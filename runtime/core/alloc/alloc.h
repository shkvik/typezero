// The allocator.

#ifndef TZ_ALLOC_H
#define TZ_ALLOC_H

#include <stddef.h>
#include <stdint.h>

// The allocator (alloc.c). tz_alloc panics when memory runs out; tz_free
// takes any block from tz_alloc, or NULL.
void *tz_alloc(size_t size);
void tz_free(void *p);

// The allocator's own memory of a thread (alloc.c): its pages with room for
// each size class, its segments, spares; and the blocks other threads freed
// that it has not taken back yet, linked by their first word.
#define TZ_HEAP_BINS 44
#define TZ_HEAP_HUGE_SPARES 4
typedef struct TzHeap {
  void *bins[TZ_HEAP_BINS];
  void *segments;
  void *spare;
  void *huge_spares[TZ_HEAP_HUGE_SPARES];
  void *remote; // through atomic operations
} TzHeap;

#endif
