#pragma once

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/SMLoc.h"
#include <cstdint>
#include <deque>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tz {

enum class UnaryOp {
  Neg,
  Plus,
  Not,
  BitNot,
  TypeOf,
  Void,    // `void x`: evaluates x; the value is undefined
  Delete,  // `delete x`: Sema rejects it, since objects and arrays always have their fields
  NonNull, // postfix `x!`
};

enum class BinaryOp {
  Add,
  Sub,
  Mul,
  Div,
  Rem,
  Pow,
  BitAnd,
  BitOr,
  BitXor,
  Shl,
  Shr,
  UShr,
  Lt,
  Le,
  Gt,
  Ge,
  Eq,
  Ne,
  LogAnd,
  LogOr,
  Nullish,    // `a ?? b`
  In,         // `key in object`: Sema rejects it until unions come
  InstanceOf, // `x instanceof C`: compares the class number in x's descriptor
};

const char *opSpelling(UnaryOp op);

const char *opSpelling(BinaryOp op);

} // namespace tz
