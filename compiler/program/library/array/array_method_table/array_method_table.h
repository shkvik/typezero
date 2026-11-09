#pragma once

#include "compiler/program/ast/builtin/builtin.h"
#include "compiler/union/union/union.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class ArrayMethod {
  PUSH,
  UNSHIFT,
  POP,
  SHIFT,
  SPLICE,
  FILL,
  SORT,
  REVERSE,
  AT,
  SLICE,
  CONCAT,
  INDEX_OF,
  LAST_INDEX_OF,
  INCLUDES,
  JOIN,
  TO_STRING,
  KEYS,
  VALUES,
  ENTRIES,
  FOR_EACH,
  MAP,
  FLAT_MAP,
  FILTER,
  REDUCE,
  REDUCE_RIGHT,
  FIND,
  FIND_LAST,
  FIND_INDEX,
  FIND_LAST_INDEX,
  SOME,
  EVERY,
};

enum class ArrayCallback {
  FOR_EACH,
  MAP,
  FLAT_MAP,
  FILTER,
  REDUCE,
  FIND,
  FIND_INDEX,
  TEST,
  COMPARE,
};

enum class ArrayMethodKind {
  READING,
  RESIZING,
  REWRITING,
  SORTING,
  CALLBACK,
};

enum class LengthChange {
  GROWS,
  SHRINKS,
  GROWS_OR_SHRINKS,
};

struct ReadingArrayMethod {
  static constexpr ArrayMethodKind kind = ArrayMethodKind::READING;
};

struct ResizingArrayMethod {
  static constexpr ArrayMethodKind kind = ArrayMethodKind::RESIZING;
  LengthChange change;
};

struct RewritingArrayMethod {
  static constexpr ArrayMethodKind kind = ArrayMethodKind::REWRITING;
  bool releasesElements;
};

struct SortingArrayMethod {
  static constexpr ArrayMethodKind kind = ArrayMethodKind::SORTING;
};

struct CallbackArrayMethod {
  static constexpr ArrayMethodKind kind = ArrayMethodKind::CALLBACK;
  ArrayCallback callback;
};

using ArrayMethodForm = Union<ReadingArrayMethod, ResizingArrayMethod, RewritingArrayMethod,
                              SortingArrayMethod, CallbackArrayMethod>;

struct ArrayMethodInfo {
  llvm::StringLiteral name;
  ArrayMethod method;
  Builtin builtin;
  ArrayMethodForm form;
};

class ArrayMethodTable {
public:
  static const ArrayMethodInfo *find(llvm::StringRef name);
  static std::optional<ArrayCallback> findCallback(llvm::StringRef name);
  static llvm::ArrayRef<ArrayMethodInfo> getMethods();
};

}
