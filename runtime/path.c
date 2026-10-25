// node:path (docs/io.md): the functions of path.posix and path.win32, which
// compute what Node.js does, step by step as its path.js does, on the UTF-16
// code units of the strings. `win32` picks the style; the default style of
// node:path is the system's (CodeGen passes it). fs.c resolves paths with
// these functions too, as Node does on Windows before it calls the system.

#include "tzrt.h"

#include <string.h>

typedef struct {
  const uint16_t *s;
  size_t n;
} Span;

static int is_posix_sep(uint16_t c) { return c == '/'; }
static int is_any_sep(uint16_t c) { return c == '/' || c == '\\'; }
static int is_drive_letter(uint16_t c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }

static Span span_of(const TzBuf *b) { return (Span){b->data, b->length}; }

static Span span_sub(Span s, size_t from, size_t to) {
  if (to > s.n)
    to = s.n;
  if (from > to)
    from = to;
  return (Span){s.s + from, to - from};
}

static uint16_t at(Span s, size_t i) { return i < s.n ? s.s[i] : 0; }

static void put(TzBuf *out, Span s) { tz_buf_units(out, s.s, s.n); }

static int span_equal(Span a, Span b) {
  return a.n == b.n && (a.n == 0 || memcmp(a.s, b.s, a.n * sizeof(uint16_t)) == 0);
}

static ptrdiff_t last_index_of(const TzBuf *b, uint16_t unit) {
  for (size_t i = b->length; i > 0; --i)
    if (b->data[i - 1] == unit)
      return (ptrdiff_t)(i - 1);
  return -1;
}

static int contains(Span s, uint16_t unit) {
  for (size_t i = 0; i < s.n; ++i)
    if (s.s[i] == unit)
      return 1;
  return 0;
}

// The text in lower case, as toLowerCase() makes it.
static void lower(TzBuf *out, Span s) {
  TzStr str = tz_str_from_utf16(s.s, s.n);
  TzStr low;
  tz_str_to_lower(&low, str.obj, str.meta);
  tz_buf_str(out, low);
  tz_str_release_value(low);
  tz_str_release_value(str);
}

static int equal_ignoring_case(Span a, Span b) {
  TzBuf x, y;
  tz_buf_init(&x);
  tz_buf_init(&y);
  lower(&x, a);
  lower(&y, b);
  int equal = span_equal(span_of(&x), span_of(&y));
  tz_buf_free(&x);
  tz_buf_free(&y);
  return equal;
}

// Resolves `.` and `..` in a path that has no root, as normalizeString in
// Node's path.js.
static void normalize_string(TzBuf *res, Span path, int allow_above_root, uint16_t separator,
                             int (*is_separator)(uint16_t)) {
  ptrdiff_t last_segment_length = 0, last_slash = -1;
  int dots = 0;
  uint16_t code = 0;
  for (size_t i = 0; i <= path.n; ++i) {
    if (i < path.n)
      code = path.s[i];
    else if (is_separator(code))
      break;
    else
      code = '/';
    if (is_separator(code)) {
      if (last_slash == (ptrdiff_t)i - 1 || dots == 1) {
        // Nothing: an empty segment or `.`.
      } else if (dots == 2) {
        size_t n = res->length;
        if (n < 2 || last_segment_length != 2 || res->data[n - 1] != '.' ||
            res->data[n - 2] != '.') {
          if (n > 2) {
            ptrdiff_t last = last_index_of(res, separator);
            if (last == -1) {
              res->length = 0;
              last_segment_length = 0;
            } else {
              res->length = (size_t)last;
              last_segment_length = (ptrdiff_t)res->length - 1 - last_index_of(res, separator);
            }
            last_slash = (ptrdiff_t)i;
            dots = 0;
            continue;
          }
          if (n != 0) {
            res->length = 0;
            last_segment_length = 0;
            last_slash = (ptrdiff_t)i;
            dots = 0;
            continue;
          }
        }
        if (allow_above_root) {
          if (res->length > 0)
            tz_buf_push(res, separator);
          tz_buf_ascii(res, "..");
          last_segment_length = 2;
        }
      } else {
        if (res->length > 0)
          tz_buf_push(res, separator);
        put(res, span_sub(path, (size_t)(last_slash + 1), i));
        last_segment_length = (ptrdiff_t)i - last_slash - 1;
      }
      last_slash = (ptrdiff_t)i;
      dots = 0;
    } else if (code == '.' && dots != -1) {
      ++dots;
    } else {
      dots = -1;
    }
  }
}

// process.cwd(), for resolve().
static void cwd(TzBuf *out, int win32) {
  TzStr dir;
  tz_process_cwd(&dir);
  size_t start = out->length;
  tz_buf_str(out, dir);
  tz_str_release_value(dir);
  if (win32)
    return;
#ifdef _WIN32
  // path.posix on Windows: the separators become '/', and the drive goes.
  if (out->length == start)
    return;
  size_t first = out->length - 1; // slice(-1), as Node does without a '/'
  for (size_t i = out->length; i > start; --i) {
    if (out->data[i - 1] == '\\')
      out->data[i - 1] = '/';
    if (out->data[i - 1] == '/')
      first = i - 1;
  }
  memmove(out->data + start, out->data + first, (out->length - first) * sizeof(uint16_t));
  out->length -= first - start;
#else
  (void)start;
#endif
}

//===----------------------------------------------------------------------===//
// Roots of win32 paths
//===----------------------------------------------------------------------===//

// `\\server\share` at the start: where its parts are. 0 if there is none.
typedef struct {
  size_t server, server_end; // the first part
  size_t share, share_end;   // the second
} UncRoot;

// Matches a UNC root after the two separators that start `path`, as Node
// does: one or more non-separators, separators, one or more non-separators.
// Returns 0 without a second part; `*complete` says whether the path ends
// right after the root.
static int match_unc(Span path, UncRoot *root) {
  size_t len = path.n, j = 2, last = j;
  while (j < len && !is_any_sep(path.s[j]))
    ++j;
  if (!(j < len && j != last))
    return 0;
  root->server = last;
  root->server_end = j;
  last = j;
  while (j < len && is_any_sep(path.s[j]))
    ++j;
  if (!(j < len && j != last))
    return 0;
  last = j;
  while (j < len && !is_any_sep(path.s[j]))
    ++j;
  root->share = last;
  root->share_end = j;
  return j == len || j != last;
}

//===----------------------------------------------------------------------===//
// resolve
//===----------------------------------------------------------------------===//

static void resolve_posix(TzBuf *out, const Span *paths, size_t count) {
  TzBuf resolved, piece, normal;
  tz_buf_init(&resolved);
  tz_buf_init(&piece);
  tz_buf_init(&normal);
  int absolute = 0;
  for (ptrdiff_t i = (ptrdiff_t)count - 1; i >= -1 && !absolute; --i) {
    piece.length = 0;
    if (i >= 0)
      put(&piece, paths[i]);
    else
      cwd(&piece, 0);
    if (piece.length == 0)
      continue;
    // `${path}/${resolvedPath}`
    tz_buf_push(&piece, '/');
    put(&piece, span_of(&resolved));
    TzBuf swap = resolved;
    resolved = piece;
    piece = swap;
    absolute = resolved.data[0] == '/';
  }
  normalize_string(&normal, span_of(&resolved), !absolute, '/', is_posix_sep);
  if (absolute) {
    tz_buf_push(out, '/');
    put(out, span_of(&normal));
  } else if (normal.length > 0) {
    put(out, span_of(&normal));
  } else {
    tz_buf_push(out, '.');
  }
  tz_buf_free(&resolved);
  tz_buf_free(&piece);
  tz_buf_free(&normal);
}

// The cwd Windows keeps for a drive, `=C:` in the environment, or the cwd
// of the process, or the root of the drive.
static void drive_cwd(TzBuf *out, Span device) {
  TzBuf name;
  tz_buf_init(&name);
  tz_buf_push(&name, '=');
  put(&name, device);
  TzStr key = tz_buf_take(&name);
  TzStr value;
  int found = tz_process_env(&value, key.obj, key.meta);
  tz_str_release_value(key);
  if (found) {
    tz_buf_str(out, value);
    tz_str_release_value(value);
  }
  if (!found || out->length == 0)
    cwd(out, 1);
  // Not on this drive: its root.
  Span path = span_of(out);
  if (!equal_ignoring_case(span_sub(path, 0, 2), device) && at(path, 2) == '\\') {
    out->length = 0;
    put(out, device);
    tz_buf_push(out, '\\');
  }
}

static void resolve_win32(TzBuf *out, const Span *paths, size_t count) {
  TzBuf device_buf, tail, piece, normal;
  tz_buf_init(&device_buf);
  tz_buf_init(&tail);
  tz_buf_init(&piece);
  tz_buf_init(&normal);
  int resolved_absolute = 0;
  for (ptrdiff_t i = (ptrdiff_t)count - 1; i >= -1; --i) {
    piece.length = 0;
    if (i >= 0) {
      if (paths[i].n == 0)
        continue;
      put(&piece, paths[i]);
    } else if (device_buf.length == 0) {
      cwd(&piece, 1);
    } else {
      drive_cwd(&piece, span_of(&device_buf));
    }
    Span path = span_of(&piece);
    size_t len = path.n, root_end = 0;
    TzBuf device;
    tz_buf_init(&device);
    int is_absolute = 0;
    uint16_t code = at(path, 0);
    if (len == 1) {
      if (is_any_sep(code)) {
        root_end = 1;
        is_absolute = 1;
      }
    } else if (is_any_sep(code)) {
      is_absolute = 1;
      if (is_any_sep(at(path, 1))) {
        UncRoot unc;
        if (match_unc(path, &unc)) {
          Span first = span_sub(path, unc.server, unc.server_end);
          tz_buf_ascii(&device, "\\\\");
          put(&device, first);
          if (first.n == 1 && (first.s[0] == '.' || first.s[0] == '?')) {
            root_end = 4; // a device root, `\\.\PHYSICALDRIVE0`
          } else {
            tz_buf_push(&device, '\\');
            put(&device, span_sub(path, unc.share, unc.share_end));
            root_end = unc.share_end;
          }
        }
      } else {
        root_end = 1;
      }
    } else if (is_drive_letter(code) && at(path, 1) == ':') {
      put(&device, span_sub(path, 0, 2));
      root_end = 2;
      if (len > 2 && is_any_sep(at(path, 2))) {
        is_absolute = 1;
        root_end = 3;
      }
    }

    int skip = 0;
    if (device.length > 0) {
      if (device_buf.length > 0) {
        // Another device: this path does not apply.
        skip = !equal_ignoring_case(span_of(&device), span_of(&device_buf));
      } else {
        put(&device_buf, span_of(&device));
      }
    }
    tz_buf_free(&device);
    if (skip)
      continue;

    if (resolved_absolute) {
      if (device_buf.length > 0)
        break;
    } else {
      // `${path.slice(rootEnd)}\\${resolvedTail}`
      TzBuf joined;
      tz_buf_init(&joined);
      put(&joined, span_sub(path, root_end, len));
      tz_buf_push(&joined, '\\');
      put(&joined, span_of(&tail));
      tz_buf_free(&tail);
      tail = joined;
      resolved_absolute = is_absolute;
      if (is_absolute && device_buf.length > 0)
        break;
    }
  }
  normalize_string(&normal, span_of(&tail), !resolved_absolute, '\\', is_any_sep);
  if (resolved_absolute) {
    put(out, span_of(&device_buf));
    tz_buf_push(out, '\\');
    put(out, span_of(&normal));
  } else if (device_buf.length + normal.length > 0) {
    put(out, span_of(&device_buf));
    put(out, span_of(&normal));
  } else {
    tz_buf_push(out, '.');
  }
  tz_buf_free(&device_buf);
  tz_buf_free(&tail);
  tz_buf_free(&piece);
  tz_buf_free(&normal);
}

static void resolve(TzBuf *out, const Span *paths, size_t count, int win32) {
  if (win32)
    resolve_win32(out, paths, count);
  else
    resolve_posix(out, paths, count);
}

//===----------------------------------------------------------------------===//
// normalize and join
//===----------------------------------------------------------------------===//

static void normalize_posix(TzBuf *out, Span path) {
  if (path.n == 0) {
    tz_buf_push(out, '.');
    return;
  }
  int absolute = path.s[0] == '/';
  int trailing = path.s[path.n - 1] == '/';
  TzBuf normal;
  tz_buf_init(&normal);
  normalize_string(&normal, path, !absolute, '/', is_posix_sep);
  if (normal.length == 0) {
    tz_buf_ascii(out, absolute ? "/" : trailing ? "./" : ".");
  } else {
    if (absolute)
      tz_buf_push(out, '/');
    put(out, span_of(&normal));
    if (trailing)
      tz_buf_push(out, '/');
  }
  tz_buf_free(&normal);
}

// The device names Windows keeps in every directory: `CON:` names the
// console, wherever it is written.
static int is_reserved_name(Span path, ptrdiff_t colon) {
  static const char *const names[] = {
      "CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7",
      "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
  // slice(0, colonIndex) of JavaScript: -1 drops the last unit.
  size_t end = colon >= 0 ? (size_t)colon : path.n > 0 ? path.n - 1 : 0;
  Span device = span_sub(path, 0, end);
  if (device.n < 3 || device.n > 4)
    return 0;
  for (size_t k = 0; k < sizeof names / sizeof *names; ++k) {
    size_t n = strlen(names[k]);
    if (n != device.n)
      continue;
    size_t i = 0;
    for (; i < n; ++i) {
      uint16_t c = device.s[i];
      if (c >= 'a' && c <= 'z')
        c = (uint16_t)(c - 'a' + 'A');
      if (c != (uint8_t)names[k][i])
        break;
    }
    if (i == n)
      return 1;
  }
  // COM¹ and the like, superscript digits that Windows reads as digits too.
  if (device.n == 4) {
    uint16_t d = device.s[3];
    if (d == 0xB9 || d == 0xB2 || d == 0xB3) {
      uint16_t a = device.s[0] & ~0x20, b = device.s[1] & ~0x20, c = device.s[2] & ~0x20;
      return (a == 'C' && b == 'O' && c == 'M') || (a == 'L' && b == 'P' && c == 'T');
    }
  }
  return 0;
}

static ptrdiff_t index_of_from(Span s, uint16_t unit, size_t from) {
  for (size_t i = from; i < s.n; ++i)
    if (s.s[i] == unit)
      return (ptrdiff_t)i;
  return -1;
}

static void normalize_win32(TzBuf *out, Span path) {
  size_t len = path.n;
  if (len == 0) {
    tz_buf_push(out, '.');
    return;
  }
  uint16_t code = path.s[0];
  if (len == 1) {
    // A single '/' becomes '\\'; anything else stays.
    if (is_posix_sep(code))
      tz_buf_push(out, '\\');
    else
      put(out, path);
    return;
  }
  size_t root_end = 0;
  TzBuf device;
  tz_buf_init(&device);
  int has_device = 0, is_absolute = 0;
  if (is_any_sep(code)) {
    is_absolute = 1;
    if (is_any_sep(path.s[1])) {
      UncRoot unc;
      if (match_unc(path, &unc)) {
        Span first = span_sub(path, unc.server, unc.server_end);
        if (first.n == 1 && (first.s[0] == '.' || first.s[0] == '?')) {
          // A device root, `\\.\PHYSICALDRIVE0`; `\\?\COM1:` names a device
          // name.
          tz_buf_ascii(&device, "\\\\");
          put(&device, first);
          root_end = 4;
          ptrdiff_t colon = index_of_from(path, ':', 0);
          Span possible = span_sub(path, 4, colon < 0 ? 0 : (size_t)colon + 1);
          if (possible.n > 0 && is_reserved_name(possible, (ptrdiff_t)possible.n - 1)) {
            device.length = 0;
            tz_buf_ascii(&device, "\\\\?\\");
            put(&device, possible);
            root_end = 4 + possible.n;
          }
        } else if (unc.share_end == len) {
          // Only a UNC root: `\\server\share\`.
          tz_buf_ascii(out, "\\\\");
          put(out, first);
          tz_buf_push(out, '\\');
          put(out, span_sub(path, unc.share, len));
          tz_buf_push(out, '\\');
          tz_buf_free(&device);
          return;
        } else {
          tz_buf_ascii(&device, "\\\\");
          put(&device, first);
          tz_buf_push(&device, '\\');
          put(&device, span_sub(path, unc.share, unc.share_end));
          root_end = unc.share_end;
        }
        has_device = 1;
      }
    } else {
      root_end = 1;
    }
  } else {
    ptrdiff_t colon = index_of_from(path, ':', 0);
    if (colon > 0) {
      if (is_drive_letter(code) && colon == 1) {
        put(&device, span_sub(path, 0, 2));
        has_device = 1;
        root_end = 2;
        if (len > 2 && is_any_sep(path.s[2])) {
          is_absolute = 1;
          root_end = 3;
        }
      } else if (is_reserved_name(path, colon)) {
        // `CON:x`: the device name is its root.
        put(&device, span_sub(path, 0, (size_t)colon + 1));
        has_device = 1;
        root_end = (size_t)colon + 1;
      }
    }
  }

  TzBuf tail;
  tz_buf_init(&tail);
  if (root_end < len)
    normalize_string(&tail, span_sub(path, root_end, len), !is_absolute, '\\', is_any_sep);
  if (tail.length == 0 && !is_absolute)
    tz_buf_push(&tail, '.');
  if (tail.length > 0 && is_any_sep(path.s[len - 1]))
    tz_buf_push(&tail, '\\');

  int dot_prefix = 0;
  if (!is_absolute && !has_device && contains(path, ':')) {
    // The tail must not become something Windows reads as an absolute path
    // (CVE-2024-36139).
    Span t = span_of(&tail);
    if (t.n >= 2 && is_drive_letter(t.s[0]) && t.s[1] == ':') {
      dot_prefix = 1;
    } else {
      for (ptrdiff_t index = index_of_from(path, ':', 0); index != -1 && !dot_prefix;
           index = index_of_from(path, ':', (size_t)index + 1))
        if ((size_t)index == len - 1 || is_any_sep(at(path, (size_t)index + 1)))
          dot_prefix = 1;
    }
    if (dot_prefix) {
      tz_buf_ascii(out, ".\\");
      put(out, span_of(&tail));
      tz_buf_free(&tail);
      tz_buf_free(&device);
      return;
    }
  }
  if (is_reserved_name(path, index_of_from(path, ':', 0))) {
    tz_buf_ascii(out, ".\\");
    put(out, span_of(&device));
    put(out, span_of(&tail));
  } else if (!has_device) {
    if (is_absolute)
      tz_buf_push(out, '\\');
    put(out, span_of(&tail));
  } else {
    put(out, span_of(&device));
    if (is_absolute)
      tz_buf_push(out, '\\');
    put(out, span_of(&tail));
  }
  tz_buf_free(&tail);
  tz_buf_free(&device);
}

static void normalize(TzBuf *out, Span path, int win32) {
  if (win32)
    normalize_win32(out, path);
  else
    normalize_posix(out, path);
}

static void join_posix(TzBuf *out, const Span *paths, size_t count) {
  TzBuf joined;
  tz_buf_init(&joined);
  int any = 0;
  for (size_t i = 0; i < count; ++i) {
    if (paths[i].n == 0)
      continue;
    if (any)
      tz_buf_push(&joined, '/');
    put(&joined, paths[i]);
    any = 1;
  }
  if (!any)
    tz_buf_push(out, '.');
  else
    normalize_posix(out, span_of(&joined));
  tz_buf_free(&joined);
}

static void join_win32(TzBuf *out, const Span *paths, size_t count) {
  TzBuf joined;
  tz_buf_init(&joined);
  Span first = {NULL, 0};
  int any = 0;
  for (size_t i = 0; i < count; ++i) {
    if (paths[i].n == 0)
      continue;
    if (any)
      tz_buf_push(&joined, '\\');
    else
      first = paths[i];
    put(&joined, paths[i]);
    any = 1;
  }
  if (!any) {
    tz_buf_push(out, '.');
    tz_buf_free(&joined);
    return;
  }
  // Two separators at the start would make a UNC path, unless the first part
  // clearly is one: exactly two, then something else.
  int needs_replace = 1;
  size_t slashes = 0;
  if (is_any_sep(first.s[0])) {
    ++slashes;
    if (first.n > 1 && is_any_sep(first.s[1])) {
      ++slashes;
      if (first.n > 2) {
        if (is_any_sep(first.s[2]))
          ++slashes;
        else
          needs_replace = 0;
      }
    }
  }
  if (needs_replace) {
    while (slashes < joined.length && is_any_sep(joined.data[slashes]))
      ++slashes;
    if (slashes >= 2) {
      joined.data[0] = '\\';
      memmove(joined.data + 1, joined.data + slashes, (joined.length - slashes) * sizeof(uint16_t));
      joined.length -= slashes - 1;
    }
  }
  // A part with a device name (`CON:x`) is left as it is, but for the
  // separators: normalizing it could make it another path.
  Span whole = span_of(&joined);
  int reserved = 0;
  for (size_t i = 0; i < whole.n && !reserved;) {
    while (i < whole.n && whole.s[i] == '\\')
      ++i;
    size_t start = i;
    while (i < whole.n && whole.s[i] != '\\')
      ++i;
    Span part = span_sub(whole, start, i);
    ptrdiff_t colon = index_of_from(part, ':', 0);
    if (part.n > 0 && colon != -1 && is_reserved_name(part, colon))
      reserved = 1;
  }
  if (reserved) {
    for (size_t i = 0; i < whole.n; ++i)
      tz_buf_push(out, whole.s[i] == '/' ? '\\' : whole.s[i]);
  } else {
    normalize_win32(out, whole);
  }
  tz_buf_free(&joined);
}

//===----------------------------------------------------------------------===//
// relative
//===----------------------------------------------------------------------===//

static void relative_posix(TzBuf *out, Span from_in, Span to_in) {
  if (span_equal(from_in, to_in))
    return;
  TzBuf fb, tb;
  tz_buf_init(&fb);
  tz_buf_init(&tb);
  resolve_posix(&fb, &from_in, 1);
  resolve_posix(&tb, &to_in, 1);
  Span from = span_of(&fb), to = span_of(&tb);
  if (!span_equal(from, to)) {
    size_t from_start = 1, from_end = from.n, from_len = from_end - from_start;
    size_t to_start = 1, to_len = to.n - to_start;
    size_t length = from_len < to_len ? from_len : to_len;
    ptrdiff_t last_common = -1;
    size_t i = 0;
    int done = 0;
    for (; i < length; ++i) {
      uint16_t c = from.s[from_start + i];
      if (c != to.s[to_start + i])
        break;
      if (c == '/')
        last_common = (ptrdiff_t)i;
    }
    if (i == length) {
      if (to_len > length) {
        if (to.s[to_start + i] == '/') {
          put(out, span_sub(to, to_start + i + 1, to.n));
          done = 1;
        } else if (i == 0) {
          put(out, span_sub(to, to_start + i, to.n));
          done = 1;
        }
      } else if (from_len > length) {
        if (from.s[from_start + i] == '/')
          last_common = (ptrdiff_t)i;
        else if (i == 0)
          last_common = 0;
      }
    }
    if (!done) {
      size_t written = 0;
      for (i = from_start + (size_t)(last_common + 1); i <= from_end; ++i)
        if (i == from_end || from.s[i] == '/') {
          tz_buf_ascii(out, written ? "/.." : "..");
          written = 1;
        }
      put(out, span_sub(to, to_start + (size_t)last_common, to.n));
    }
  }
  tz_buf_free(&fb);
  tz_buf_free(&tb);
}

// Splits at '\\' as split('\\') does, without an empty last part.
static size_t split_backslash(Span s, Span *parts, size_t max) {
  size_t count = 0, start = 0;
  for (size_t i = 0; i <= s.n; ++i)
    if (i == s.n || s.s[i] == '\\') {
      if (count < max)
        parts[count] = span_sub(s, start, i);
      ++count;
      start = i + 1;
    }
  if (count > 0 && count <= max && parts[count - 1].n == 0)
    --count;
  return count;
}

static void relative_win32(TzBuf *out, Span from_in, Span to_in) {
  if (span_equal(from_in, to_in))
    return;
  TzBuf fo, to_b, fl, tl;
  tz_buf_init(&fo);
  tz_buf_init(&to_b);
  tz_buf_init(&fl);
  tz_buf_init(&tl);
  resolve_win32(&fo, &from_in, 1);
  resolve_win32(&to_b, &to_in, 1);
  Span from_orig = span_of(&fo), to_orig = span_of(&to_b);
  if (span_equal(from_orig, to_orig))
    goto done;
  lower(&fl, from_orig);
  lower(&tl, to_orig);
  Span from = span_of(&fl), to = span_of(&tl);
  if (span_equal(from, to))
    goto done;

  if (from_orig.n != from.n || to_orig.n != to.n) {
    // Lower case changed the lengths: compare part by part.
    size_t from_count = split_backslash(from_orig, NULL, 0);
    size_t to_count = split_backslash(to_orig, NULL, 0);
    Span *fp = (Span *)tz_alloc((from_count + 1) * sizeof(Span));
    Span *tp = (Span *)tz_alloc((to_count + 1) * sizeof(Span));
    from_count = split_backslash(from_orig, fp, from_count + 1);
    to_count = split_backslash(to_orig, tp, to_count + 1);
    size_t length = from_count < to_count ? from_count : to_count, i = 0;
    for (; i < length; ++i)
      if (!equal_ignoring_case(fp[i], tp[i]))
        break;
    if (i == 0) {
      put(out, to_orig);
    } else if (i == length) {
      if (to_count > length) {
        for (size_t k = i; k < to_count; ++k) {
          if (k > i)
            tz_buf_push(out, '\\');
          put(out, tp[k]);
        }
      } else if (from_count > length) {
        for (size_t k = 0; k + 1 + i < from_count; ++k)
          tz_buf_ascii(out, "..\\");
        tz_buf_ascii(out, "..");
      }
    } else {
      for (size_t k = i; k < from_count; ++k)
        tz_buf_ascii(out, "..\\");
      for (size_t k = i; k < to_count; ++k) {
        if (k > i)
          tz_buf_push(out, '\\');
        put(out, tp[k]);
      }
    }
    tz_free(fp);
    tz_free(tp);
    goto done;
  }

  {
    size_t from_start = 0;
    while (from_start < from.n && from.s[from_start] == '\\')
      ++from_start;
    size_t from_end = from.n;
    while (from_end - 1 > from_start && from.s[from_end - 1] == '\\')
      --from_end;
    size_t from_len = from_end - from_start;
    size_t to_start = 0;
    while (to_start < to.n && to.s[to_start] == '\\')
      ++to_start;
    size_t to_end = to.n;
    while (to_end - 1 > to_start && to.s[to_end - 1] == '\\')
      --to_end;
    size_t to_len = to_end - to_start;

    size_t length = from_len < to_len ? from_len : to_len, i = 0;
    ptrdiff_t last_common = -1;
    for (; i < length; ++i) {
      uint16_t c = from.s[from_start + i];
      if (c != to.s[to_start + i])
        break;
      if (c == '\\')
        last_common = (ptrdiff_t)i;
    }
    if (i != length) {
      if (last_common == -1) {
        put(out, to_orig);
        goto done;
      }
    } else {
      if (to_len > length) {
        if (to.s[to_start + i] == '\\') {
          put(out, span_sub(to_orig, to_start + i + 1, to_orig.n));
          goto done;
        }
        if (i == 2) {
          put(out, span_sub(to_orig, to_start + i, to_orig.n));
          goto done;
        }
      }
      if (from_len > length) {
        if (from.s[from_start + i] == '\\')
          last_common = (ptrdiff_t)i;
        else if (i == 2)
          last_common = 3;
      }
      if (last_common == -1)
        last_common = 0;
    }
    size_t written = 0;
    for (i = from_start + (size_t)(last_common + 1); i <= from_end; ++i)
      if (i == from_end || from.s[i] == '\\') {
        tz_buf_ascii(out, written ? "\\.." : "..");
        written = 1;
      }
    to_start += (size_t)last_common;
    if (written) {
      put(out, span_sub(to_orig, to_start, to_end));
    } else {
      if (at(to_orig, to_start) == '\\')
        ++to_start;
      put(out, span_sub(to_orig, to_start, to_end));
    }
  }
done:
  tz_buf_free(&fo);
  tz_buf_free(&to_b);
  tz_buf_free(&fl);
  tz_buf_free(&tl);
}

//===----------------------------------------------------------------------===//
// dirname, basename, extname
//===----------------------------------------------------------------------===//

static Span dirname_posix(Span path, const char **literal) {
  *literal = NULL;
  if (path.n == 0) {
    *literal = ".";
    return path;
  }
  int has_root = path.s[0] == '/';
  ptrdiff_t end = -1;
  int matched_slash = 1;
  for (size_t i = path.n - 1; i >= 1; --i) {
    if (path.s[i] == '/') {
      if (!matched_slash) {
        end = (ptrdiff_t)i;
        break;
      }
    } else {
      matched_slash = 0;
    }
  }
  if (end == -1) {
    *literal = has_root ? "/" : ".";
    return path;
  }
  if (has_root && end == 1) {
    *literal = "//";
    return path;
  }
  return span_sub(path, 0, (size_t)end);
}

static Span dirname_win32(Span path, const char **literal) {
  *literal = NULL;
  size_t len = path.n;
  if (len == 0) {
    *literal = ".";
    return path;
  }
  ptrdiff_t root_end = -1;
  size_t offset = 0;
  uint16_t code = path.s[0];
  if (len == 1) {
    if (is_any_sep(code))
      return path;
    *literal = ".";
    return path;
  }
  if (is_any_sep(code)) {
    root_end = 1;
    offset = 1;
    if (is_any_sep(path.s[1])) {
      UncRoot unc;
      if (match_unc(path, &unc)) {
        if (unc.share_end == len)
          return path; // only a UNC root
        root_end = (ptrdiff_t)unc.share_end + 1;
        offset = unc.share_end + 1;
      }
    }
  } else if (is_drive_letter(code) && path.s[1] == ':') {
    root_end = len > 2 && is_any_sep(path.s[2]) ? 3 : 2;
    offset = (size_t)root_end;
  }
  ptrdiff_t end = -1;
  int matched_slash = 1;
  for (ptrdiff_t i = (ptrdiff_t)len - 1; i >= (ptrdiff_t)offset; --i) {
    if (is_any_sep(path.s[i])) {
      if (!matched_slash) {
        end = i;
        break;
      }
    } else {
      matched_slash = 0;
    }
  }
  if (end == -1) {
    if (root_end == -1) {
      *literal = ".";
      return path;
    }
    end = root_end;
  }
  return span_sub(path, 0, (size_t)end);
}

static Span basename_of(Span path, const Span *suffix, int win32) {
  int (*is_sep)(uint16_t) = win32 ? is_any_sep : is_posix_sep;
  size_t start = 0;
  ptrdiff_t end = -1;
  int matched_slash = 1;
  if (win32 && path.n >= 2 && is_drive_letter(path.s[0]) && path.s[1] == ':')
    start = 2;
  if (suffix && suffix->n > 0 && suffix->n <= path.n) {
    if (span_equal(*suffix, path))
      return (Span){path.s, 0};
    ptrdiff_t ext = (ptrdiff_t)suffix->n - 1, first_non_slash_end = -1;
    for (ptrdiff_t i = (ptrdiff_t)path.n - 1; i >= (ptrdiff_t)start; --i) {
      uint16_t code = path.s[i];
      if (is_sep(code)) {
        if (!matched_slash) {
          start = (size_t)i + 1;
          break;
        }
      } else {
        if (first_non_slash_end == -1) {
          matched_slash = 0;
          first_non_slash_end = i + 1;
        }
        if (ext >= 0) {
          if (code == suffix->s[ext]) {
            if (--ext == -1)
              end = i;
          } else {
            ext = -1;
            end = first_non_slash_end;
          }
        }
      }
    }
    if ((ptrdiff_t)start == end)
      end = first_non_slash_end;
    else if (end == -1)
      end = (ptrdiff_t)path.n;
    return span_sub(path, start, (size_t)end);
  }
  for (ptrdiff_t i = (ptrdiff_t)path.n - 1; i >= (ptrdiff_t)start; --i) {
    if (is_sep(path.s[i])) {
      if (!matched_slash) {
        start = (size_t)i + 1;
        break;
      }
    } else if (end == -1) {
      matched_slash = 0;
      end = i + 1;
    }
  }
  if (end == -1)
    return (Span){path.s, 0};
  return span_sub(path, start, (size_t)end);
}

// The last part of a path, looked for down to `from`, split at its last dot
// as extname() and parse() split it: [start_part, start_dot) and
// [start_dot, end), with start_dot -1 for no extension and end -1 for no
// part. start_part is `start_part` if no separator comes before it.
typedef struct {
  ptrdiff_t start_part, start_dot, end;
} LastPart;

static LastPart last_part(Span path, size_t from, size_t start_part, int win32) {
  int (*is_sep)(uint16_t) = win32 ? is_any_sep : is_posix_sep;
  LastPart r = {(ptrdiff_t)start_part, -1, -1};
  int matched_slash = 1, pre_dot_state = 0;
  for (ptrdiff_t i = (ptrdiff_t)path.n - 1; i >= (ptrdiff_t)from; --i) {
    uint16_t code = path.s[i];
    if (is_sep(code)) {
      if (!matched_slash) {
        r.start_part = i + 1;
        break;
      }
      continue;
    }
    if (r.end == -1) {
      matched_slash = 0;
      r.end = i + 1;
    }
    if (code == '.') {
      if (r.start_dot == -1)
        r.start_dot = i;
      else if (pre_dot_state != 1)
        pre_dot_state = 1;
    } else if (r.start_dot != -1) {
      pre_dot_state = -1;
    }
  }
  // No dot, a dot right at the start of the part, or the part `..`.
  if (r.start_dot == -1 || r.end == -1 || pre_dot_state == 0 ||
      (pre_dot_state == 1 && r.start_dot == r.end - 1 && r.start_dot == r.start_part + 1))
    r.start_dot = -1;
  return r;
}

static Span extname_of(Span path, int win32) {
  size_t start = 0;
  if (win32 && path.n >= 2 && path.s[1] == ':' && is_drive_letter(path.s[0]))
    start = 2;
  LastPart part = last_part(path, start, start, win32);
  if (part.start_dot == -1)
    return (Span){path.s, 0};
  return span_sub(path, (size_t)part.start_dot, (size_t)part.end);
}

//===----------------------------------------------------------------------===//
// The functions of node:path
//===----------------------------------------------------------------------===//

// The elements of an array of strings, flat, as spans; `bufs` holds them.
static Span *spans_of(TzArray *parts, TzBuf **bufs) {
  size_t n = parts->length;
  Span *spans = (Span *)tz_alloc((n + 1) * sizeof(Span));
  *bufs = (TzBuf *)tz_alloc((n + 1) * sizeof(TzBuf));
  for (size_t i = 0; i < n; ++i) {
    tz_buf_init(&(*bufs)[i]);
    tz_buf_str(&(*bufs)[i], ((TzStr *)parts->data)[i]);
    spans[i] = span_of(&(*bufs)[i]);
  }
  return spans;
}

static void free_spans(Span *spans, TzBuf *bufs, size_t n) {
  for (size_t i = 0; i < n; ++i)
    tz_buf_free(&bufs[i]);
  tz_free(bufs);
  tz_free(spans);
}

typedef struct {
  TzBuf buf;
  Span span;
} Text;

static void text_init(Text *t, void *obj, uint64_t meta) {
  tz_buf_init(&t->buf);
  tz_buf_str(&t->buf, (TzStr){obj, meta});
  t->span = span_of(&t->buf);
}

static void finish(TzStr *out, TzBuf *b) { *out = tz_buf_take(b); }

void tz_path_join(TzStr *out, TzArray *parts, int32_t win32) {
  TzBuf *bufs;
  Span *spans = spans_of(parts, &bufs);
  TzBuf b;
  tz_buf_init(&b);
  if (win32)
    join_win32(&b, spans, parts->length);
  else
    join_posix(&b, spans, parts->length);
  free_spans(spans, bufs, parts->length);
  finish(out, &b);
}

void tz_path_resolve(TzStr *out, TzArray *parts, int32_t win32) {
  TzBuf *bufs;
  Span *spans = spans_of(parts, &bufs);
  TzBuf b;
  tz_buf_init(&b);
  resolve(&b, spans, parts->length, win32);
  free_spans(spans, bufs, parts->length);
  finish(out, &b);
}

void tz_path_normalize(TzStr *out, void *obj, uint64_t meta, int32_t win32) {
  Text t;
  text_init(&t, obj, meta);
  TzBuf b;
  tz_buf_init(&b);
  normalize(&b, t.span, win32);
  tz_buf_free(&t.buf);
  finish(out, &b);
}

int32_t tz_path_is_absolute(void *obj, uint64_t meta, int32_t win32) {
  Text t;
  text_init(&t, obj, meta);
  Span p = t.span;
  int absolute;
  if (!win32)
    absolute = p.n > 0 && p.s[0] == '/';
  else
    absolute = p.n > 0 && (is_any_sep(p.s[0]) || (p.n > 2 && is_drive_letter(p.s[0]) &&
                                                  p.s[1] == ':' && is_any_sep(p.s[2])));
  tz_buf_free(&t.buf);
  return absolute;
}

void tz_path_dirname(TzStr *out, void *obj, uint64_t meta, int32_t win32) {
  Text t;
  text_init(&t, obj, meta);
  const char *literal;
  Span dir = win32 ? dirname_win32(t.span, &literal) : dirname_posix(t.span, &literal);
  TzBuf b;
  tz_buf_init(&b);
  if (literal)
    tz_buf_ascii(&b, literal);
  else
    put(&b, dir);
  tz_buf_free(&t.buf);
  finish(out, &b);
}

void tz_path_basename(TzStr *out, void *obj, uint64_t meta, void *suffix_obj, uint64_t suffix_meta,
                      int32_t has_suffix, int32_t win32) {
  Text t, suffix;
  text_init(&t, obj, meta);
  text_init(&suffix, has_suffix ? suffix_obj : NULL, has_suffix ? suffix_meta : 0);
  TzBuf b;
  tz_buf_init(&b);
  put(&b, basename_of(t.span, has_suffix ? &suffix.span : NULL, win32));
  tz_buf_free(&t.buf);
  tz_buf_free(&suffix.buf);
  finish(out, &b);
}

void tz_path_extname(TzStr *out, void *obj, uint64_t meta, int32_t win32) {
  Text t;
  text_init(&t, obj, meta);
  TzBuf b;
  tz_buf_init(&b);
  put(&b, extname_of(t.span, win32));
  tz_buf_free(&t.buf);
  finish(out, &b);
}

void tz_path_relative(TzStr *out, void *from_obj, uint64_t from_meta, void *to_obj,
                      uint64_t to_meta, int32_t win32) {
  Text from, to;
  text_init(&from, from_obj, from_meta);
  text_init(&to, to_obj, to_meta);
  TzBuf b;
  tz_buf_init(&b);
  if (win32)
    relative_win32(&b, from.span, to.span);
  else
    relative_posix(&b, from.span, to.span);
  tz_buf_free(&from.buf);
  tz_buf_free(&to.buf);
  finish(out, &b);
}

// path.parse(): { root, dir, base, ext, name }, which `make` builds from a
// TzParsedPath.
void *tz_path_parse(void *obj, uint64_t meta, int32_t win32, TzMaker make) {
  Text t;
  text_init(&t, obj, meta);
  Span path = t.span;
  size_t len = path.n;
  Span root = {path.s, 0}, dir = root, base = root, ext = root, name = root;
  int whole = 0; // the path is all root, or all base
  size_t root_end = 0;
  if (len > 0 && win32) {
    uint16_t code = path.s[0];
    if (len == 1) {
      if (is_any_sep(code))
        root = dir = path;
      else
        base = name = path;
      whole = 1;
    } else if (is_any_sep(code)) {
      root_end = 1;
      if (is_any_sep(path.s[1])) {
        UncRoot unc;
        if (match_unc(path, &unc))
          root_end = unc.share_end == len ? len : unc.share_end + 1;
      }
    } else if (is_drive_letter(code) && path.s[1] == ':') {
      if (len <= 2 || (is_any_sep(path.s[2]) && len == 3)) {
        root = dir = path;
        whole = 1;
      } else {
        root_end = is_any_sep(path.s[2]) ? 3 : 2;
      }
    }
    if (!whole) {
      root = span_sub(path, 0, root_end);
      LastPart part = last_part(path, root_end, root_end, 1);
      if (part.end != -1) {
        if (part.start_dot == -1) {
          base = name = span_sub(path, (size_t)part.start_part, (size_t)part.end);
        } else {
          name = span_sub(path, (size_t)part.start_part, (size_t)part.start_dot);
          base = span_sub(path, (size_t)part.start_part, (size_t)part.end);
          ext = span_sub(path, (size_t)part.start_dot, (size_t)part.end);
        }
      }
      // A directory that is the root keeps its separator: `C:\abc` is in
      // `C:\`.
      if (part.start_part > 0 && (size_t)part.start_part != root_end)
        dir = span_sub(path, 0, (size_t)part.start_part - 1);
      else
        dir = root;
    }
  } else if (len > 0) {
    int absolute = path.s[0] == '/';
    if (absolute)
      root = span_sub(path, 0, 1);
    LastPart part = last_part(path, absolute ? 1 : 0, 0, 0);
    if (part.end != -1) {
      size_t from = part.start_part == 0 && absolute ? 1 : (size_t)part.start_part;
      if (part.start_dot == -1) {
        base = name = span_sub(path, from, (size_t)part.end);
      } else {
        name = span_sub(path, from, (size_t)part.start_dot);
        base = span_sub(path, from, (size_t)part.end);
        ext = span_sub(path, (size_t)part.start_dot, (size_t)part.end);
      }
    }
    if (part.start_part > 0)
      dir = span_sub(path, 0, (size_t)part.start_part - 1);
    else if (absolute)
      dir = root;
  }
  TzParsedPath parts;
  parts.root = tz_str_from_utf16(root.s, root.n);
  parts.dir = tz_str_from_utf16(dir.s, dir.n);
  parts.base = tz_str_from_utf16(base.s, base.n);
  parts.ext = tz_str_from_utf16(ext.s, ext.n);
  parts.name = tz_str_from_utf16(name.s, name.n);
  tz_buf_free(&t.buf);
  return make(&parts);
}

// path.format(): `dir` (or `root`), then `base` (or `name` and `ext`).
void tz_path_format(TzStr *out, const TzOptStr *parts, int32_t win32) {
  // The fields in the order of TzParsedPath: root, dir, base, ext, name.
  TzBuf f[5];
  for (int i = 0; i < 5; ++i) {
    tz_buf_init(&f[i]);
    if (parts[i].has)
      tz_buf_str(&f[i], parts[i].value);
  }
  Span root = span_of(&f[0]), dir = span_of(&f[1]), base = span_of(&f[2]), ext = span_of(&f[3]),
       name = span_of(&f[4]);
  TzBuf b;
  tz_buf_init(&b);
  Span d = dir.n > 0 ? dir : root;
  if (d.n > 0) {
    put(&b, d);
    // A root ends with its separator.
    if (!span_equal(d, root))
      tz_buf_push(&b, win32 ? '\\' : '/');
  }
  if (base.n > 0) {
    put(&b, base);
  } else {
    put(&b, name);
    if (ext.n > 0) {
      if (ext.s[0] != '.')
        tz_buf_push(&b, '.');
      put(&b, ext);
    }
  }
  for (int i = 0; i < 5; ++i)
    tz_buf_free(&f[i]);
  finish(out, &b);
}

// path.toNamespacedPath(): on win32, the `\\?\` form of an absolute path,
// which takes long paths; posix paths stay as they are.
static void namespaced(TzBuf *out, Span path) {
  if (path.n == 0)
    return;
  TzBuf resolved;
  tz_buf_init(&resolved);
  resolve_win32(&resolved, &path, 1);
  Span r = span_of(&resolved);
  if (r.n <= 2) {
    put(out, path);
  } else if (r.s[0] == '\\') {
    if (r.s[1] == '\\' && r.s[2] != '?' && r.s[2] != '.') {
      tz_buf_ascii(out, "\\\\?\\UNC\\");
      put(out, span_sub(r, 2, r.n));
    } else {
      put(out, r);
    }
  } else if (is_drive_letter(r.s[0]) && r.s[1] == ':' && r.s[2] == '\\') {
    tz_buf_ascii(out, "\\\\?\\");
    put(out, r);
  } else {
    put(out, r);
  }
  tz_buf_free(&resolved);
}

void tz_path_to_namespaced(TzStr *out, void *obj, uint64_t meta, int32_t win32) {
  Text t;
  text_init(&t, obj, meta);
  TzBuf b;
  tz_buf_init(&b);
  if (win32)
    namespaced(&b, t.span);
  else
    put(&b, t.span);
  tz_buf_free(&t.buf);
  finish(out, &b);
}

void tz_path_resolve_units(TzBuf *out, const uint16_t *units, size_t length) {
  Span path = {units, length};
#ifdef _WIN32
  resolve_win32(out, &path, 1);
#else
  resolve_posix(out, &path, 1);
#endif
}

void tz_path_namespaced_units(TzBuf *out, const uint16_t *units, size_t length) {
  namespaced(out, (Span){units, length});
}

void tz_path_join_units(TzBuf *out, const uint16_t *a, size_t a_length, const uint16_t *b,
                        size_t b_length) {
  Span parts[2] = {{a, a_length}, {b, b_length}};
#ifdef _WIN32
  join_win32(out, parts, 2);
#else
  join_posix(out, parts, 2);
#endif
}
