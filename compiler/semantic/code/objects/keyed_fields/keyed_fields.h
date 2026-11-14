#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/code/objects/code_objects_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <optional>
#include <string>

namespace tz {

class KeyedFields {
public:
  explicit KeyedFields(SemanticState &owner);

  void afterTupleKey(const AfterTupleKey &task);
  void afterObjectKey(const AfterObjectKey &task);
  void objectKeyReady(const ObjectKeyReady &task);
  void checkKeyedField(const CheckKeyedField &task);

private:
  using ResultKeyedFieldRead = Result<Expr *, CodeObjectsRegistryCheckKeyedFieldError>;
  using ResultKeyedFieldName = Result<std::string, CodeObjectsRegistryCheckKeyedFieldError>;

  void reportKeyed(IndexExpr *index, const CodeObjectsRegistryCheckKeyedFieldError &error);
  void pushNamedField(IndexExpr *index, const Type *type, const std::string &name,
                      KeyedAccess access);


  bool isInherited(llvm::StringRef name) const;
  bool isAllAssigned(llvm::ArrayRef<const Type *> names) const;
  bool isWrongKey(CodeObjectsRegistryCheckKeyedFieldErrorCode code) const;
  bool hasReadonly(const Type *type, llvm::ArrayRef<const Type *> names) const;
  bool isPublicField(const ClassDecl *classDecl, const std::string &name) const;


  const Type *constTextType(const Expr *checked) const;
  const Type *keyedFieldType(const Type *type, const Type *keyType) const;


  ResultKeyedFieldName checkTupleKeyName(IndexExpr *index, const Type *type);
  ResultKeyedFieldRead checkKeySetWrite(IndexExpr *index, const Type *type, const Type *keyType,
                                        const Type *field);
  ResultKeyedFieldName checkObjectKeyName(const Type *type, const Type *keyType,
                                          const std::optional<std::string> &key) const;


  ClassDecl *classOf(const Type *type) const;
  Expr *keySetIndex(IndexExpr *index, const Type *field) const;
  std::optional<std::u16string> writtenText(const Expr *expr) const;

  SemanticState &state;
};

}
