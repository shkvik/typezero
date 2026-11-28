#include "runtime/library/path/path/path.h"
#include "runtime/core/alloc/alloc.h"
#include "runtime/core/array/array.h"
#include "runtime/core/string/string.h"
#include "runtime/core/tzrt/tzrt.h"
#include "runtime/library/process/process/process.h"
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
  if (to > s.n) {
    to = s.n;
  }
  if (from > to) {
    from = to;
  }

  return (Span){s.s + from, to - from};
}

static uint16_t at(Span s, size_t i) { return i < s.n ? s.s[i] : 0; }

static void put(TzBuf *out, Span s) { tz_buf_units(out, s.s, s.n); }

static int span_equal(Span a, Span b) {
  return a.n == b.n && (a.n == 0 || memcmp(a.s, b.s, a.n * sizeof(uint16_t)) == 0);
}

static ptrdiff_t last_index_of(const TzBuf *b, uint16_t unit) {
  for (size_t i = b->length; i > 0; --i) {
    if (b->data[i - 1] == unit) {
      return (ptrdiff_t)(i - 1);
    }
  }

  return -1;
}

static int contains(Span s, uint16_t unit) {
  for (size_t i = 0; i < s.n; ++i) {
    if (s.s[i] == unit) {
      return 1;
    }
  }

  return 0;
}

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

static void normalize_string(TzBuf *res, Span path, int allow_above_root, uint16_t separator,
                             int (*is_separator)(uint16_t)) {
  ptrdiff_t last_segment_length = 0, last_slash = -1;
  int dots = 0;
  uint16_t code = 0;
  for (size_t i = 0; i <= path.n; ++i) {
    if (i < path.n) {
      code = path.s[i];
    } else if (is_separator(code)) {
      break;
    } else {
      code = '/';
    }
    if (is_separator(code)) {
      if (last_slash == (ptrdiff_t)i - 1 || dots == 1) {
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
          if (res->length > 0) {
            tz_buf_push(res, separator);
          }
          tz_buf_ascii(res, "..");
          last_segment_length = 2;
        }
      } else {
        if (res->length > 0) {
          tz_buf_push(res, separator);
        }
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

static void cwd(TzBuf *out, int win32) {
  TzStr dir;
  tz_process_cwd(&dir);
  size_t start = out->length;
  tz_buf_str(out, dir);
  tz_str_release_value(dir);
  if (win32) {
    return;
  }
#ifdef _WIN32
  if (out->length == start) {
    return;
  }
  size_t first = out->length - 1;
  for (size_t i = out->length; i > start; --i) {
    if (out->data[i - 1] == '\\') {
      out->data[i - 1] = '/';
    }
    if (out->data[i - 1] == '/') {
      first = i - 1;
    }
  }
  memmove(out->data + start, out->data + first, (out->length - first) * sizeof(uint16_t));
  out->length -= first - start;
#else
  (void)start;
#endif
}

typedef struct {
  size_t server, server_end;
  size_t share, share_end;
} UncRoot;

static int match_unc(Span path, UncRoot *root) {
  size_t len = path.n, j = 2, last = j;
  while (j < len && !is_any_sep(path.s[j])) {
    ++j;
  }
  if (!(j < len && j != last)) {
    return 0;
  }
  root->server = last;
  root->server_end = j;
  last = j;
  while (j < len && is_any_sep(path.s[j])) {
    ++j;
  }
  if (!(j < len && j != last)) {
    return 0;
  }
  last = j;
  while (j < len && !is_any_sep(path.s[j])) {
    ++j;
  }
  root->share = last;
  root->share_end = j;

  return j == len || j != last;
}

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
  if (!found || out->length == 0) {
    cwd(out, 1);
  }
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
      if (paths[i].n == 0) {
        continue;
      }
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
            root_end = 4;
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
        skip = !equal_ignoring_case(span_of(&device), span_of(&device_buf));
      } else {
        put(&device_buf, span_of(&device));
      }
    }
    tz_buf_free(&device);
    if (skip) {
      continue;
    }

    if (resolved_absolute) {
      if (device_buf.length > 0) {
        break;
      }
    } else {
      TzBuf joined;
      tz_buf_init(&joined);
      put(&joined, span_sub(path, root_end, len));
      tz_buf_push(&joined, '\\');
      put(&joined, span_of(&tail));
      tz_buf_free(&tail);
      tail = joined;
      resolved_absolute = is_absolute;
      if (is_absolute && device_buf.length > 0) {
        break;
      }
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

static int is_reserved_name(Span path, ptrdiff_t colon) {
  static const char *const names[] = {
      "CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7",
      "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
  size_t end = colon >= 0 ? (size_t)colon : path.n > 0 ? path.n - 1 : 0;
  Span device = span_sub(path, 0, end);
  if (device.n < 3 || device.n > 4) {
    return 0;
  }
  for (size_t k = 0; k < sizeof names / sizeof *names; ++k) {
    size_t n = strlen(names[k]);
    if (n != device.n) {
      continue;
    }
    size_t i = 0;
    for (; i < n; ++i) {
      uint16_t c = device.s[i];
      if (c >= 'a' && c <= 'z') {
        c = (uint16_t)(c - 'a' + 'A');
      }
      if (c != (uint8_t)names[k][i]) {
        break;
      }
    }
    if (i == n) {
      return 1;
    }
  }
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
  for (size_t i = from; i < s.n; ++i) {
    if (s.s[i] == unit) {
      return (ptrdiff_t)i;
    }
  }

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
    if (is_posix_sep(code)) {
      tz_buf_push(out, '\\');
    } else {
      put(out, path);
    }
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
        put(&device, span_sub(path, 0, (size_t)colon + 1));
        has_device = 1;
        root_end = (size_t)colon + 1;
      }
    }
  }

  TzBuf tail;
  tz_buf_init(&tail);
  if (root_end < len) {
    normalize_string(&tail, span_sub(path, root_end, len), !is_absolute, '\\', is_any_sep);
  }
  if (tail.length == 0 && !is_absolute) {
    tz_buf_push(&tail, '.');
  }
  if (tail.length > 0 && is_any_sep(path.s[len - 1])) {
    tz_buf_push(&tail, '\\');
  }

  int dot_prefix = 0;
  if (!is_absolute && !has_device && contains(path, ':')) {
    Span t = span_of(&tail);
    if (t.n >= 2 && is_drive_letter(t.s[0]) && t.s[1] == ':') {
      dot_prefix = 1;
    } else {
      for (ptrdiff_t index = index_of_from(path, ':', 0); index != -1 && !dot_prefix;
           index = index_of_from(path, ':', (size_t)index + 1)) {
        if ((size_t)index == len - 1 || is_any_sep(at(path, (size_t)index + 1))) {
          dot_prefix = 1;
        }
      }
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
    if (is_absolute) {
      tz_buf_push(out, '\\');
    }
    put(out, span_of(&tail));
  } else {
    put(out, span_of(&device));
    if (is_absolute) {
      tz_buf_push(out, '\\');
    }
    put(out, span_of(&tail));
  }
  tz_buf_free(&tail);
  tz_buf_free(&device);
}

static void join_win32(TzBuf *out, const Span *paths, size_t count) {
  TzBuf joined;
  tz_buf_init(&joined);
  Span first = {NULL, 0};
  int any = 0;
  for (size_t i = 0; i < count; ++i) {
    if (paths[i].n == 0) {
      continue;
    }
    if (any) {
      tz_buf_push(&joined, '\\');
    } else {
      first = paths[i];
    }
    put(&joined, paths[i]);
    any = 1;
  }
  if (!any) {
    tz_buf_push(out, '.');
    tz_buf_free(&joined);
    return;
  }
  int needs_replace = 1;
  size_t slashes = 0;
  if (is_any_sep(first.s[0])) {
    ++slashes;
    if (first.n > 1 && is_any_sep(first.s[1])) {
      ++slashes;
      if (first.n > 2) {
        if (is_any_sep(first.s[2])) {
          ++slashes;
        } else {
          needs_replace = 0;
        }
      }
    }
  }
  if (needs_replace) {
    while (slashes < joined.length && is_any_sep(joined.data[slashes])) {
      ++slashes;
    }
    if (slashes >= 2) {
      joined.data[0] = '\\';
      memmove(joined.data + 1, joined.data + slashes, (joined.length - slashes) * sizeof(uint16_t));
      joined.length -= slashes - 1;
    }
  }
  Span whole = span_of(&joined);
  int reserved = 0;
  for (size_t i = 0; i < whole.n && !reserved;) {
    while (i < whole.n && whole.s[i] == '\\') {
      ++i;
    }
    size_t start = i;
    while (i < whole.n && whole.s[i] != '\\') {
      ++i;
    }
    Span part = span_sub(whole, start, i);
    ptrdiff_t colon = index_of_from(part, ':', 0);
    if (part.n > 0 && colon != -1 && is_reserved_name(part, colon)) {
      reserved = 1;
    }
  }
  if (reserved) {
    for (size_t i = 0; i < whole.n; ++i) {
      tz_buf_push(out, whole.s[i] == '/' ? '\\' : whole.s[i]);
    }
  } else {
    normalize_win32(out, whole);
  }
  tz_buf_free(&joined);
}

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
      if (r.start_dot == -1) {
        r.start_dot = i;
      } else if (pre_dot_state != 1) {
        pre_dot_state = 1;
      }
    } else if (r.start_dot != -1) {
      pre_dot_state = -1;
    }
  }
  if (r.start_dot == -1 || r.end == -1 || pre_dot_state == 0 ||
      (pre_dot_state == 1 && r.start_dot == r.end - 1 && r.start_dot == r.start_part + 1)) {
    r.start_dot = -1;
  }

  return r;
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

void *tz_path_parse(void *obj, uint64_t meta, int64_t win32, TzMaker make) {
  Text t;
  text_init(&t, obj, meta);
  Span path = t.span;
  size_t len = path.n;
  Span root = {path.s, 0}, dir = root, base = root, ext = root, name = root;
  int whole = 0;
  size_t root_end = 0;
  if (len > 0 && win32) {
    uint16_t code = path.s[0];
    if (len == 1) {
      if (is_any_sep(code)) {
        root = dir = path;
      } else {
        base = name = path;
      }
      whole = 1;
    } else if (is_any_sep(code)) {
      root_end = 1;
      if (is_any_sep(path.s[1])) {
        UncRoot unc;
        if (match_unc(path, &unc)) {
          root_end = unc.share_end == len ? len : unc.share_end + 1;
        }
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
      if (part.start_part > 0 && (size_t)part.start_part != root_end) {
        dir = span_sub(path, 0, (size_t)part.start_part - 1);
      } else {
        dir = root;
      }
    }
  } else if (len > 0) {
    int absolute = path.s[0] == '/';
    if (absolute) {
      root = span_sub(path, 0, 1);
    }
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
    if (part.start_part > 0) {
      dir = span_sub(path, 0, (size_t)part.start_part - 1);
    } else if (absolute) {
      dir = root;
    }
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

void tz_path_format(TzStr *out, const TzOptStr *parts, int64_t win32) {
  TzBuf f[5];
  for (int i = 0; i < 5; ++i) {
    tz_buf_init(&f[i]);
    if (parts[i].has) {
      tz_buf_str(&f[i], parts[i].value);
    }
  }
  Span root = span_of(&f[0]), dir = span_of(&f[1]), base = span_of(&f[2]), ext = span_of(&f[3]),
       name = span_of(&f[4]);
  TzBuf b;
  tz_buf_init(&b);
  Span d = dir.n > 0 ? dir : root;
  if (d.n > 0) {
    put(&b, d);
    if (!span_equal(d, root)) {
      tz_buf_push(&b, win32 ? '\\' : '/');
    }
  }
  if (base.n > 0) {
    put(&b, base);
  } else {
    put(&b, name);
    if (ext.n > 0) {
      if (ext.s[0] != '.') {
        tz_buf_push(&b, '.');
      }
      put(&b, ext);
    }
  }
  for (int i = 0; i < 5; ++i) {
    tz_buf_free(&f[i]);
  }
  finish(out, &b);
}

static void namespaced(TzBuf *out, Span path) {
  if (path.n == 0) {
    return;
  }
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

void tz_path_join_units(TzBuf *out, const uint16_t *a, size_t a_length, const uint16_t *b,
                        size_t b_length) {
  Span parts[2] = {{a, a_length}, {b, b_length}};
#ifdef _WIN32
  join_win32(out, parts, 2);
#else
  join_posix(out, parts, 2);
#endif
}
