#pragma once

#include "compiler/union/union/union.h"
#include <cstddef>
#include <string>
#include <vector>

namespace tz {

class Type;

enum class PortableKind {
  SAME_TYPE,
  CLASS_TYPE,
  CLASS_REF_TYPE,
  OPTIONAL_TYPE,
  ARRAY_TYPE,
  TUPLE_TYPE,
  FUNCTION_TYPE,
  UNION_TYPE,
  OBJECT_TYPE,
  PROMISE_TYPE,
  DICT_TYPE,
  ITERATOR_TYPE,
  RESULT_TYPE,
  BAD_TYPE,
};

struct PortableSame {
  static constexpr PortableKind kind = PortableKind::SAME_TYPE;
  const Type *type = nullptr;
};

struct PortableClass {
  static constexpr PortableKind kind = PortableKind::CLASS_TYPE;
  std::string file;
  std::string name;
};

struct PortableClassRef {
  static constexpr PortableKind kind = PortableKind::CLASS_REF_TYPE;
  std::string file;
  std::string name;
};

struct PortableOptional {
  static constexpr PortableKind kind = PortableKind::OPTIONAL_TYPE;
};

struct PortableArray {
  static constexpr PortableKind kind = PortableKind::ARRAY_TYPE;
};

struct PortableTuple {
  static constexpr PortableKind kind = PortableKind::TUPLE_TYPE;
  size_t min = 0;
  bool rest = false;
};

struct PortableFunction {
  static constexpr PortableKind kind = PortableKind::FUNCTION_TYPE;
  size_t min = 0;
  bool rest = false;
};

struct PortableUnion {
  static constexpr PortableKind kind = PortableKind::UNION_TYPE;
};

struct PortableObject {
  static constexpr PortableKind kind = PortableKind::OBJECT_TYPE;
  std::vector<std::string> names;
  std::vector<bool> readonly;
};

struct PortablePromise {
  static constexpr PortableKind kind = PortableKind::PROMISE_TYPE;
};

struct PortableDict {
  static constexpr PortableKind kind = PortableKind::DICT_TYPE;
  int dict = 0;
};

struct PortableIterator {
  static constexpr PortableKind kind = PortableKind::ITERATOR_TYPE;
};

struct PortableResult {
  static constexpr PortableKind kind = PortableKind::RESULT_TYPE;
};

struct PortableBad {
  static constexpr PortableKind kind = PortableKind::BAD_TYPE;
};

using PortableForm =
    Union<PortableSame, PortableClass, PortableClassRef, PortableOptional, PortableArray,
          PortableTuple, PortableFunction, PortableUnion, PortableObject, PortablePromise,
          PortableDict, PortableIterator, PortableResult, PortableBad>;

}
