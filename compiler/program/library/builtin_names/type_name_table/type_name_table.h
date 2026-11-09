#pragma once

#include "compiler/union/union/union.h"
#include "llvm/ADT/StringRef.h"

namespace tz {

enum class TypeName {
  ARRAY,
  ARRAY_BUFFER,
  ARRAY_BUFFER_LIKE,
  ASYNC_GENERATOR,
  ASYNC_ITERABLE,
  ASYNC_ITERABLE_ITERATOR,
  ASYNC_ITERATOR,
  BIGINT,
  BIG_INT64_ARRAY,
  BIG_UINT64_ARRAY,
  BOOLEAN,
  CONSTRUCTOR,
  CONSTRUCTOR_ARGS,
  DATE,
  ERR,
  GENERATOR,
  IMMEDIATE,
  INSTANCE_TYPE,
  ITERABLE,
  ITERABLE_ITERATOR,
  ITERATOR,
  ITERATOR_RESULT,
  JSON,
  LONG,
  MAP,
  METHOD_DESCRIPTOR,
  NEVER,
  NULL_TYPE,
  NUMBER,
  OK,
  PROMISE,
  PROMISE_LIKE,
  READONLY_ARRAY,
  READONLY_MAP,
  READONLY_SET,
  RECORD,
  REG_EXP,
  REG_EXP_MATCH_ARRAY,
  RESULT,
  SET,
  SHARED_ARRAY_BUFFER,
  STRING,
  TEMPLATE_STRINGS_ARRAY,
  TIMEOUT,
  UNDEFINED,
  UNKNOWN,
  VOID_TYPE,
  WEAK_MAP,
  WEAK_REF,
  WEAK_SET,
};

enum class TypeNameKind {
  PLAIN,
  ARRAY,
  ITERATOR,
  COLLECTION,
  WEAK_COLLECTION,
  PROMISE,
  RESULT,
};

enum class Collection {
  MAP,
  SET,
  RECORD,
};

struct PlainTypeName {
  static constexpr TypeNameKind kind = TypeNameKind::PLAIN;
};

struct ArrayTypeName {
  static constexpr TypeNameKind kind = TypeNameKind::ARRAY;
};

struct IteratorTypeName {
  static constexpr TypeNameKind kind = TypeNameKind::ITERATOR;
  bool async;
};

struct CollectionTypeName {
  static constexpr TypeNameKind kind = TypeNameKind::COLLECTION;
  Collection collection;
};

struct WeakCollectionTypeName {
  static constexpr TypeNameKind kind = TypeNameKind::WEAK_COLLECTION;
};

struct PromiseTypeName {
  static constexpr TypeNameKind kind = TypeNameKind::PROMISE;
};

struct ResultTypeName {
  static constexpr TypeNameKind kind = TypeNameKind::RESULT;
};

using TypeNameForm = Union<PlainTypeName, ArrayTypeName, IteratorTypeName, CollectionTypeName,
                           WeakCollectionTypeName, PromiseTypeName, ResultTypeName>;

struct TypeNameInfo {
  llvm::StringLiteral name;
  TypeName type;
  TypeNameForm form;
};

class TypeNameTable {
public:
  static const TypeNameInfo *find(llvm::StringRef name);
  static bool isName(llvm::StringRef name, TypeName type);
};

}
