#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/objects/keyed_field_checker/keyed_field_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class FieldFacts;
class ExprChecker;
class LiteralTypes;
class ObjectChecker;
class TypeRelations;
class ConstantFolder;

class KeyedFieldChecker {
public:
  using ResultKeyedFieldRead = Result<Expr *, KeyedFieldCheckerCheckKeyedFieldError>;

  KeyedFieldChecker(SemanticState &owner, SemanticParts &all);

  std::optional<std::string> constantKey(const Expr *key) const;
  void keyNames(const Expr *key, llvm::function_ref<void(llvm::StringRef)> each) const;
  ResultKeyedFieldRead checkKeyedField(IndexExpr *index, bool write);
  static const Type *keyedFieldType(const Type *type, const Type *keyType);
  static bool wrongKey(KeyedFieldCheckerCheckKeyedFieldErrorCode code);

private:
  using ResultKeyedFieldName = Result<std::string, KeyedFieldCheckerCheckKeyedFieldError>;

  static bool isInherited(llvm::StringRef name);
  ResultKeyedFieldName tupleKeyName(IndexExpr *index, const Type *type);
  static ResultKeyedFieldName objectKeyName(const Type *type, const Type *keyType,
                                            const std::optional<std::string> &key);
  ResultKeyedFieldRead checkKeySetField(IndexExpr *index, const Type *type, const Type *keyType,
                                        const Type *field, bool write);
  ResultKeyedFieldRead checkKeySetWrite(IndexExpr *index, const Type *type, const Type *keyType,
                                        const Type *field);
  bool allAssigned(llvm::ArrayRef<const Type *> names) const;
  static bool anyReadonly(const Type *type, llvm::ArrayRef<const Type *> names);
  ResultKeyedFieldRead checkNamedField(IndexExpr *index, const Type *type, const std::string &name,
                                       bool write);

  SemanticState &state;
  FieldFacts *fieldFacts = nullptr;
  ExprChecker *exprChecker = nullptr;
  LiteralTypes *literalTypes = nullptr;
  ObjectChecker *objectChecker = nullptr;
  TypeRelations *typeRelations = nullptr;
  ConstantFolder *constantFolder = nullptr;
};

}
