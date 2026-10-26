#include "runtime/core/array/array.h"
#include "runtime/core/closure/closure.h"
#include "runtime/core/string/string.h"
#include "runtime/core/tzrt/tzrt.h"
#include <string.h>

static int holds_value(const TzField *field, const TzInstance *object) {
  if (tz_field_is_symbol(object->layout, field)) {
    return 0;
  }
  if ((object->layout->flags & TZ_LAYOUT_LIBRARY) && field->name_length == 13 &&
      memcmp(field->name, "_maxListeners", 13) == 0) {
    return 1;
  }
  if (field->offset == TZ_FIELD_GETTER) {
    return 0;
  }

  return field->offset == TZ_FIELD_METHOD || field->type->kind != TZ_T_OPTIONAL ||
         tz_optional_has(field->type, (const char *)object + field->offset);
}

static uint32_t count_values(const TzInstance *object) {
  uint32_t count = 0;
  for (uint32_t i = 0; i < object->layout->count; ++i) {
    count += holds_value(&object->layout->fields[i], object);
  }

  return count;
}

static TzStr field_name(const TzField *field) {
  const unsigned char *text = (const unsigned char *)field->name;
  size_t n = field->name_length;
  int ascii = 1;
  for (size_t i = 0; i < n && ascii; ++i) {
    ascii = text[i] < 0x80;
  }
  if (ascii) {
    return tz_str_from_ascii(field->name, n);
  }
  TzBuf b;
  tz_buf_init(&b);
  for (size_t i = 0; i < n;) {
    uint32_t c = text[i];
    size_t extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    if (extra) {
      c &= 0x3Fu >> extra;
    }
    for (size_t k = 1; k <= extra && i + k < n; ++k) {
      c = (c << 6) | (text[i + k] & 0x3Fu);
    }
    i += extra + 1;
    if (c >= 0x10000) {
      c -= 0x10000;
      tz_buf_push(&b, (uint16_t)(0xD800 + (c >> 10)));
      tz_buf_push(&b, (uint16_t)(0xDC00 + (c & 0x3FF)));
    } else {
      tz_buf_push(&b, (uint16_t)c);
    }
  }

  return tz_buf_take(&b);
}

TzArray *tz_obj_keys(const TzInstance *object, const TzType *string_type) {
  TzArray *keys = tz_arr_new(string_type, 0, count_values(object));
  for (uint32_t i = 0; i < object->layout->count; ++i) {
    const TzField *field = &object->layout->fields[i];
    if (!holds_value(field, object)) {
      continue;
    }
    TzStr name = field_name(field);
    tz_arr_push_value(keys, &name);
  }

  return keys;
}

static void store_as(const TzType *to, const TzType *from, const char *value, char *out) {
  if (from->kind == TZ_T_OPTIONAL && to->kind == TZ_T_UNION) {
    from = from->inner;
  }
  for (uint32_t k = 0; k < to->size; ++k) {
    out[k] = 0;
  }
  if (to->kind != TZ_T_UNION || from->kind == TZ_T_UNION) {
    uint32_t size = from->size < to->size ? from->size : to->size;
    for (uint32_t k = 0; k < size; ++k) {
      out[k] = value[k];
    }
    return;
  }
  const TzUnionType *u = (const TzUnionType *)to;
  uint32_t tag = 0;
  for (uint32_t slot = 1; slot < TZ_UNION_SLOTS && !tag; ++slot) {
    if (u->slots[slot] == from) {
      tag = slot;
    }
  }
  for (uint32_t slot = 1; slot < TZ_UNION_SLOTS && !tag; ++slot) {
    if (u->slots[slot] && u->slots[slot]->kind == from->kind) {
      tag = slot;
    }
  }
  *(uint32_t *)out = tag;
  for (uint32_t k = 0; k < from->size && 8 + k < to->size; ++k) {
    out[8 + k] = value[k];
  }
}

TzArray *tz_obj_values(const TzInstance *object, const TzType *element) {
  TzArray *values = tz_arr_new(element, 0, count_values(object));
  char buffer[64];
  for (uint32_t i = 0; i < object->layout->count; ++i) {
    const TzField *field = &object->layout->fields[i];
    if (!holds_value(field, object)) {
      continue;
    }
    store_as(element, field->type, (const char *)object + field->offset, buffer);
    tz_value_retain(element, buffer);
    tz_arr_push_value(values, buffer);
  }

  return values;
}

TzArray *tz_obj_entries(const TzInstance *object, const TzType *tuple_type,
                        const TzLayout *tuple_layout, int64_t tuple_size) {
  TzArray *entries = tz_arr_new(tuple_type, 0, count_values(object));
  const TzField *key = &tuple_layout->fields[0], *value = &tuple_layout->fields[1];
  for (uint32_t i = 0; i < object->layout->count; ++i) {
    const TzField *field = &object->layout->fields[i];
    if (!holds_value(field, object)) {
      continue;
    }
    TzInstance *entry = (TzInstance *)tz_obj_alloc(tuple_size);
    entry->rc = 1;
    entry->layout = tuple_layout;
    *(TzStr *)((char *)entry + key->offset) = field_name(field);
    char *slot = (char *)entry + value->offset;
    store_as(value->type, field->type, (const char *)object + field->offset, slot);
    tz_value_retain(value->type, slot);
    tz_arr_push_value(entries, &entry);
  }

  return entries;
}
