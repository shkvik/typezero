#pragma once

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
  TYPED_ARRAY,
  NOT_ON_TYPED_ARRAY,
};

struct TypedArrayCallbackMethod {
  static constexpr BytesMethodKind kind = BytesMethodKind::TYPED_ARRAY_CALLBACK;
};

struct TypedArrayMethod {
  static constexpr BytesMethodKind kind = BytesMethodKind::TYPED_ARRAY;
};

struct NotOnTypedArrayMethod {
  static constexpr BytesMethodKind kind = BytesMethodKind::NOT_ON_TYPED_ARRAY;
};

using BytesMethodForm = Union<TypedArrayCallbackMethod, TypedArrayMethod, NotOnTypedArrayMethod>;

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
