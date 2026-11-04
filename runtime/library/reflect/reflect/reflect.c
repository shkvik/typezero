#include "runtime/library/reflect/reflect/reflect.h"
#include "runtime/core/alloc/alloc.h"
#include "runtime/core/array/array.h"
#include "runtime/core/closure/closure.h"
#include "runtime/core/error/error.h"
#include "runtime/core/group/group.h"
#include "runtime/core/map/map.h"
#include "runtime/core/output/output.h"
#include "runtime/core/string/string.h"
#include "runtime/core/tzrt/tzrt.h"
#include "runtime/core/unknown/unknown.h"
#include "runtime/library/console/inspect/inspect.h"
#include "runtime/library/json/json/json.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

int32_t tz_json_kind(const TzJson *j);

typedef struct {
  TzClass *const *classes;
  uint32_t class_count;
  const TzInvokerEntry *invokers;
  uint32_t invoker_count;
} TzReflectStore;

TzReflectStore tz_reflect_store;

void tz_reflect_init(TzClass *const *classes, uint32_t count, const TzInvokerEntry *invokers,
                     uint32_t invoker_count) {
  tz_reflect_store.classes = classes;
  tz_reflect_store.class_count = count;
  tz_reflect_store.invokers = invokers;
  tz_reflect_store.invoker_count = invoker_count;
}

static const TzBox *as_box(const void *u) {
  const TzInstance *o = (const TzInstance *)u;

  return o && o->layout == &tz_box_layout ? (const TzBox *)u : NULL;
}

static int same_name(const char *name, uint32_t length, TzStr key) {
  uint32_t units;
  int two;
  const void *data = tz_str_units(&key, &units, &two);
  uint32_t k = 0;
  for (uint32_t i = 0; i < length;) {
    uint8_t c = (uint8_t)name[i];
    uint32_t cp = c, n = 1;
    if (c >= 0xF0) {
      cp = c & 0x07, n = 4;
    } else if (c >= 0xE0) {
      cp = c & 0x0F, n = 3;
    } else if (c >= 0xC0) {
      cp = c & 0x1F, n = 2;
    }
    for (uint32_t j = 1; j < n && i + j < length; ++j) {
      cp = (cp << 6) | ((uint8_t)name[i + j] & 0x3F);
    }
    i += n;
    uint16_t want[2];
    uint32_t count = 1;
    if (cp >= 0x10000) {
      want[0] = (uint16_t)(0xD800 + ((cp - 0x10000) >> 10));
      want[1] = (uint16_t)(0xDC00 + ((cp - 0x10000) & 0x3FF));
      count = 2;
    } else {
      want[0] = (uint16_t)cp;
    }
    for (uint32_t j = 0; j < count; ++j, ++k) {
      if (k >= units) {
        return 0;
      }
      uint16_t unit = two ? ((const uint16_t *)data)[k] : ((const uint8_t *)data)[k];
      if (unit != want[j]) {
        return 0;
      }
    }
  }

  return k == units;
}

static void describe(const void *u, char *out, size_t size) {
  int32_t kind = tz_unknown_kind(u);
  const TzBox *box = as_box(u);
  if (kind == TZ_KIND_NUMBER) {
    char number[32];
    size_t n = tz_format_f64(tz_unknown_number(u), number);
    snprintf(out, size, "%.*s", (int)n, number);
    return;
  }
  if (kind == TZ_KIND_BOOLEAN) {
    snprintf(out, size, "%s", tz_unknown_bool(u) ? "true" : "false");
    return;
  }
  if (u && !box) {
    const TzLayout *layout = ((const TzInstance *)u)->layout;
    if (layout->flags & TZ_LAYOUT_CLASS) {
      snprintf(out, size, "the class %.*s", (int)layout->name_length, layout->name);
    } else if (layout->name && layout->name_length) {
      snprintf(out, size, "an object of class %.*s", (int)layout->name_length, layout->name);
    } else {
      snprintf(out, size, "an object");
    }
    return;
  }
  if (box && box->type->kind == TZ_T_ARRAY) {
    snprintf(out, size, "an array");
    return;
  }
  static const char *const names[] = {"undefined", "a number",  "a string",
                                      "a boolean", "an object", "a function"};
  snprintf(out, size, "%s", kind >= 0 && kind <= 5 ? names[kind] : "a value");
}

TZ_THREAD_LOCAL TzMaker tz_reflect_maker;

void tz_reflect_errors(TzMaker make) { tz_reflect_maker = make; }

typedef struct {
  const char *code;
  TzStr message;
} FailureParts;

void tz_reflect_fail_str(const char *code, uint32_t code_length, void *message_obj,
                         uint64_t message_meta) {
  if (!tz_reflect_maker) {
    TzBuf units;
    tz_buf_init(&units);
    tz_buf_str(&units, (TzStr){message_obj, message_meta});
    char text[512];
    size_t n = 0;
    for (size_t i = 0; i < units.length && n + 1 < sizeof text; ++i) {
      text[n++] = units.data[i] < 0x80 ? (char)units.data[i] : '?';
    }
    text[n] = 0;
    tz_buf_free(&units);
    tz_panicf("TypeError: %s", text);
  }
  char text[32];
  snprintf(text, sizeof text, "%.*s", (int)code_length, code);
  FailureParts parts;
  parts.code = text;
  parts.message = (TzStr){message_obj, message_meta};
  tz_str_retain_value(parts.message);
  tz_set_failed(tz_reflect_maker(&parts));
}

TZ_COLD static void reflect_fail(const char *code, const char *format, ...) {
  char text[512];
  va_list args;
  va_start(args, format);
  int length = vsnprintf(text, sizeof text, format, args);
  va_end(args);
  if (length < 0) {
    length = 0;
  }
  if ((size_t)length >= sizeof text) {
    length = (int)sizeof text - 1;
  }
  if (!tz_reflect_maker) {
    tz_panicf("TypeError: %s", text);
  }
  TzBuf buf;
  tz_buf_init(&buf);
  tz_buf_utf8(&buf, text, (size_t)length);
  FailureParts parts;
  parts.code = code;
  parts.message = tz_buf_take(&buf);
  tz_set_failed(tz_reflect_maker(&parts));
}

void tz_reflect_mismatch(const char *what, uint32_t what_length, int32_t index,
                         const char *expected, uint32_t expected_length, const void *got) {
  char given[160];
  describe(got, given, sizeof given);
  reflect_fail("ERR_INVALID_ARGUMENT", "%.*s: argument %d: expected %.*s, got %s", (int)what_length,
               what, (int)index + 1, (int)expected_length, expected, given);
}

void tz_reflect_arity(const char *what, uint32_t what_length, int32_t min, int32_t max,
                      uint32_t count) {
  char takes[64];
  if (max < 0) {
    snprintf(takes, sizeof takes, "%d or more arguments", (int)min);
  } else if (min == max) {
    snprintf(takes, sizeof takes, "%d argument%s", (int)min, min == 1 ? "" : "s");
  } else {
    snprintf(takes, sizeof takes, "%d to %d arguments", (int)min, (int)max);
  }
  reflect_fail("ERR_INVALID_ARITY", "%.*s: expected %s, got %u", (int)what_length, what, takes,
               count);
}

void tz_class_name(TzStr *out, const TzClass *cls) {
  const TzLayout *layout = cls->layout;
  TzBuf buf;
  tz_buf_init(&buf);
  const char *name = layout->name;
  for (uint32_t i = 0; i < layout->name_length;) {
    uint8_t c = (uint8_t)name[i];
    uint32_t cp = c, n = 1;
    if (c >= 0xF0) {
      cp = c & 0x07, n = 4;
    } else if (c >= 0xE0) {
      cp = c & 0x0F, n = 3;
    } else if (c >= 0xC0) {
      cp = c & 0x1F, n = 2;
    }
    for (uint32_t j = 1; j < n && i + j < layout->name_length; ++j) {
      cp = (cp << 6) | ((uint8_t)name[i + j] & 0x3F);
    }
    i += n;
    if (cp >= 0x10000) {
      tz_buf_push(&buf, (uint16_t)(0xD800 + ((cp - 0x10000) >> 10)));
      tz_buf_push(&buf, (uint16_t)(0xDC00 + ((cp - 0x10000) & 0x3FF)));
    } else {
      tz_buf_push(&buf, (uint16_t)cp);
    }
  }
  *out = tz_buf_take(&buf);
}

typedef struct {
  const char *code;
  struct {
    TzStr message, class_name, field, expected, got, value;
  } details;
} FillTypeParts;

typedef struct {
  const char *code;
  struct {
    TzStr message, class_name, field;
    TzArray *fields;
  } details;
} FillUnknownParts;

typedef struct {
  const char *code;
  struct {
    TzStr message, class_name;
  } details;
} FillClassParts;

static TzStr utf8_text(const char *text, size_t length) {
  TzBuf buf;
  tz_buf_init(&buf);
  tz_buf_utf8(&buf, text, length);

  return tz_buf_take(&buf);
}

static TzStr format_text(const char *format, ...) {
  char text[1024];
  va_list args;
  va_start(args, format);
  int length = vsnprintf(text, sizeof text, format, args);
  va_end(args);
  if (length < 0) {
    length = 0;
  }
  if ((size_t)length >= sizeof text) {
    length = (int)sizeof text - 1;
  }

  return utf8_text(text, (size_t)length);
}

static size_t units_utf8(TzStr s, size_t limit, char *out, size_t size) {
  uint32_t units;
  int two;
  const void *data = tz_str_units(&s, &units, &two);
  size_t n = 0;
  uint32_t shown = units < limit ? units : (uint32_t)limit;
  for (uint32_t i = 0; i < shown && n + 4 < size; ++i) {
    uint32_t c = two ? ((const uint16_t *)data)[i] : ((const uint8_t *)data)[i];
    if (c < 0x80) {
      out[n++] = (char)c;
    } else if (c < 0x800) {
      out[n++] = (char)(0xC0 | (c >> 6));
      out[n++] = (char)(0x80 | (c & 0x3F));
    } else {
      out[n++] = (char)(0xE0 | (c >> 12));
      out[n++] = (char)(0x80 | ((c >> 6) & 0x3F));
      out[n++] = (char)(0x80 | (c & 0x3F));
    }
  }
  if (shown < units && n + 4 < size) {
    out[n++] = (char)0xE2;
    out[n++] = (char)0x80;
    out[n++] = (char)0xA6;
  }
  out[n] = 0;

  return n;
}

static const char *kind_name(const void *u, char *out, size_t size) {
  if (!u) {
    return "undefined";
  }
  const TzBox *box = as_box(u);
  if (box) {
    uint32_t kind = box->type->kind;
    if (tz_kind_integer(kind)) {
      return "int";
    }
    switch (kind) {
    case TZ_T_DOUBLE:
      return "double";
    case TZ_T_STRING:
    case TZ_T_ATOM:
      return "string";
    case TZ_T_BOOL:
      return "boolean";
    case TZ_T_JSON:
      return "Json";
    case TZ_T_ARRAY:
      return "array";
    case TZ_T_FUNCTION:
      return "function";
    case TZ_T_DECIMAL:
      return "decimal";
    default:
      return "value";
    }
  }
  const TzLayout *layout = ((const TzInstance *)u)->layout;
  if (layout->flags & TZ_LAYOUT_CLASS) {
    return "class";
  }
  if (layout->name && layout->name_length) {
    snprintf(out, size, "%.*s", (int)layout->name_length, layout->name);
    return out;
  }

  return "object";
}

static void fill_set_failed(TzMaker make, void *parts) { tz_set_failed(make(parts)); }

void tz_reflect_fill_mismatch(const char *cls, uint32_t cls_length, const char *field,
                              uint32_t field_length, int32_t index, const char *expected,
                              uint32_t expected_length, const void *got, TzMaker make) {
  char got_name[128];
  const char *got_kind = kind_name(got, got_name, sizeof got_name);
  TzStr shown;
  tz_lib_inspect(&shown, got, NULL);
  char value[512];
  units_utf8(shown, 80, value, sizeof value);
  tz_str_release_value(shown);
  char place[256];
  char path[256];
  if (index >= 0) {
    snprintf(place, sizeof place, "element %d of field %.*s", (int)index, (int)field_length, field);
    snprintf(path, sizeof path, "%.*s[%d]", (int)field_length, field, (int)index);
  } else {
    snprintf(place, sizeof place, "field %.*s", (int)field_length, field);
    snprintf(path, sizeof path, "%.*s", (int)field_length, field);
  }
  FillTypeParts parts;
  parts.code = "ERR_INVALID_FIELD_TYPE";
  if (!got) {
    parts.details.message =
        format_text("could not fill %.*s: %s is %.*s, and the value given is undefined; leave "
                    "the key out to keep the value the class starts with",
                    (int)cls_length, cls, place, (int)expected_length, expected);
  } else if (as_box(got)) {
    parts.details.message =
        format_text("could not fill %.*s: %s is %.*s, and the value given is the %s %s; check "
                    "and convert the values before Reflect.fill",
                    (int)cls_length, cls, place, (int)expected_length, expected, got_kind, value);
  } else {
    parts.details.message =
        format_text("could not fill %.*s: %s is %.*s, and the value given is %s; check and "
                    "convert the values before Reflect.fill",
                    (int)cls_length, cls, place, (int)expected_length, expected, value);
  }
  parts.details.class_name = utf8_text(cls, cls_length);
  parts.details.field = utf8_text(path, strlen(path));
  parts.details.expected = utf8_text(expected, expected_length);
  parts.details.got = utf8_text(got_kind, strlen(got_kind));
  parts.details.value = utf8_text(value, strlen(value));
  fill_set_failed(make, &parts);
}

int32_t tz_reflect_fill_keys(const void *values, const TzFillName *names, uint32_t count,
                             const char *cls, uint32_t cls_length, TzMaker make) {
  const TzMap *map = (const TzMap *)values;
  uint32_t at = 0;
  for (char *e = (char *)tz_map_next(map, &at); e; e = (char *)tz_map_next(map, &at)) {
    TzStr key = *(const TzStr *)(e + map->layout->key_offset);
    int known = 0;
    for (uint32_t i = 0; i < count && !known; ++i) {
      known = same_name(names[i].name, names[i].length, key);
    }
    if (known) {
      continue;
    }
    char field[256];
    units_utf8(key, 120, field, sizeof field);
    char list[512];
    size_t n = 0;
    list[0] = 0;
    for (uint32_t i = 0; i < count && n + 4 < sizeof list; ++i) {
      int wrote = snprintf(list + n, sizeof list - n, "%s%.*s", i ? ", " : "", (int)names[i].length,
                           names[i].name);
      if (wrote < 0) {
        break;
      }
      n += (size_t)wrote < sizeof list - n ? (size_t)wrote : sizeof list - n - 1;
    }
    FillUnknownParts parts;
    parts.code = "ERR_UNKNOWN_FIELD";
    parts.details.message =
        count ? format_text("could not fill %.*s: it has no field %s; its fields are %s",
                            (int)cls_length, cls, field, list)
              : format_text("could not fill %.*s: it has no field %s, and no fields at all",
                            (int)cls_length, cls, field);
    parts.details.class_name = utf8_text(cls, cls_length);
    parts.details.field = utf8_text(field, strlen(field));
    parts.details.fields = tz_arr_new(&tz_string_type, 0, count);
    for (uint32_t i = 0; i < count; ++i) {
      TzStr name = utf8_text(names[i].name, names[i].length);
      tz_arr_push_value(parts.details.fields, &name);
    }
    fill_set_failed(make, &parts);
    return 0;
  }

  return 1;
}

void *tz_reflect_fill_value(const void *values, const char *name, uint32_t length, int32_t *found) {
  const TzMap *map = (const TzMap *)values;
  *found = 0;
  uint32_t at = 0;
  for (char *e = (char *)tz_map_next(map, &at); e; e = (char *)tz_map_next(map, &at)) {
    if (same_name(name, length, *(const TzStr *)(e + map->layout->key_offset))) {
      *found = 1;
      return *(void *const *)(e + map->layout->value_offset);
    }
  }

  return NULL;
}

int32_t tz_reflect_fill_exact(const void *u, uint32_t want) {
  const TzBox *box = as_box(u);
  if (!box) {
    return 0;
  }
  uint32_t kind = box->type->kind;
  if (tz_kind_integer(want)) {
    return tz_kind_integer(kind);
  }

  return kind == want;
}

int32_t tz_reflect_fill_atom(const void *u, const TzFillName *texts, uint32_t count) {
  const TzBox *box = as_box(u);
  if (!box || box->type->kind != TZ_T_STRING) {
    return -1;
  }
  TzStr text = *(const TzStr *)TZ_BOX_VALUE(box);
  for (uint32_t i = 0; i < count; ++i) {
    if (same_name(texts[i].name, texts[i].length, text)) {
      return (int32_t)i;
    }
  }

  return -1;
}

static const TzType fill_unknown_type = {TZ_T_UNKNOWN, sizeof(void *), NULL};
static const TzType fill_unknown_array = {TZ_T_ARRAY, sizeof(void *), &fill_unknown_type};

void *tz_reflect_fill_array(const void *u, const TzType *array_type,
                            int32_t (*convert)(const void *u, void *out), const char *cls,
                            uint32_t cls_length, const char *field, uint32_t field_length,
                            const char *expected, uint32_t expected_length, TzMaker make) {
  if (!u || !tz_unknown_is_array(u)) {
    char wanted[256];
    snprintf(wanted, sizeof wanted, "%.*s[]", (int)expected_length, expected);
    tz_reflect_fill_mismatch(cls, cls_length, field, field_length, -1, wanted, strlen(wanted), u,
                             make);
    return NULL;
  }
  TzArray *items = (TzArray *)tz_unknown_array(u, &fill_unknown_array);
  const TzType *element = array_type->inner;
  TzArray *out = tz_arr_new(element, 0, items->length);
  char small[64];
  char *slot = element->size <= sizeof small ? small : (char *)tz_alloc(element->size);
  for (uint32_t i = 0; i < items->length; ++i) {
    void *item = ((void **)items->data)[i];
    if (!convert(item, slot)) {
      tz_reflect_fill_mismatch(cls, cls_length, field, field_length, (int32_t)i, expected,
                               expected_length, item, make);
      tz_value_release(array_type, &out);
      tz_value_release(&fill_unknown_array, &items);
      if (slot != small) {
        tz_free(slot);
      }
      return NULL;
    }
    tz_arr_push_value(out, slot);
  }
  tz_value_release(&fill_unknown_array, &items);
  if (slot != small) {
    tz_free(slot);
  }

  return out;
}

void *tz_reflect_fill(const void *cls, const void *values, TzMaker make) {
  const TzInstance *object = (const TzInstance *)cls;
  const TzClass *klass = object && !as_box(cls) && (object->layout->flags & TZ_LAYOUT_CLASS)
                             ? (const TzClass *)cls
                             : NULL;
  if (klass && klass->info && klass->info->fill) {
    return klass->info->fill(values);
  }
  const TzLayout *layout = klass ? klass->layout : NULL;
  int name_length = layout ? (int)layout->name_length : 5;
  const char *name = layout ? layout->name : "value";
  FillClassParts parts;
  uint32_t problem = klass && klass->info ? klass->info->fill_problem : TZ_FILL_OK;
  if (problem == TZ_FILL_CONSTRUCTOR) {
    parts.code = "ERR_HAS_CONSTRUCTOR";
    parts.details.message =
        format_text("could not fill %.*s: it sets its fields in a constructor; make it with "
                    "Reflect.construct(%.*s, [...])",
                    name_length, name, name_length, name);
  } else if (problem == TZ_FILL_ABSTRACT) {
    parts.code = "ERR_ABSTRACT_CLASS";
    parts.details.message = format_text(
        "could not fill %.*s: it is abstract; fill a class that extends it", name_length, name);
  } else {
    parts.code = "ERR_NOT_REFLECTED";
    parts.details.message = format_text(
        "could not fill %.*s: it is not a class of the program, which Reflect.fill cannot make",
        name_length, name);
  }
  parts.details.class_name = utf8_text(name, (size_t)name_length);
  fill_set_failed(make, &parts);

  return NULL;
}

static const TzClass *as_class(const void *u) {
  if (u && !as_box(u) && (((const TzInstance *)u)->layout->flags & TZ_LAYOUT_CLASS)) {
    return (const TzClass *)u;
  }
  char given[160];
  describe(u, given, sizeof given);
  reflect_fail("ERR_NOT_CLASS", "%s is not a constructor", given);

  return NULL;
}

void *tz_reflect_new(const TzClass *cls, void *const *args, uint32_t count) {
  const TzLayout *layout = cls->layout;
  if (!cls->info) {
    reflect_fail("ERR_NOT_REFLECTED", "%.*s of JavaScript cannot be made through reflection",
                 (int)layout->name_length, layout->name);
    return NULL;
  }
  if (!cls->info->construct) {
    reflect_fail("ERR_ABSTRACT_CLASS", "class %.*s is abstract and cannot be made",
                 (int)layout->name_length, layout->name);
    return NULL;
  }

  return cls->info->construct(args, count);
}

static void as_unknowns(const TzArray *array, void **out) {
  for (uint32_t i = 0; i < array->length; ++i) {
    char *element = array->data + (size_t)i * array->type->size;
    tz_value_retain(array->type, element);
    out[i] = tz_unknown_of(array->type, element);
  }
}

static void release_unknowns(void **values, uint32_t count) {
  for (uint32_t i = 0; i < count; ++i) {
    tz_instance_release((TzInstance *)values[i]);
  }
}

void *tz_reflect_construct(const TzClass *cls, const TzArray *args) {
  if (!as_class(cls)) {
    return NULL;
  }
  void *small[8];
  void **values = args->length <= 8 ? small : (void **)tz_alloc(args->length * sizeof(void *));
  as_unknowns(args, values);
  void *result = tz_reflect_new(cls, values, args->length);
  release_unknowns(values, args->length);
  if (values != small) {
    tz_free(values);
  }

  return result;
}

void *tz_reflect_make(void *(*construct)(void *const *args, uint32_t count), const TzArray *args) {
  void *small[8];
  void **values = args->length <= 8 ? small : (void **)tz_alloc(args->length * sizeof(void *));
  as_unknowns(args, values);
  void *result = construct(values, args->length);
  release_unknowns(values, args->length);
  if (values != small) {
    tz_free(values);
  }

  return result;
}

static const TzClassInfo *info_of(const TzInstance *object) {
  if (object->layout->flags & TZ_LAYOUT_CLASS) {
    return NULL;
  }
  uint32_t id = object->layout->class_id;
  if (!id || id >= tz_reflect_store.class_count || !tz_reflect_store.classes[id]) {
    return NULL;
  }

  return tz_reflect_store.classes[id]->info;
}

void *tz_reflect_get(const void *target, void *key_obj, uint64_t key_meta) {
  TzStr key = {key_obj, key_meta};
  if (!target || as_box(target)) {
    return NULL;
  }
  const TzInstance *object = (const TzInstance *)target;
  const TzLayout *layout = object->layout;
  for (uint32_t i = 0; i < layout->count; ++i) {
    const TzField *field = &layout->fields[i];
    if (field->offset == TZ_FIELD_METHOD || !same_name(field->name, field->name_length, key)) {
      continue;
    }
    char *value = (char *)object + field->offset;
    tz_value_retain(field->type, value);

    return tz_unknown_of(field->type, value);
  }
  if ((layout->flags & TZ_LAYOUT_CLASS) && same_name("name", 4, key)) {
    TzStr name;
    tz_class_name(&name, (const TzClass *)object);
    return tz_unknown_box(&tz_string_type, &name);
  }
  const TzClassInfo *info = info_of(object);
  if (info) {
    for (uint32_t i = 0; i < info->method_count; ++i) {
      const TzReflectMethod *method = &info->methods[i];
      if (!same_name(method->name, method->name_length, key)) {
        continue;
      }
      TzFunction fn = {method->info, NULL};

      return tz_unknown_box(method->type, &fn);
    }
  }

  return NULL;
}

void *tz_reflect_apply(const void *f, void *self, const TzArray *args) {
  const TzBox *box = as_box(f);
  if (!box || box->type->kind != TZ_T_FUNCTION) {
    char given[160];
    describe(f, given, sizeof given);
    reflect_fail("ERR_NOT_FUNCTION",
                 "Function.prototype.apply was called on %s, which is not a function", given);
    return NULL;
  }
  TzInvoker invoke = NULL;
  for (uint32_t i = 0; i < tz_reflect_store.invoker_count && !invoke; ++i) {
    if (tz_reflect_store.invokers[i].type == box->type) {
      invoke = tz_reflect_store.invokers[i].invoke;
    }
  }
  if (!invoke) {
    reflect_fail("ERR_NOT_FUNCTION", "Reflect.apply cannot call this function");
    return NULL;
  }
  void *small[8];
  void **values = args->length <= 8 ? small : (void **)tz_alloc(args->length * sizeof(void *));
  as_unknowns(args, values);
  void *result = invoke((const TzFunction *)TZ_BOX_VALUE(box), self, values, args->length);
  release_unknowns(values, args->length);
  if (values != small) {
    tz_free(values);
  }

  return result;
}

static int box_number(const void *u, double *out) {
  const TzBox *box = as_box(u);
  if (!box) {
    return 0;
  }
  const char *value = TZ_BOX_VALUE(box);
  if (tz_kind_integer(box->type->kind)) {
    *out = tz_integer_double(box->type->kind, tz_load_integer(box->type->kind, value));
    return 1;
  }
  switch (box->type->kind) {
  case TZ_T_DOUBLE:
    *out = *(const double *)value;
    return 1;
  case TZ_T_JSON:
    if (tz_json_kind(*(const TzJson *const *)value) != TZ_JSON_NUMBER) {
      return 0;
    }
    *out = tz_unknown_number(u);
    return 1;
  default:
    return 0;
  }
}

int32_t tz_unknown_to_double(const void *u, double *out) { return box_number(u, out); }

int32_t tz_unknown_to_integer(const void *u, int64_t *out, int32_t kind) {
  const TzBox *box = as_box(u);
  if (box && tz_kind_integer(box->type->kind)) {
    int64_t v = tz_load_integer(box->type->kind, TZ_BOX_VALUE(box));
    if (!tz_integer_fits((uint32_t)kind, v, box->type->kind == TZ_T_UINT64)) {
      return 0;
    }
    *out = v;
    return 1;
  }
  double n;
  if (!box_number(u, &n) || n != floor(n) || (n == 0 && 1 / n < 0) || n < -9223372036854775808.0 ||
      n >= (kind == TZ_T_UINT64 ? 18446744073709551616.0 : 9223372036854775808.0)) {
    return 0;
  }
  int64_t v = n >= 9223372036854775808.0 ? (int64_t)(uint64_t)n : (int64_t)n;
  if (!tz_integer_fits((uint32_t)kind, v, n >= 9223372036854775808.0)) {
    return 0;
  }
  *out = v;

  return 1;
}

int32_t tz_unknown_to_bool(const void *u, int32_t *out) {
  if (tz_unknown_kind(u) != TZ_KIND_BOOLEAN) {
    return 0;
  }
  *out = tz_unknown_bool(u);

  return 1;
}

int32_t tz_unknown_to_string(const void *u, TzStr *out) {
  if (tz_unknown_kind(u) != TZ_KIND_STRING) {
    return 0;
  }
  tz_unknown_string(out, u);

  return 1;
}

int32_t tz_unknown_to_box(const void *u, const TzType *type, void *out) {
  const TzBox *box = as_box(u);
  if (!box || (box->type != type && !(type->kind == TZ_T_JSON && box->type->kind == TZ_T_JSON))) {
    return 0;
  }
  memcpy(out, TZ_BOX_VALUE(box), type->size);
  tz_value_retain(type, out);

  return 1;
}

const TzLayout *tz_unknown_layout(const void *u) {
  return u && !as_box(u) ? ((const TzInstance *)u)->layout : NULL;
}

uint32_t tz_unknown_box_kind(const void *u) {
  const TzBox *box = as_box(u);

  return box ? box->type->kind : 0;
}

int32_t tz_unknown_holds(const void *u, const TzType *type) {
  const TzBox *box = as_box(u);

  return box && (box->type == type || (type->kind == TZ_T_JSON && box->type->kind == TZ_T_JSON));
}
