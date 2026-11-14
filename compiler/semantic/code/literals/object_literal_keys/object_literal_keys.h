#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/code/literals/code_literals_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <optional>
#include <vector>

namespace tz {

class ObjectLiteralKeys {
public:
  explicit ObjectLiteralKeys(SemanticState &owner);

  void afterUnionNeeds(const AfterUnionNeeds &task);
  void afterComputedKey(const AfterComputedKey &task);
  void checkObjectLiteral(const CheckObjectLiteral &task);

private:
  using ResultComputedKeyKnown = Result<const Type *, CodeLiteralsRegistryComputedKeyError>;
  using ResultFieldNameIsFresh =
      Result<const ObjectLitExpr::Property *, CodeLiteralsRegistryUniqueNameError>;
  using ResultPropertyHasValue =
      Result<const ObjectLitExpr::Property *, CodeLiteralsRegistryPropertyPlainError>;

  void nextPropertyKey(ObjectLitDraft draft);
  void afterPropertyKeys(ObjectLitDraft draft);
  void continueObjectLit(ObjectLitDraft draft);
  void startRecordLit(ObjectLitExpr *literal, const Type *record);
  void walkThisMethods(ObjectLitExpr *literal, const Type *target) const;


  bool isLiteralSet(const Type *type) const;
  bool isUsableTarget(const Type *type) const;
  bool isLiteralKeys(const Type *keyType) const;
  bool isPlainObjectMember(const Type *member) const;
  bool isAllKeyed(const ObjectLitExpr *literal) const;
  bool hasObjectSpread(const ObjectLitExpr *literal) const;
  bool isTagConvertible(const Type *tag, const Type *fieldType) const;
  bool hasProperty(const ObjectLitExpr *literal, llvm::StringRef name) const;
  bool hasFittingNames(const ObjectLitExpr *literal, const Type *member) const;


  const Type *enumTag(const Expr *expr) const;
  const Type *knownLiteralSet(const Expr *value) const;
  const Type *literalMember(const ObjectLitExpr *literal, const Type *target) const;
  const Type *propertyTag(const ObjectLitExpr::Property &property, const Field *field) const;


  std::optional<int> memberScore(const ObjectLitExpr *literal, const Type *member) const;
  std::optional<int> candidateScore(const ObjectLitExpr *literal, const Type *member) const;
  std::optional<int> propertyScore(const ObjectLitExpr::Property &property,
                                   const Type *member) const;


  ResultComputedKeyKnown checkComputedKey(const Type *keyType) const;
  ResultPropertyHasValue checkPropertyPlain(const ObjectLitExpr::Property &property) const;
  ResultFieldNameIsFresh checkUniqueName(const ObjectLitExpr *literal, size_t index,
                                         llvm::ArrayRef<bool> named) const;


  const VarSymbol *namedVariable(const Expr *expr) const;
  std::vector<EnumDecl *> referencedEnums(const ObjectLitExpr *literal) const;

  SemanticState &state;
};

}
