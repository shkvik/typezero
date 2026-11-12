#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/objects/keyed_field_checker/keyed_field_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class ClassBases;
class SemanticParts;
class FieldFacts;
class ExprChecker;
class LiteralTypes;
class ObjectChecker;
class TypeRelations;
class ClassDeclarer;
class ConstantFolder;

enum class KeyedAccess {
  READ,
  WRITE,
};

class KeyedFieldChecker {
public:
  using ResultKeyedFieldRead = Result<Expr *, KeyedFieldCheckerCheckKeyedFieldError>;

  KeyedFieldChecker(SemanticState &owner, SemanticParts &all);

  std::optional<std::string> constantKey(const Expr *key) const;
  std::vector<std::string> keyNames(const Expr *key) const;
  ResultKeyedFieldRead checkKeyedField(IndexExpr *index, KeyedAccess access);
  const Type *keyedFieldType(const Type *type, const Type *keyType) const;
  bool isWrongKey(KeyedFieldCheckerCheckKeyedFieldErrorCode code) const;

private:
  using ResultKeyedFieldName = Result<std::string, KeyedFieldCheckerCheckKeyedFieldError>;

  bool isInherited(llvm::StringRef name) const;
  std::vector<std::string> memberConstants(const MemberExpr *member) const;
  bool isPublicField(const ClassDecl *classDecl, const std::string &name) const;
  ResultKeyedFieldRead checkTupleField(IndexExpr *index, const Type *type, KeyedAccess access);
  ResultKeyedFieldName checkTupleKeyName(IndexExpr *index, const Type *type);
  ResultKeyedFieldName checkObjectKeyName(const Type *type, const Type *keyType,
                                          const std::optional<std::string> &key) const;
  ResultKeyedFieldRead checkKeySetField(IndexExpr *index, const Type *type, const Type *keyType,
                                        const Type *field, KeyedAccess access);
  Expr *keySetIndex(IndexExpr *index, const Type *field) const;
  ResultKeyedFieldRead checkKeySetWrite(IndexExpr *index, const Type *type, const Type *keyType,
                                        const Type *field);
  bool isAllAssigned(llvm::ArrayRef<const Type *> names) const;
  bool hasReadonly(const Type *type, llvm::ArrayRef<const Type *> names) const;
  ResultKeyedFieldRead checkNamedField(IndexExpr *index, const Type *type, const std::string &name,
                                       KeyedAccess access);

  SemanticState &state;
  FieldFacts *fieldFacts = nullptr;
  ClassBases *classBases = nullptr;
  ExprChecker *exprChecker = nullptr;
  LiteralTypes *literalTypes = nullptr;
  ObjectChecker *objectChecker = nullptr;
  TypeRelations *typeRelations = nullptr;
  ClassDeclarer *classDeclarer = nullptr;
  ConstantFolder *constantFolder = nullptr;
};

}
