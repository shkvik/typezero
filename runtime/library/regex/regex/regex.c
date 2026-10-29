#include "runtime/library/regex/regex/regex.h"
#include "runtime/core/output/output.h"
#include "runtime/core/fail/fail.h"
#include "runtime/core/alloc/alloc.h"
#include "runtime/core/array/array.h"
#include "runtime/core/closure/closure.h"
#include "runtime/core/error/error.h"
#include "runtime/core/string/string.h"
#include "runtime/core/tzrt/tzrt.h"
#include <stdio.h>
#include <string.h>

enum {
  E_CHOICE,
  E_CAPTURE,
  E_REGISTER,
  E_REPEAT_GREEDY,
  E_REPEAT_LAZY,
};

typedef struct {
  int32_t kind;
  uint32_t pc;
  int32_t pos;
  int32_t n;
} ReEntry;

typedef struct {
  const uint32_t *code;
  const uint8_t *s8;
  const uint16_t *s16;
  int32_t length;
  int unicode;
  int32_t *caps;
  int32_t *regs;
} Vm;

TZ_THREAD_LOCAL ReEntry *tz_re_stack;
TZ_THREAD_LOCAL size_t tz_re_stack_size, tz_re_stack_capacity;

TZ_NOINLINE static void grow_stack(void) {
  size_t capacity = tz_re_stack_capacity * 2 + 256;
  ReEntry *stack = (ReEntry *)tz_alloc(capacity * sizeof(ReEntry));
  if (tz_re_stack_size) {
    memcpy(stack, tz_re_stack, tz_re_stack_size * sizeof(ReEntry));
  }
  tz_free(tz_re_stack);
  tz_re_stack = stack;
  tz_re_stack_capacity = capacity;
}

static inline void push(int32_t kind, uint32_t pc, int32_t pos, int32_t n) {
  if (tz_re_stack_size == tz_re_stack_capacity) {
    grow_stack();
  }
  ReEntry *e = &tz_re_stack[tz_re_stack_size++];
  e->kind = kind;
  e->pc = pc;
  e->pos = pos;
  e->n = n;
}

static inline uint32_t unit(const Vm *vm, int32_t i) { return vm->s16 ? vm->s16[i] : vm->s8[i]; }

static inline int is_lead(uint32_t u) { return u >= 0xD800 && u <= 0xDBFF; }
static inline int is_trail(uint32_t u) { return u >= 0xDC00 && u <= 0xDFFF; }

static inline int is_line_terminator(uint32_t c) {
  return c == '\n' || c == '\r' || c == 0x2028 || c == 0x2029;
}

static inline int32_t read_forward(const Vm *vm, int32_t *pos) {
  int32_t p = *pos;
  if (p >= vm->length) {
    return -1;
  }
  uint32_t c = unit(vm, p);
  if (vm->unicode) {
    if (is_lead(c) && p + 1 < vm->length && is_trail(unit(vm, p + 1))) {
      *pos = p + 2;
      return (int32_t)(0x10000 + ((c - 0xD800) << 10) + (unit(vm, p + 1) - 0xDC00));
    }
    if (is_trail(c) && p > 0 && is_lead(unit(vm, p - 1))) {
      return -1;
    }
  }
  *pos = p + 1;

  return (int32_t)c;
}

static inline int32_t read_backward(const Vm *vm, int32_t *pos) {
  int32_t p = *pos;
  if (p <= 0) {
    return -1;
  }
  uint32_t c = unit(vm, p - 1);
  if (vm->unicode) {
    if (is_trail(c) && p >= 2 && is_lead(unit(vm, p - 2))) {
      *pos = p - 2;
      return (int32_t)(0x10000 + ((unit(vm, p - 2) - 0xD800) << 10) + (c - 0xDC00));
    }
    if (is_lead(c) && p < vm->length && is_trail(unit(vm, p))) {
      return -1;
    }
  }
  *pos = p - 1;

  return (int32_t)c;
}

static int in_set(const uint32_t *op, uint32_t c) {
  if (c < 256) {
    return (op[1 + c / 32] >> (c % 32)) & 1;
  }
  const uint32_t *r = op + 9;
  uint32_t lo = 0, hi = op[0];
  while (lo < hi) {
    uint32_t mid = (lo + hi) / 2;
    if (c < r[2 * mid]) {
      hi = mid;
    } else if (c > r[2 * mid + 1]) {
      lo = mid + 1;
    } else {
      return 1;
    }
  }

  return 0;
}

static inline int is_word(uint32_t c, int folded) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
         (folded && (c == 0x017F || c == 0x212A));
}

static int match_one(const Vm *vm, uint32_t pc, int32_t *pos) {
  const uint32_t *op = vm->code + pc;
  int32_t p = *pos;
  int32_t c;
  switch (op[0]) {
  case RE_CHAR:
    if (!vm->unicode || op[1] < 0xD800 || (op[1] > 0xDFFF && op[1] < 0x10000)) {
      if (p >= vm->length || unit(vm, p) != op[1]) {
        return 0;
      }
      *pos = p + 1;
      return 1;
    }
    c = read_forward(vm, &p);
    break;
  case RE_CHAR_BACK:
    if (!vm->unicode || op[1] < 0xD800 || (op[1] > 0xDFFF && op[1] < 0x10000)) {
      if (p <= 0 || unit(vm, p - 1) != op[1]) {
        return 0;
      }
      *pos = p - 1;
      return 1;
    }
    c = read_backward(vm, &p);
    break;
  case RE_ANY:
  case RE_ALL:
  case RE_SET:
    c = read_forward(vm, &p);
    break;
  default:
    c = read_backward(vm, &p);
    break;
  }
  if (c < 0) {
    return 0;
  }
  switch (op[0]) {
  case RE_CHAR:
  case RE_CHAR_BACK:
    if ((uint32_t)c != op[1]) {
      return 0;
    }
    break;
  case RE_ANY:
  case RE_ANY_BACK:
    if (is_line_terminator((uint32_t)c)) {
      return 0;
    }
    break;
  case RE_SET:
  case RE_SET_BACK:
    if (!in_set(op + 1, (uint32_t)c)) {
      return 0;
    }
    break;
  default:
    break;
  }
  *pos = p;

  return 1;
}

static inline void set_capture(Vm *vm, uint32_t slot, int32_t value) {
  if (vm->caps[slot] == value) {
    return;
  }
  push(E_CAPTURE, 0, vm->caps[slot], (int32_t)slot);
  vm->caps[slot] = value;
}

static inline void set_register(Vm *vm, uint32_t r, int32_t value) {
  if (vm->regs[r] == value) {
    return;
  }
  push(E_REGISTER, 0, vm->regs[r], (int32_t)r);
  vm->regs[r] = value;
}

static inline uint32_t fold(uint32_t c, uint32_t flags) {
  if (!(flags & RE_REF_IGNORE_CASE)) {
    return c;
  }

  return tz_re_canonicalize(c, (flags & RE_REF_UNICODE) != 0);
}

static inline int inside_pair(const Vm *vm, int32_t pos) {
  return pos > 0 && pos < vm->length && is_trail(unit(vm, pos)) && is_lead(unit(vm, pos - 1));
}

static int match_backref(const Vm *vm, const uint32_t *op, int32_t *pos) {
  uint32_t flags = op[1], n = op[2];
  int32_t start = -1, end = -1;
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t k = op[3 + i];
    if (vm->caps[2 * k] >= 0 && vm->caps[2 * k + 1] >= 0) {
      start = vm->caps[2 * k];
      end = vm->caps[2 * k + 1];
      break;
    }
  }
  if (start < 0) {
    return !(vm->unicode && inside_pair(vm, *pos));
  }
  int32_t length = end - start;
  int32_t p = *pos;
  int back = (flags & RE_REF_BACK) != 0;
  int32_t from = back ? p - length : p;
  if (from < 0 || from + length > vm->length) {
    return 0;
  }
  if (!(flags & RE_REF_IGNORE_CASE)) {
    for (int32_t i = 0; i < length; ++i) {
      if (unit(vm, start + i) != unit(vm, from + i)) {
        return 0;
      }
    }
  } else {
    Vm view = *vm;
    view.unicode = (flags & RE_REF_UNICODE) != 0;
    int32_t a = start, b = from;
    while (a < end) {
      int32_t x = read_forward(&view, &a), y = read_forward(&view, &b);
      if (b > from + length || fold((uint32_t)x, flags) != fold((uint32_t)y, flags)) {
        return 0;
      }
    }
    if (b != from + length) {
      return 0;
    }
  }
  *pos = back ? from : from + length;

  return !(vm->unicode && inside_pair(vm, *pos));
}

static int run(Vm *vm, uint32_t pc, int32_t pos, int32_t *end);

static int32_t next_char(const uint32_t *code, uint32_t pc, int unicode) {
  for (;;) {
    const uint32_t *op = code + pc;
    switch (op[0]) {
    case RE_SET_POSITION:
      pc += 2;
      continue;
    case RE_SAVE:
    case RE_RESET:
      pc += 3;
      continue;
    case RE_CHAR:
      if (unicode && op[1] >= 0xD800 && (op[1] <= 0xDFFF || op[1] >= 0x10000)) {
        return -1;
      }
      return (int32_t)op[1];
    default:
      return -1;
    }
  }
}

static void drop_choices(size_t mark) {
  size_t kept = mark;
  for (size_t i = mark; i < tz_re_stack_size; ++i) {
    if (tz_re_stack[i].kind == E_CAPTURE || tz_re_stack[i].kind == E_REGISTER) {
      tz_re_stack[kept++] = tz_re_stack[i];
    }
  }
  tz_re_stack_size = kept;
}

static void unwind(Vm *vm, size_t mark) {
  while (tz_re_stack_size > mark) {
    ReEntry *e = &tz_re_stack[--tz_re_stack_size];
    if (e->kind == E_CAPTURE) {
      vm->caps[e->n] = e->pos;
    } else if (e->kind == E_REGISTER) {
      vm->regs[e->n] = e->pos;
    }
  }
}

static int run(Vm *vm, uint32_t pc, int32_t pos, int32_t *end) {
  const uint32_t *code = vm->code;
  size_t base = tz_re_stack_size;
  for (;;) {
    const uint32_t *op = code + pc;
    switch (op[0]) {
    case RE_MATCH:
      *end = pos;
      return 1;
    case RE_CHAR:
      if (!vm->unicode || op[1] < 0xD800 || (op[1] > 0xDFFF && op[1] < 0x10000)) {
        if (pos >= vm->length || unit(vm, pos) != op[1]) {
          goto fail;
        }
        ++pos;
        pc += 2;
        continue;
      }
      if (!match_one(vm, pc, &pos)) {
        goto fail;
      }
      pc += 2;
      continue;
    case RE_CHAR_BACK:
    case RE_ANY:
    case RE_ANY_BACK:
    case RE_ALL:
    case RE_ALL_BACK:
      if (!match_one(vm, pc, &pos)) {
        goto fail;
      }
      pc += op[0] == RE_CHAR_BACK ? 2 : 1;
      continue;
    case RE_SET:
    case RE_SET_BACK:
      if (!match_one(vm, pc, &pos)) {
        goto fail;
      }
      pc += 10 + 2 * op[1];
      continue;
    case RE_LINE_START:
      if (pos != 0) {
        goto fail;
      }
      ++pc;
      continue;
    case RE_LINE_START_M:
      if (pos != 0 && !is_line_terminator(unit(vm, pos - 1))) {
        goto fail;
      }
      ++pc;
      continue;
    case RE_LINE_END:
      if (pos != vm->length) {
        goto fail;
      }
      ++pc;
      continue;
    case RE_LINE_END_M:
      if (pos != vm->length && !is_line_terminator(unit(vm, pos))) {
        goto fail;
      }
      ++pc;
      continue;
    case RE_WORD_BOUNDARY:
    case RE_NOT_WORD_BOUNDARY:
    case RE_WORD_BOUNDARY_I:
    case RE_NOT_WORD_BOUNDARY_I: {
      int folded = op[0] == RE_WORD_BOUNDARY_I || op[0] == RE_NOT_WORD_BOUNDARY_I;
      int a = pos > 0 && is_word(unit(vm, pos - 1), folded);
      int b = pos < vm->length && is_word(unit(vm, pos), folded);
      int boundary = op[0] == RE_WORD_BOUNDARY || op[0] == RE_WORD_BOUNDARY_I;
      if ((a != b) != boundary) {
        goto fail;
      }
      ++pc;
      continue;
    }
    case RE_GOTO:
      pc = op[1];
      continue;
    case RE_SPLIT_NEXT:
      push(E_CHOICE, op[1], pos, 0);
      pc += 2;
      continue;
    case RE_SPLIT_GOTO:
      push(E_CHOICE, pc + 2, pos, 0);
      pc = op[1];
      continue;
    case RE_SET_POSITION:
      set_register(vm, op[1], pos);
      pc += 2;
      continue;
    case RE_SAVE:
      set_capture(vm, 2 * op[1], vm->regs[op[2]]);
      set_capture(vm, 2 * op[1] + 1, pos);
      pc += 3;
      continue;
    case RE_SAVE_BACK:
      set_capture(vm, 2 * op[1], pos);
      set_capture(vm, 2 * op[1] + 1, vm->regs[op[2]]);
      pc += 3;
      continue;
    case RE_RESET:
      for (uint32_t k = op[1]; k < op[2]; ++k) {
        set_capture(vm, 2 * k, -1);
        set_capture(vm, 2 * k + 1, -1);
      }
      pc += 3;
      continue;
    case RE_SET_REGISTER:
      set_register(vm, op[1], (int32_t)op[2]);
      pc += 3;
      continue;
    case RE_LOOP: {
      uint32_t n = (uint32_t)vm->regs[op[1]];
      if (n < op[2]) {
        pc = op[5];
      } else if (op[3] != RE_INFINITY && n >= op[3]) {
        pc = op[6];
      } else if (op[4]) {
        push(E_CHOICE, op[6], pos, 0);
        pc = op[5];
      } else {
        push(E_CHOICE, op[5], pos, 0);
        pc = op[6];
      }
      continue;
    }
    case RE_LOOP_END: {
      uint32_t n = (uint32_t)vm->regs[op[1]];
      if (n >= op[2] && pos == vm->regs[op[3]]) {
        goto fail;
      }
      if (n < RE_INFINITY - 1) {
        set_register(vm, op[1], (int32_t)(n + 1));
      }
      pc = op[4];
      continue;
    }
    case RE_REPEAT: {
      uint32_t min = op[1], max = op[2], atom = pc + 5, next = atom + op[4];
      uint32_t count = 0;
      int32_t p = pos, min_pos = pos;
      if (op[3] && !vm->unicode && (code[atom] == RE_SET || code[atom] == RE_CHAR)) {
        const uint32_t *set = code + atom + 1;
        int32_t limit = max < (uint32_t)(vm->length - pos) ? pos + (int32_t)max : vm->length;
        if (code[atom] == RE_CHAR) {
          while (p < limit && unit(vm, p) == set[0]) {
            ++p;
          }
        } else if (vm->s8) {
          while (p < limit && ((set[1 + vm->s8[p] / 32] >> (vm->s8[p] % 32)) & 1)) {
            ++p;
          }
        } else {
          while (p < limit && in_set(set, vm->s16[p])) {
            ++p;
          }
        }
        count = (uint32_t)(p - pos);
        if (count < min) {
          goto fail;
        }
        min_pos = pos + (int32_t)min;
        if (count > min) {
          push(E_REPEAT_GREEDY, pc, p, min_pos);
        }
      } else if (op[3]) {
        while (count < max && match_one(vm, atom, &p)) {
          if (++count == min) {
            min_pos = p;
          }
        }
        if (count < min) {
          goto fail;
        }
        if (min == 0) {
          min_pos = pos;
        }
        if (count > min) {
          push(E_REPEAT_GREEDY, pc, p, min_pos);
        }
      } else {
        while (count < min && match_one(vm, atom, &p)) {
          ++count;
        }
        if (count < min) {
          goto fail;
        }
        if (count < max) {
          push(E_REPEAT_LAZY, pc, p, (int32_t)count);
        }
      }
      pos = p;
      pc = next;
      continue;
    }
    case RE_BACKREF:
      if (!match_backref(vm, op, &pos)) {
        goto fail;
      }
      pc += 3 + op[2];
      continue;
    case RE_LOOK: {
      size_t mark = tz_re_stack_size;
      int32_t ignored;
      int matched = run(vm, pc + 3, pos, &ignored);
      if (op[1] & RE_LOOK_NEGATIVE) {
        if (matched) {
          unwind(vm, mark);
          goto fail;
        }
      } else {
        if (!matched) {
          goto fail;
        }
        drop_choices(mark);
      }
      pc = op[2];
      continue;
    }
    default:
      goto fail;
    }

  fail:
    for (;;) {
      if (tz_re_stack_size == base) {
        return 0;
      }
      ReEntry *e = &tz_re_stack[--tz_re_stack_size];
      switch (e->kind) {
      case E_CAPTURE:
        vm->caps[e->n] = e->pos;
        continue;
      case E_REGISTER:
        vm->regs[e->n] = e->pos;
        continue;
      case E_CHOICE:
        pc = e->pc;
        pos = e->pos;
        break;
      case E_REPEAT_GREEDY: {
        const uint32_t *rep = code + e->pc;
        uint32_t atom = e->pc + 5;
        int back = code[atom] == RE_CHAR_BACK || code[atom] == RE_ANY_BACK ||
                   code[atom] == RE_ALL_BACK || code[atom] == RE_SET_BACK;
        int32_t p = e->pos, min_pos = e->n;
        uint32_t next = e->pc + 5 + rep[4];
        int32_t hint = back ? -1 : next_char(code, next, vm->unicode);
        if (!back) {
          p -= vm->unicode && p - 2 >= min_pos && is_trail(unit(vm, p - 1)) &&
                       is_lead(unit(vm, p - 2))
                   ? 2
                   : 1;
          if (hint >= 0) {
            while (p > min_pos && unit(vm, p) != (uint32_t)hint) {
              --p;
            }
            if (unit(vm, p) != (uint32_t)hint) {
              continue;
            }
          }
        } else {
          p += vm->unicode && p + 2 <= min_pos && is_lead(unit(vm, p)) && is_trail(unit(vm, p + 1))
                   ? 2
                   : 1;
        }
        if (p != min_pos) {
          e->pos = p;
          ++tz_re_stack_size;
        }
        pc = next;
        pos = p;
        break;
      }
      case E_REPEAT_LAZY: {
        const uint32_t *rep = code + e->pc;
        int32_t p = e->pos;
        uint32_t count = (uint32_t)e->n;
        if (!match_one(vm, e->pc + 5, &p)) {
          continue;
        }
        ++count;
        if (count < rep[2]) {
          e->pos = p;
          e->n = (int32_t)count;
          ++tz_re_stack_size;
        }
        pc = e->pc + 5 + rep[4];
        pos = p;
        break;
      }
      default:
        continue;
      }
      break;
    }
  }
}

TZ_THREAD_LOCAL int32_t *tz_re_registers;
TZ_THREAD_LOCAL size_t tz_re_register_capacity;

static int interpret_one(const uint32_t *program, const void *units, int32_t wide, int32_t length,
                         int32_t start, int32_t *caps, int32_t sticky) {
  uint32_t flags = program[TZ_RE_H_FLAGS];
  uint32_t captures = program[TZ_RE_H_CAPTURES];
  uint32_t registers = program[TZ_RE_H_REGISTERS];
  if (registers > tz_re_register_capacity) {
    tz_free(tz_re_registers);
    tz_re_register_capacity = registers * 2;
    tz_re_registers = (int32_t *)tz_alloc(tz_re_register_capacity * sizeof(int32_t));
  }
  Vm vm;
  vm.code = program + program[TZ_RE_H_CODE];
  vm.s8 = wide ? NULL : (const uint8_t *)units;
  vm.s16 = wide ? (const uint16_t *)units : NULL;
  vm.length = length;
  vm.unicode = (flags & (TZ_RE_UNICODE | TZ_RE_UNICODE_SETS)) != 0;
  vm.caps = caps;
  vm.regs = tz_re_registers;
  const uint32_t *first = program + program[TZ_RE_H_FIRST];
  uint32_t first_flags = first[8];
  int filter = (first_flags & TZ_RE_FIRST_KNOWN) && !sticky;
  int wide_first = (first_flags & TZ_RE_FIRST_WIDE) != 0;

  if (vm.unicode && start > 0 && start < length && is_trail(unit(&vm, start)) &&
      is_lead(unit(&vm, start - 1))) {
    --start;
  }
  for (int32_t i = start; i <= length;) {
    if ((first_flags & TZ_RE_FIRST_ANCHORED) && i > 0) {
      return 0;
    }
    if (filter) {
      while (i < length) {
        uint32_t c = unit(&vm, i);
        if (c < 256 ? (first[c / 32] >> (c % 32)) & 1 : wide_first) {
          break;
        }
        ++i;
      }
      if (i >= length) {
        return 0;
      }
    }
    for (uint32_t k = 0; k < 2 * captures; ++k) {
      caps[k] = -1;
    }
    tz_re_stack_size = 0;
    int32_t end;
    if (run(&vm, 0, i, &end)) {
      caps[0] = i;
      caps[1] = end;
      tz_re_stack_size = 0;
      return 1;
    }
    if (sticky) {
      return 0;
    }
    ++i;
  }

  return 0;
}

static int32_t advance_units(const void *units, int32_t wide, int32_t length, int32_t index,
                             uint32_t flags) {
  if (wide && (flags & (TZ_RE_UNICODE | TZ_RE_UNICODE_SETS)) && index + 1 < length &&
      is_lead(((const uint16_t *)units)[index]) && is_trail(((const uint16_t *)units)[index + 1])) {
    return index + 2;
  }

  return index + 1;
}

int32_t tz_re_interpret(const uint32_t *program, const void *units, int32_t wide, int32_t length,
                        int32_t start, int32_t *caps, int32_t sticky, int32_t max) {
  uint32_t slots = 2 * program[TZ_RE_H_CAPTURES];
  int32_t found = 0;
  while (found < max && start <= length) {
    int32_t *c = caps + (size_t)found * slots;
    if (!interpret_one(program, units, wide, length, start, c, sticky)) {
      break;
    }
    ++found;
    start = c[0] == c[1] ? advance_units(units, wide, length, c[1], program[TZ_RE_H_FLAGS]) : c[1];
  }

  return found;
}

TZ_THREAD_LOCAL int64_t *tz_re_bt;
TZ_THREAD_LOCAL int32_t tz_re_bt_capacity;

TZ_NOINLINE int64_t *tz_re_bt_grow(int32_t top, int32_t need) {
  int64_t capacity = (int64_t)tz_re_bt_capacity * 2 + 256;
  if (capacity < need) {
    capacity = need;
  }
  if (capacity > INT32_MAX) {
    capacity = INT32_MAX;
  }
  int64_t *stack = (int64_t *)tz_alloc((size_t)capacity * sizeof(int64_t));
  if (top) {
    memcpy(stack, tz_re_bt, (size_t)top * sizeof(int64_t));
  }
  tz_free(tz_re_bt);
  tz_re_bt = stack;
  tz_re_bt_capacity = (int32_t)capacity;

  return stack;
}

int32_t tz_re_bt_drop_choices(int64_t *stack, int32_t mark, int32_t top) {
  int32_t kept = mark;
  for (int32_t i = mark; i < top; ++i) {
    if ((int32_t)stack[i] < 0) {
      stack[kept++] = stack[i];
    }
  }

  return kept;
}

void tz_re_bt_unwind(const int64_t *stack, int32_t mark, int32_t top, int32_t *slots) {
  while (top > mark) {
    int64_t e = stack[--top];
    int32_t code = (int32_t)e;
    if (code < 0) {
      slots[~code] = (int32_t)(e >> 32);
    }
  }
}

int64_t tz_re_read_pair(const uint16_t *units, int32_t length, int32_t pos, int32_t back) {
  Vm vm;
  memset(&vm, 0, sizeof vm);
  vm.s16 = units;
  vm.length = length;
  vm.unicode = 1;
  int32_t c = back ? read_backward(&vm, &pos) : read_forward(&vm, &pos);

  return c < 0 ? -1 : (int64_t)pos << 32 | (uint32_t)c;
}

int32_t tz_re_backref(const void *units, int32_t wide, int32_t length, int32_t *caps, int32_t pos,
                      const uint32_t *op, int32_t unicode) {
  Vm vm;
  memset(&vm, 0, sizeof vm);
  vm.s8 = wide ? NULL : (const uint8_t *)units;
  vm.s16 = wide ? (const uint16_t *)units : NULL;
  vm.length = length;
  vm.unicode = unicode;
  vm.caps = caps;

  return match_backref(&vm, op, &pos) ? pos : -1;
}

TzReMemo *tz_re_memo_start(int32_t rows, int32_t length) {
  TzReMemo *m = (TzReMemo *)tz_alloc(sizeof(TzReMemo));
  size_t pages = (size_t)length / TZ_RE_MEMO_PAGE + 1;
  m->pages = (uint8_t **)tz_alloc(pages * sizeof(uint8_t *));
  memset(m->pages, 0, pages * sizeof(uint8_t *));
  m->rows = rows;
  m->length = length;
  m->log = NULL;
  m->log_length = m->log_capacity = 0;

  return m;
}

TZ_NOINLINE uint8_t *tz_re_memo_page(TzReMemo *m, int32_t page) {
  size_t bytes = (size_t)m->rows * TZ_RE_MEMO_PAGE / 8;
  uint8_t *bits = (uint8_t *)tz_alloc(bytes);
  memset(bits, 0, bytes);
  m->pages[page] = bits;

  return bits;
}

static void memo_unset(TzReMemo *m, int32_t pos, int32_t row) {
  uint8_t *page = m->pages[pos / TZ_RE_MEMO_PAGE];
  if (!page) {
    return;
  }
  uint32_t bit = (uint32_t)row * TZ_RE_MEMO_PAGE + (uint32_t)(pos % TZ_RE_MEMO_PAGE);
  page[bit / 8] &= (uint8_t)~(1u << (bit % 8));
}

TZ_NOINLINE void tz_re_memo_log(TzReMemo *m, int32_t row, int32_t pos) {
  if (m->log_length == m->log_capacity) {
    int32_t capacity = m->log_capacity * 2 + 64;
    int64_t *log = (int64_t *)tz_alloc((size_t)capacity * sizeof(int64_t));
    if (m->log_length) {
      memcpy(log, m->log, (size_t)m->log_length * sizeof(int64_t));
    }
    tz_free(m->log);
    m->log = log;
    m->log_capacity = capacity;
  }
  m->log[m->log_length++] = (int64_t)pos << 32 | (uint32_t)row;
}

int32_t tz_re_memo_visit(TzReMemo *m, int32_t row, int32_t pos, int32_t log) {
  uint8_t *page = m->pages[pos / TZ_RE_MEMO_PAGE];
  if (!page) {
    page = tz_re_memo_page(m, pos / TZ_RE_MEMO_PAGE);
  }
  uint32_t bit = (uint32_t)row * TZ_RE_MEMO_PAGE + (uint32_t)(pos % TZ_RE_MEMO_PAGE);
  uint8_t mask = (uint8_t)(1u << (bit % 8));
  if (page[bit / 8] & mask) {
    return 1;
  }
  page[bit / 8] |= mask;
  if (log) {
    tz_re_memo_log(m, row, pos);
  }

  return 0;
}

void tz_re_memo_forget(TzReMemo *m, int32_t mark) {
  for (int32_t i = mark; i < m->log_length; ++i) {
    memo_unset(m, (int32_t)(m->log[i] >> 32), (int32_t)(uint32_t)m->log[i]);
  }
  if (m->log_length > mark) {
    m->log_length = mark;
  }
}

void tz_re_memo_clear(TzReMemo *m, int32_t pos) {
  for (int32_t row = 0; row < m->rows; ++row) {
    memo_unset(m, pos, row);
  }
}

void tz_re_memo_end(TzReMemo *m) {
  for (int32_t i = 0; i <= m->length / TZ_RE_MEMO_PAGE; ++i) {
    tz_free(m->pages[i]);
  }
  tz_free(m->pages);
  tz_free(m->log);
  tz_free(m);
}

int32_t tz_re_in_ranges(uint32_t c, const uint32_t *ranges, uint32_t n) {
  uint32_t lo = 0, hi = n;
  while (lo < hi) {
    uint32_t mid = (lo + hi) / 2;
    if (c < ranges[2 * mid]) {
      hi = mid;
    } else if (c > ranges[2 * mid + 1]) {
      lo = mid + 1;
    } else {
      return 1;
    }
  }

  return 0;
}

void tz_re_drop(void *object) {
  TzRegex *re = (TzRegex *)object;
  if (re->owned) {
    tz_re_free(re->owned);
  }
  tz_obj_free(re);
}

const TzLayout tz_re_layout = {tz_re_drop, NULL, 0, TZ_LAYOUT_REGEX, NULL, 0, 0};

TzRegex *tz_re_new(const uint32_t *program, TzReMatch match) {
  TzRegex *re = (TzRegex *)tz_obj_alloc(sizeof(TzRegex));
  re->rc = 1;
  re->layout = &tz_re_layout;
  re->last_index = 0;
  re->program = program;
  re->owned = NULL;
  re->match = match;

  return re;
}

static size_t utf8_of(const uint16_t *units, size_t n, char *out, size_t capacity) {
  size_t length = 0;
  for (size_t i = 0; i < n && length + 4 < capacity; ++i) {
    uint32_t c = units[i];
    if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n && units[i + 1] >= 0xDC00 &&
        units[i + 1] <= 0xDFFF) {
      c = 0x10000 + ((c - 0xD800) << 10) + (units[++i] - 0xDC00);
    }
    if (c < 0x80) {
      out[length++] = (char)c;
    } else if (c < 0x800) {
      out[length++] = (char)(0xC0 | (c >> 6));
      out[length++] = (char)(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
      out[length++] = (char)(0xE0 | (c >> 12));
      out[length++] = (char)(0x80 | ((c >> 6) & 0x3F));
      out[length++] = (char)(0x80 | (c & 0x3F));
    } else {
      out[length++] = (char)(0xF0 | (c >> 18));
      out[length++] = (char)(0x80 | ((c >> 12) & 0x3F));
      out[length++] = (char)(0x80 | ((c >> 6) & 0x3F));
      out[length++] = (char)(0x80 | (c & 0x3F));
    }
  }
  out[length] = 0;

  return length;
}

static TzRegex *from_string(void *p_obj, uint64_t p_meta, void *f_obj, uint64_t f_meta,
                            int32_t has_flags, TzMaker fail) {
  TzStr pattern = {p_obj, p_meta}, flag_text = {f_obj, f_meta};
  TzBuf p, f;
  tz_buf_init(&p);
  tz_buf_init(&f);
  tz_buf_str(&p, pattern);
  if (has_flags) {
    tz_buf_str(&f, flag_text);
  }
  uint32_t flags = 0;
  char pattern8[200], flags8[64];
  if (!tz_re_parse_flags(f.data, f.length, &flags)) {
    utf8_of(f.data, f.length, flags8, sizeof flags8);
    tz_buf_free(&p);
    tz_buf_free(&f);
    char message[160];
    snprintf(message, sizeof message, "Invalid flags supplied to RegExp constructor '%s'", flags8);
    if (!fail) {
      tz_panicf("internal error: %s", message);
    }
    tz_fail_utf8(fail, "ERR_INVALID_REGEX", message);
    return NULL;
  }
  const char *error = NULL;
  size_t at = 0;
  uint32_t *program = tz_re_compile(p.data, p.length, flags, &error, &at);
  if (!program) {
    utf8_of(p.data, p.length, pattern8, sizeof pattern8);
    utf8_of(f.data, f.length, flags8, sizeof flags8);
    tz_buf_free(&p);
    tz_buf_free(&f);
    char message[400];
    snprintf(message, sizeof message, "Invalid regular expression: /%s/%s: %s", pattern8, flags8,
             error);
    if (!fail) {
      tz_panicf("internal error: %s", message);
    }
    tz_fail_utf8(fail, "ERR_INVALID_REGEX", message);
    return NULL;
  }
  tz_buf_free(&p);
  tz_buf_free(&f);
  TzRegex *re = tz_re_new(program, tz_re_interpret);
  re->owned = program;

  return re;
}

TzRegex *tz_re_from_string(void *p_obj, uint64_t p_meta, void *f_obj, uint64_t f_meta,
                           int32_t has_flags) {
  return from_string(p_obj, p_meta, f_obj, f_meta, has_flags, NULL);
}

TzRegex *tz_re_parse(void *p_obj, uint64_t p_meta, void *f_obj, uint64_t f_meta, int32_t has_flags,
                     TzMaker fail) {
  return from_string(p_obj, p_meta, f_obj, f_meta, has_flags, fail);
}

static void buf_source(TzBuf *out, const uint32_t *program) {
  uint32_t n = tz_re_source_length(program);
  for (uint32_t i = 0; i < n; ++i) {
    tz_buf_push(out, tz_re_source_unit(program, i));
  }
}

static void buf_flags(TzBuf *out, const uint32_t *program) {
  static const char letters[] = "dgimsuvy";
  uint32_t flags = tz_re_flags(program);
  for (int i = 0; i < 8; ++i) {
    if (flags & (1u << i)) {
      tz_buf_push(out, (uint16_t)letters[i]);
    }
  }
}

void tz_buf_regex(TzBuf *out, const TzRegex *re) {
  tz_buf_push(out, '/');
  buf_source(out, re->program);
  tz_buf_push(out, '/');
  buf_flags(out, re->program);
}

void tz_re_source(TzStr *out, TzRegex *re) {
  TzBuf b;
  tz_buf_init(&b);
  buf_source(&b, re->program);
  *out = tz_buf_take(&b);
}

void tz_re_flags_text(TzStr *out, TzRegex *re) {
  TzBuf b;
  tz_buf_init(&b);
  buf_flags(&b, re->program);
  *out = tz_buf_take(&b);
}

void tz_re_to_string(TzStr *out, TzRegex *re) {
  TzBuf b;
  tz_buf_init(&b);
  tz_buf_regex(&b, re);
  *out = tz_buf_take(&b);
}

int32_t tz_re_flag(TzRegex *re, int64_t flag) {
  return (tz_re_flags(re->program) & (uint32_t)flag) != 0;
}

typedef struct {
  TzStr original;
  TzStr flat;
  const void *units;
  uint32_t length;
  int two;
} Text;

static void text_of(Text *t, void *obj, uint64_t meta) {
  t->original.obj = obj;
  t->original.meta = meta;
  t->flat = t->original;
  t->units = tz_str_units(&t->flat, &t->length, &t->two);
}

static inline uint32_t text_unit(const Text *t, uint32_t i) {
  return t->two ? ((const uint16_t *)t->units)[i] : ((const uint8_t *)t->units)[i];
}

typedef struct {
  TzStr *items;
  uint32_t length, capacity;
} Pieces;

static void pieces_add(Pieces *p, TzStr piece) {
  if (p->length == p->capacity) {
    uint32_t capacity = p->capacity * 2 + 16;
    TzStr *items = (TzStr *)tz_alloc((size_t)capacity * sizeof(TzStr));
    if (p->length) {
      memcpy(items, p->items, (size_t)p->length * sizeof(TzStr));
    }
    tz_free(p->items);
    p->items = items;
    p->capacity = capacity;
  }
  p->items[p->length++] = piece;
}

static void pieces_text(Pieces *p, const Text *t, uint32_t from, uint32_t to) {
  if (to > from) {
    pieces_add(p, tz_str_sub(t->original, from, to));
  }
}

static TzStr pieces_join(Pieces *p) {
  TzStr none = {NULL, 0}, out;
  TzStr empty = tz_str_sub(none, 0, 0);
  tz_str_join(&out, p->items, p->length, &empty);
  for (uint32_t i = 0; i < p->length; ++i) {
    tz_str_release_value(p->items[i]);
  }
  tz_free(p->items);
  p->items = NULL;
  p->length = p->capacity = 0;

  return out;
}

static uint32_t advance(const Text *t, uint32_t index, uint32_t flags) {
  if ((flags & (TZ_RE_UNICODE | TZ_RE_UNICODE_SETS)) && index + 1 < t->length &&
      is_lead(text_unit(t, index)) && is_trail(text_unit(t, index + 1))) {
    return index + 2;
  }

  return index + 1;
}

typedef struct {
  int32_t local[64];
  int32_t *caps;
} Captures;

static int32_t *captures_for(Captures *c, const uint32_t *program) {
  size_t n = 2 * (size_t)tz_re_captures(program);
  c->caps = n <= 64 ? c->local : (int32_t *)tz_alloc(n * sizeof(int32_t));

  return c->caps;
}

static void captures_free(Captures *c) {
  if (c->caps != c->local) {
    tz_free(c->caps);
  }
}

static int builtin_exec(TzRegex *re, const Text *t, int32_t *caps) {
  uint32_t flags = tz_re_flags(re->program);
  int stateful = (flags & (TZ_RE_GLOBAL | TZ_RE_STICKY)) != 0;
  int64_t last = stateful && re->last_index > 0 ? re->last_index : 0;
  if (last > (int64_t)t->length ||
      !re->match(re->program, t->units, t->two, (int32_t)t->length, (int32_t)last, caps,
                 (flags & TZ_RE_STICKY) != 0, 1)) {
    if (stateful) {
      re->last_index = 0;
    }
    return 0;
  }
  if (stateful) {
    re->last_index = caps[1];
  }

  return 1;
}

typedef struct {
  TzRegex *re;
  const Text *t;
  int32_t *caps;
  int32_t local[256];
  int32_t slots, size, found, next, start;
  int done;
} Batch;

static void batch_init(Batch *b, TzRegex *re, const Text *t) {
  b->re = re;
  b->t = t;
  b->slots = 2 * (int32_t)tz_re_captures(re->program);
  b->size = b->slots <= 256 ? 256 / b->slots : 1;
  b->caps = b->slots <= 256 ? b->local : (int32_t *)tz_alloc((size_t)b->slots * sizeof(int32_t));
  b->found = b->next = b->start = 0;
  b->done = 0;
  re->last_index = 0;
}

static const int32_t *batch_next(Batch *b) {
  if (b->next == b->found) {
    if (b->done || b->start > (int32_t)b->t->length) {
      return NULL;
    }
    uint32_t flags = tz_re_flags(b->re->program);
    b->found = b->re->match(b->re->program, b->t->units, b->t->two, (int32_t)b->t->length, b->start,
                            b->caps, (flags & TZ_RE_STICKY) != 0, b->size);
    b->next = 0;
    b->done = b->found < b->size;
    if (b->found == 0) {
      return NULL;
    }
    const int32_t *last = b->caps + (size_t)(b->found - 1) * b->slots;
    b->start = last[0] == last[1] ? (int32_t)advance(b->t, (uint32_t)last[1], flags) : last[1];
  }

  return b->caps + (size_t)(b->next++) * b->slots;
}

static void batch_free(Batch *b) {
  if (b->caps != b->local) {
    tz_free(b->caps);
  }
  b->re->last_index = 0;
}

static const TzType pair_object_type = {TZ_T_OBJECT, sizeof(void *), NULL};
TzType tz_re_indices_type = {TZ_T_OPTIONAL, sizeof(void *), &pair_object_type};
TzType tz_re_named_indices_type = {TZ_T_OPTIONAL, sizeof(void *), &pair_object_type};

static void *new_instance(const TzLayout *layout, uint32_t size) {
  char *object = (char *)tz_obj_alloc(size);
  memset(object, 0, size);
  ((TzInstance *)object)->rc = 1;
  ((TzInstance *)object)->layout = layout;

  return object;
}

static void *build_groups(const TzMatchShape *shape, TzStr input, const int32_t *caps) {
  char *groups = (char *)new_instance(shape->groups_layout, shape->groups_size);
  for (uint32_t i = 0; i < shape->name_count; ++i) {
    const TzMatchName *name = &shape->names[i];
    uint32_t k = name->capture;
    if (caps[2 * k] < 0) {
      continue;
    }
    *(TzStr *)(groups + name->offset) =
        tz_str_sub(input, (uint32_t)caps[2 * k], (uint32_t)caps[2 * k + 1]);
    if (name->optional) {
      groups[name->offset + sizeof(TzStr)] = 1;
    }
  }

  return groups;
}

static void *build_match(const TzMatchShape *shape, const uint32_t *program, TzStr input,
                         const int32_t *caps) {
  uint32_t n = tz_re_captures(program);
  char *match = (char *)new_instance(shape->layout, shape->size);
  TzArray *items = tz_arr_new(shape->item_type, n, n);
  size_t size = shape->item_type->size, has = shape->item_type->inner->size;
  for (uint32_t k = 0; k < n; ++k) {
    if (caps[2 * k] < 0) {
      continue;
    }
    char *item = items->data + k * size;
    *(TzStr *)item = tz_str_sub(input, (uint32_t)caps[2 * k], (uint32_t)caps[2 * k + 1]);
    item[has] = 1;
  }
  *(TzArray **)(match + shape->items) = items;
  *(int64_t *)(match + shape->index) = caps[0];
  tz_str_retain_value(input);
  *(TzStr *)(match + shape->input) = input;
  if (shape->groups_layout) {
    *(void **)(match + shape->groups) = build_groups(shape, input, caps);
  }
  if (shape->indices) {
    TzArray *indices =
        tz_arr_new(shape->groups_layout ? &tz_re_named_indices_type : &tz_re_indices_type, 0, n);
    for (uint32_t k = 0; k < n; ++k) {
      char *pair = NULL;
      if (caps[2 * k] >= 0) {
        pair = (char *)new_instance(shape->pair_layout, shape->pair_size);
        *(int64_t *)(pair + shape->pair_layout->fields[0].offset) = caps[2 * k];
        *(int64_t *)(pair + shape->pair_layout->fields[1].offset) = caps[2 * k + 1];
      }
      tz_arr_push_value(indices, &pair);
    }
    *(TzArray **)(match + shape->indices) = indices;
  }

  return match;
}

int32_t tz_re_test(TzRegex *re, void *s_obj, uint64_t s_meta) {
  Text t;
  text_of(&t, s_obj, s_meta);
  Captures c;
  int found = builtin_exec(re, &t, captures_for(&c, re->program));
  captures_free(&c);

  return found;
}

void *tz_re_exec(TzRegex *re, void *s_obj, uint64_t s_meta, const TzMatchShape *shape) {
  Text t;
  text_of(&t, s_obj, s_meta);
  Captures c;
  int32_t *caps = captures_for(&c, re->program);
  void *match =
      builtin_exec(re, &t, caps) ? build_match(shape, re->program, t.original, caps) : NULL;
  captures_free(&c);

  return match;
}

TzArray *tz_re_match_global(TzRegex *re, void *s_obj, uint64_t s_meta, const TzType *string_type) {
  Text t;
  text_of(&t, s_obj, s_meta);
  TzArray *texts = NULL;
  Batch batch;
  batch_init(&batch, re, &t);
  const int32_t *caps;
  while ((caps = batch_next(&batch)) != NULL) {
    if (!texts) {
      texts = tz_arr_new(string_type, 0, 0);
    }
    TzStr text = tz_str_sub(t.original, (uint32_t)caps[0], (uint32_t)caps[1]);
    if (texts->length == texts->capacity && texts->length >= 64 && caps[1] > 0) {
      uint64_t estimate = (uint64_t)texts->length * t.length / (uint32_t)caps[1];
      estimate += estimate / 8 + 64;
      if (estimate > (uint64_t)t.length + 1) {
        estimate = (uint64_t)t.length + 1;
      }
      if (estimate > texts->length) {
        tz_arr_grow(texts, (int64_t)estimate);
      }
    }
    if (texts->length < texts->capacity) {
      ((TzStr *)texts->data)[texts->length++] = text;
    } else {
      tz_arr_push_value(texts, &text);
    }
  }
  batch_free(&batch);

  return texts;
}

void tz_re_check_match_all(const TzRegex *re) { (void)re; }

int32_t tz_re_match_all_begin(TzRegex *re) { return (int32_t)re->last_index; }

void *tz_re_match_next(TzRegex *re, int32_t *last_index, void *s_obj, uint64_t s_meta,
                       const TzMatchShape *shape) {
  uint32_t flags = tz_re_flags(re->program);
  Text t;
  text_of(&t, s_obj, s_meta);
  Captures c;
  int32_t *caps = captures_for(&c, re->program);
  void *match = NULL;
  int64_t last = *last_index > 0 ? *last_index : 0;
  int32_t next = 0;
  if (last <= (int64_t)t.length && re->match(re->program, t.units, t.two, (int32_t)t.length,
                                             (int32_t)last, caps, (flags & TZ_RE_STICKY) != 0, 1)) {
    match = build_match(shape, re->program, t.original, caps);
    next = caps[1];
    if (caps[0] == caps[1]) {
      next = (int32_t)advance(&t, (uint32_t)next, flags);
    }
  }
  *last_index = next;
  captures_free(&c);

  return match;
}

int32_t tz_re_search(TzRegex *re, void *s_obj, uint64_t s_meta) {
  Text t;
  text_of(&t, s_obj, s_meta);
  Captures c;
  int32_t *caps = captures_for(&c, re->program);
  int32_t previous = re->last_index;
  re->last_index = 0;
  int32_t at = builtin_exec(re, &t, caps) ? caps[0] : -1;
  re->last_index = previous;
  captures_free(&c);

  return at;
}

static int has_names(const uint32_t *program) {
  uint32_t start;
  for (uint32_t k = 1; k < tz_re_captures(program); ++k) {
    if (tz_re_name(program, k, &start)) {
      return 1;
    }
  }

  return 0;
}

static uint32_t named_capture(const uint32_t *program, const Text *replacement, uint32_t from,
                              uint32_t to, const int32_t *caps) {
  for (uint32_t k = 1; k < tz_re_captures(program); ++k) {
    uint32_t start, length = tz_re_name(program, k, &start);
    if (length != to - from || caps[2 * k] < 0) {
      continue;
    }
    uint32_t i = 0;
    while (i < length && tz_re_name_unit(program, start, i) == text_unit(replacement, from + i)) {
      ++i;
    }
    if (i == length) {
      return k;
    }
  }

  return 0;
}

static void substitute(Pieces *out, const Text *rep, const Text *s, const uint32_t *program,
                       const int32_t *caps) {
  uint32_t n = tz_re_captures(program), length = rep->length, plain = 0;
  for (uint32_t i = 0; i + 1 < length; ++i) {
    if (text_unit(rep, i) != '$') {
      continue;
    }
    uint32_t c = text_unit(rep, i + 1), used = 1;
    int32_t from = -1, to = -1;
    if (c == '$') {
      pieces_text(out, rep, plain, i + 1);
      plain = i + 2;
      ++i;
      continue;
    } else if (c == '&') {
      from = caps[0];
      to = caps[1];
    } else if (c == '`') {
      from = 0;
      to = caps[0];
    } else if (c == '\'') {
      from = caps[1];
      to = (int32_t)s->length;
    } else if (c >= '0' && c <= '9') {
      uint32_t one = c - '0';
      uint32_t next = i + 2 < length ? text_unit(rep, i + 2) : 0;
      uint32_t two = next >= '0' && next <= '9' ? one * 10 + (next - '0') : 0;
      uint32_t k;
      if (two >= 1 && two < n) {
        k = two;
        used = 2;
      } else if (one >= 1 && one < n) {
        k = one;
      } else {
        continue;
      }
      from = caps[2 * k];
      to = caps[2 * k + 1];
    } else if (c == '<') {
      if (!has_names(program)) {
        continue;
      }
      uint32_t close = i + 2;
      while (close < length && text_unit(rep, close) != '>') {
        ++close;
      }
      if (close >= length) {
        continue;
      }
      uint32_t k = named_capture(program, rep, i + 2, close, caps);
      if (k) {
        from = caps[2 * k];
        to = caps[2 * k + 1];
      }
      used = close - i;
    } else {
      continue;
    }
    pieces_text(out, rep, plain, i);
    if (from >= 0) {
      pieces_text(out, s, (uint32_t)from, (uint32_t)to);
    }
    plain = i + 1 + used;
    i += used;
  }
  pieces_text(out, rep, plain, length);
}

void tz_re_replace(TzStr *out, TzRegex *re, void *s_obj, uint64_t s_meta, void *r_obj,
                   uint64_t r_meta, int32_t all) {
  uint32_t flags = tz_re_flags(re->program);
  int global = all || (flags & TZ_RE_GLOBAL) != 0;
  Text t, rep;
  text_of(&t, s_obj, s_meta);
  text_of(&rep, r_obj, r_meta);
  Pieces b = {NULL, 0, 0};
  uint32_t copied = 0;
  int any = 0;
  if (global) {
    Batch batch;
    batch_init(&batch, re, &t);
    const int32_t *caps;
    while ((caps = batch_next(&batch)) != NULL) {
      any = 1;
      if ((uint32_t)caps[0] >= copied) {
        pieces_text(&b, &t, copied, (uint32_t)caps[0]);
        substitute(&b, &rep, &t, re->program, caps);
        copied = (uint32_t)caps[1];
      }
    }
    batch_free(&batch);
  } else {
    Captures c;
    int32_t *caps = captures_for(&c, re->program);
    if (builtin_exec(re, &t, caps)) {
      any = 1;
      pieces_text(&b, &t, copied, (uint32_t)caps[0]);
      substitute(&b, &rep, &t, re->program, caps);
      copied = (uint32_t)caps[1];
    }
    captures_free(&c);
  }
  if (!any) {
    tz_str_retain_value(t.original);
    *out = t.original;
    return;
  }
  pieces_text(&b, &t, copied, t.length);
  *out = pieces_join(&b);
}

static void push_piece(TzArray *a, TzStr s, int32_t from, int32_t to, int optional) {
  char value[32];
  memset(value, 0, sizeof value);
  if (from >= 0) {
    *(TzStr *)value = tz_str_sub(s, (uint32_t)from, (uint32_t)to);
    if (optional) {
      value[sizeof(TzStr)] = 1;
    }
  }
  tz_arr_push_value(a, value);
}

TzArray *tz_re_split(TzRegex *re, void *s_obj, uint64_t s_meta, int64_t limit, int32_t has_limit,
                     const TzType *item_type) {
  uint32_t max = has_limit ? (uint32_t)limit : UINT32_MAX;
  int optional = item_type->kind == TZ_T_OPTIONAL;
  TzArray *a = tz_arr_new(item_type, 0, 0);
  if (max == 0) {
    return a;
  }
  Text t;
  text_of(&t, s_obj, s_meta);
  const uint32_t *program = re->program;
  uint32_t flags = tz_re_flags(program), n = tz_re_captures(program);
  Captures c;
  int32_t *caps = captures_for(&c, program);
  if (t.length == 0) {
    if (!re->match(program, t.units, t.two, 0, 0, caps, 1, 1)) {
      push_piece(a, t.original, 0, 0, optional);
    }
    captures_free(&c);
    return a;
  }
  uint32_t p = 0, q = 0;
  while (q < t.length) {
    if (!re->match(program, t.units, t.two, (int32_t)t.length, (int32_t)q, caps, 0, 1)) {
      break;
    }
    q = (uint32_t)caps[0];
    if (q >= t.length) {
      break;
    }
    uint32_t e = (uint32_t)caps[1];
    if (e == p) {
      q = advance(&t, q, flags);
      continue;
    }
    push_piece(a, t.original, (int32_t)p, (int32_t)q, optional);
    if (a->length == max) {
      goto done;
    }
    p = e;
    for (uint32_t k = 1; k < n; ++k) {
      push_piece(a, t.original, caps[2 * k], caps[2 * k + 1], optional);
      if (a->length == max) {
        goto done;
      }
    }
    q = p;
  }
  push_piece(a, t.original, (int32_t)p, (int32_t)t.length, optional);
done:
  captures_free(&c);

  return a;
}

_Static_assert(sizeof(TzReplacer) <= TZ_REPLACER_SIZE, "TzReplacer does not fit its room");

void tz_re_replacer_init(TzReplacer *r, TzRegex *re, void *s_obj, uint64_t s_meta, int32_t all) {
  uint32_t flags = tz_re_flags(re->program);
  int global = all || (flags & TZ_RE_GLOBAL) != 0;
  Text t;
  text_of(&t, s_obj, s_meta);
  r->re = re;
  r->input = t.original;
  r->captures = tz_re_captures(re->program);
  r->count = r->capacity = r->next = r->copied = 0;
  r->matches = NULL;
  r->pieces = NULL;
  r->piece_count = r->piece_capacity = 0;
  size_t words = 2 * (size_t)r->captures;
  Captures c;
  Batch batch;
  int32_t *one = NULL;
  if (global) {
    batch_init(&batch, re, &t);
  } else {
    one = captures_for(&c, re->program);
  }
  for (;;) {
    const int32_t *caps = global ? batch_next(&batch) : builtin_exec(re, &t, one) ? one : NULL;
    if (!caps) {
      break;
    }
    if (r->count == r->capacity) {
      uint32_t capacity = r->capacity * 2 + 4;
      int32_t *matches = (int32_t *)tz_alloc((size_t)capacity * words * sizeof(int32_t));
      if (r->count) {
        memcpy(matches, r->matches, (size_t)r->count * words * sizeof(int32_t));
      }
      tz_free(r->matches);
      r->matches = matches;
      r->capacity = capacity;
    }
    memcpy(r->matches + (size_t)r->count * words, caps, words * sizeof(int32_t));
    ++r->count;
    if (!global) {
      break;
    }
  }
  if (global) {
    batch_free(&batch);
  } else {
    captures_free(&c);
  }
}

const int32_t *tz_re_replacer_next(TzReplacer *r) {
  if (r->next == r->count) {
    return NULL;
  }

  return r->matches + (size_t)(r->next++) * 2 * r->captures;
}

int32_t tz_re_replacer_capture(TzStr *out, TzReplacer *r, const int32_t *caps, int32_t k) {
  if (caps[2 * k] < 0) {
    out->obj = NULL;
    out->meta = 0;
    return 0;
  }
  *out = tz_str_sub(r->input, (uint32_t)caps[2 * k], (uint32_t)caps[2 * k + 1]);

  return 1;
}

void *tz_re_replacer_groups(TzReplacer *r, const TzMatchShape *shape, const int32_t *caps) {
  return build_groups(shape, r->input, caps);
}

void tz_re_replacer_put(TzReplacer *r, const int32_t *caps, void *obj, uint64_t meta) {
  if ((uint32_t)caps[0] < r->copied) {
    return;
  }
  Text t, given;
  text_of(&t, r->input.obj, r->input.meta);
  text_of(&given, obj, meta);
  Pieces out = {r->pieces, r->piece_count, r->piece_capacity};
  pieces_text(&out, &t, r->copied, (uint32_t)caps[0]);
  if (given.length) {
    tz_str_retain_value(given.original);
    pieces_add(&out, given.original);
  }
  r->pieces = out.items;
  r->piece_count = out.length;
  r->piece_capacity = out.capacity;
  r->copied = (uint32_t)caps[1];
}

void tz_re_replacer_drop(TzReplacer *r) {
  tz_free(r->matches);
  r->matches = NULL;
  for (uint32_t i = 0; i < r->piece_count; ++i) {
    tz_str_release_value(r->pieces[i]);
  }
  tz_free(r->pieces);
  r->pieces = NULL;
  r->piece_count = r->piece_capacity = 0;
}

void tz_re_replacer_finish(TzStr *out, TzReplacer *r) {
  tz_free(r->matches);
  if (r->count == 0) {
    tz_free(r->pieces);
    tz_str_retain_value(r->input);
    *out = r->input;
    return;
  }
  Text t;
  text_of(&t, r->input.obj, r->input.meta);
  Pieces pieces = {r->pieces, r->piece_count, r->piece_capacity};
  pieces_text(&pieces, &t, r->copied, t.length);
  *out = pieces_join(&pieces);
}

static void buf_hex(TzBuf *out, uint32_t value, int digits) {
  static const char hex[] = "0123456789abcdef";
  for (int i = digits - 1; i >= 0; --i) {
    tz_buf_push(out, (uint16_t)hex[(value >> (4 * i)) & 15]);
  }
}

void tz_re_escape(TzStr *out, void *obj, uint64_t meta) {
  Text t;
  text_of(&t, obj, meta);
  TzBuf b;
  tz_buf_init(&b);
  for (uint32_t i = 0; i < t.length; ++i) {
    uint32_t c = text_unit(&t, i);
    int pair = is_lead(c) && i + 1 < t.length && is_trail(text_unit(&t, i + 1));
    if (b.length == 0 && ((c >= '0' && c <= '9') || ((c | 0x20) >= 'a' && (c | 0x20) <= 'z'))) {
      tz_buf_ascii(&b, "\\x");
      buf_hex(&b, c, 2);
      continue;
    }
    if (c < 128 && c && strchr("^$\\.*+?()[]{}|/", (int)c)) {
      tz_buf_push(&b, '\\');
      tz_buf_push(&b, (uint16_t)c);
      continue;
    }
    const char *control = c == '\t'   ? "\\t"
                          : c == '\n' ? "\\n"
                          : c == '\v' ? "\\v"
                          : c == '\f' ? "\\f"
                          : c == '\r' ? "\\r"
                                      : NULL;
    if (control) {
      tz_buf_ascii(&b, control);
      continue;
    }
    int other = c < 128 && c && strchr(",-=<>#&!%:;@~'`\"", (int)c);
    int space = c == ' ' || c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) ||
                c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000 ||
                c == 0xFEFF;
    int lone = (is_lead(c) && !pair) || is_trail(c);
    if (other || space || lone) {
      tz_buf_ascii(&b, c <= 0xFF ? "\\x" : "\\u");
      buf_hex(&b, c, c <= 0xFF ? 2 : 4);
      continue;
    }
    tz_buf_push(&b, (uint16_t)c);
    if (pair) {
      tz_buf_push(&b, (uint16_t)text_unit(&t, ++i));
    }
  }
  *out = tz_buf_take(&b);
}
