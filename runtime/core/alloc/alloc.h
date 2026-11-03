#ifndef TZ_ALLOC_H
#define TZ_ALLOC_H

#include <stddef.h>
#include <stdint.h>

void *tz_alloc(size_t size);
void tz_free(void *p);
void tz_weak_mark(void *p);
void tz_weak_unmark(void *p);
int tz_weak_marked(void *p);

#define TZ_HEAP_BINS 44
#define TZ_HEAP_HUGE_SPARES 4
typedef struct TzHeap {
  void *bins[TZ_HEAP_BINS];
  void *segments;
  void *spare;
  void *huge_spares[TZ_HEAP_HUGE_SPARES];
  void *remote;
} TzHeap;

#endif
