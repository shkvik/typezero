// node:path.

#ifndef TZ_PATH_H
#define TZ_PATH_H

#include "runtime/core/string/string.h"
#include <stddef.h>
#include <stdint.h>

// path.parse(): ParsedPath.
typedef struct {
  TzStr root, dir, base, ext, name;
} TzParsedPath;

// For fs.c: path.resolve() and path.toNamespacedPath() of one path, and
// path.join() of two, in the system's style.
void tz_path_resolve_units(TzBuf *out, const uint16_t *units, size_t length);
void tz_path_namespaced_units(TzBuf *out, const uint16_t *units, size_t length);
void tz_path_join_units(TzBuf *out, const uint16_t *a, size_t a_length, const uint16_t *b,
                        size_t b_length);

#endif
