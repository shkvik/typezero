#pragma once

#include "compiler/program/ast/builtin/builtin.h"
#include "compiler/union/union/union.h"
#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class BytesMethod {
  AT,
  COPY_WITHIN,
  ENTRIES,
  EVERY,
  FILL,
  FILTER,
  FIND,
  FIND_INDEX,
  FIND_LAST,
  FIND_LAST_INDEX,
  FOR_EACH,
  INCLUDES,
  INDEX_OF,
  JOIN,
  KEYS,
  LAST_INDEX_OF,
  MAP,
  REDUCE,
  REDUCE_RIGHT,
  REVERSE,
  SET,
  SLICE,
  SOME,
  SORT,
  SUBARRAY,
  TO_REVERSED,
  TO_SORTED,
  TO_STRING,
  VALUES,
  WITH,
  FLAT_MAP,
  TO_LOCALE_STRING,
  TO_JSON,
  EQUALS,
  COMPARE,
  COPY,
  WRITE,
  SWAP16,
  SWAP32,
  SWAP64,
  RESIZE,
  TRANSFER,
  TRANSFER_TO_FIXED_LENGTH,
  ENCODE,
  ENCODE_INTO,
  DECODE,
  IS_VIEW,
  OF,
  FROM,
  PARSE,
  ALLOC,
  ALLOC_UNSAFE,
  ALLOC_UNSAFE_SLOW,
  CONCAT,
  BYTE_LENGTH,
  IS_BUFFER,
  IS_ENCODING,
};

enum class BytesMethodKind {
  TYPED_ARRAY_CALLBACK,
  TYPED_ARRAY_SORT,
  TYPED_ARRAY_REVERSE,
  TYPED_ARRAY_AT,
  TYPED_ARRAY_SEARCH,
  TYPED_ARRAY_JOIN,
  TYPED_ARRAY_FILL,
  TYPED_ARRAY_SET,
  TYPED_ARRAY_POSITIONS,
  TYPED_ARRAY_WITH,
  TYPED_ARRAY_ITERATOR,
  NOT_ON_TYPED_ARRAY,
};

enum class ReorderPlace {
  IN_PLACE,
  NEW_COPY,
};

enum class BytesSearchMode {
  INDEX_OF,
  LAST_INDEX_OF,
  INCLUDES,
};

struct TypedArrayCallbackMethod {
  static constexpr BytesMethodKind kind = BytesMethodKind::TYPED_ARRAY_CALLBACK;
};

struct TypedArraySortMethod {
  static constexpr BytesMethodKind kind = BytesMethodKind::TYPED_ARRAY_SORT;
  Builtin builtin;
  ReorderPlace place;
};

struct TypedArrayReverseMethod {
  static constexpr BytesMethodKind kind = BytesMethodKind::TYPED_ARRAY_REVERSE;
  Builtin builtin;
  ReorderPlace place;
};

struct TypedArrayAtMethod {
  static constexpr BytesMethodKind kind = BytesMethodKind::TYPED_ARRAY_AT;
};

struct TypedArraySearchMethod {
  static constexpr BytesMethodKind kind = BytesMethodKind::TYPED_ARRAY_SEARCH;
  BytesSearchMode mode;
};

struct TypedArrayJoinMethod {
  static constexpr BytesMethodKind kind = BytesMethodKind::TYPED_ARRAY_JOIN;
  int maxArgs;
};

struct TypedArrayFillMethod {
  static constexpr BytesMethodKind kind = BytesMethodKind::TYPED_ARRAY_FILL;
};

struct TypedArraySetMethod {
  static constexpr BytesMethodKind kind = BytesMethodKind::TYPED_ARRAY_SET;
};

struct TypedArrayPositionsMethod {
  static constexpr BytesMethodKind kind = BytesMethodKind::TYPED_ARRAY_POSITIONS;
  Builtin builtin;
  int minArgs;
  int maxArgs;
};

struct TypedArrayWithMethod {
  static constexpr BytesMethodKind kind = BytesMethodKind::TYPED_ARRAY_WITH;
};

struct TypedArrayIteratorMethod {
  static constexpr BytesMethodKind kind = BytesMethodKind::TYPED_ARRAY_ITERATOR;
};

struct NotOnTypedArrayMethod {
  static constexpr BytesMethodKind kind = BytesMethodKind::NOT_ON_TYPED_ARRAY;
};

using BytesMethodForm =
    Union<TypedArrayCallbackMethod, TypedArraySortMethod, TypedArrayReverseMethod,
          TypedArrayAtMethod, TypedArraySearchMethod, TypedArrayJoinMethod, TypedArrayFillMethod,
          TypedArraySetMethod, TypedArrayPositionsMethod, TypedArrayWithMethod,
          TypedArrayIteratorMethod, NotOnTypedArrayMethod>;

struct BytesMethodInfo {
  llvm::StringLiteral name;
  BytesMethod method;
  BytesMethodForm form;
};

class BytesMethodTable {
public:
  static const BytesMethodInfo *find(llvm::StringRef name);
  static std::optional<BytesMethod> findMethod(llvm::StringRef name);
  static llvm::StringRef getName(BytesMethod method);
};

}
