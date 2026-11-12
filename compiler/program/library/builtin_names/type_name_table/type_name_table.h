#pragma once

#include "compiler/program/types/type_op/type_op.h"
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
  CAPITALIZE,
  CONSTRUCTOR,
  CONSTRUCTOR_ARGS,
  DATE,
  ERR,
  EXCLUDE,
  EXTRACT,
  GENERATOR,
  IMMEDIATE,
  INSTANCE_TYPE,
  ITERABLE,
  ITERABLE_ITERATOR,
  ITERATOR,
  ITERATOR_RESULT,
  JSON,
  LONG,
  LOWERCASE,
  MAP,
  METHOD_DESCRIPTOR,
  NEVER,
  NON_NULLABLE,
  NULL_TYPE,
  NUMBER,
  OK,
  PARAMETERS,
  PROMISE,
  PROMISE_LIKE,
  READONLY_ARRAY,
  READONLY_MAP,
  READONLY_SET,
  RECORD,
  REG_EXP,
  REG_EXP_MATCH_ARRAY,
  RESULT,
  RETURN_TYPE,
  SET,
  SHARED_ARRAY_BUFFER,
  STRING,
  TEMPLATE_STRINGS_ARRAY,
  TIMEOUT,
  UNCAPITALIZE,
  UNDEFINED,
  UNKNOWN,
  UPPERCASE,
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
  UTILITY,
};

enum class Collection {
  MAP,
  SET,
  RECORD,
};

enum class UtilityOperand {
  ANY_TYPE,
  FUNCTION_TYPE,
  TEXT_TYPE,
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

struct UtilityTypeName {
  static constexpr TypeNameKind kind = TypeNameKind::UTILITY;
  TypeOp op;
  int argumentCount;
  UtilityOperand operand;
};

using TypeNameForm =
    Union<PlainTypeName, ArrayTypeName, IteratorTypeName, CollectionTypeName,
          WeakCollectionTypeName, PromiseTypeName, ResultTypeName, UtilityTypeName>;

struct TypeNameInfo {
  llvm::StringLiteral name;
  TypeName type;
  TypeNameForm form;
};

class TypeNameTable {
public:
  static const TypeNameInfo *find(llvm::StringRef name);
  static const UtilityTypeName *findUtility(llvm::StringRef name);
  static llvm::StringRef getName(TypeName type);
  static bool isName(llvm::StringRef name, TypeName type);
};

}
