#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/code/indexing/code_indexing_registry_errors.h"
#include "compiler/semantic/code/unknown_values/code_unknown_values_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"

namespace tz {

class CodeIndexingRegistry {
public:
  explicit CodeIndexingRegistry(SemanticState &owner);

  void checkIndex(const CheckIndex &task);
  void checkPosition(const CheckPosition &task);
  void finishPosition(const FinishPosition &task);
  void writeIndexType(const WriteIndexType &task);
  void enumIndexValue(const EnumIndexValue &task);
  void finishEnumIndex(const FinishEnumIndex &task);
  void afterIndexObject(const AfterIndexObject &task);
  void afterIndexListPart(const AfterIndexListPart &task);
  void finishGenericIndex(const FinishGenericIndex &task);
  void finishUnknownIndex(const FinishUnknownIndex &task);
  void finishPositionIndex(const FinishPositionIndex &task);
  void afterProcessEnvIndex(const AfterProcessEnvIndex &task);
  void reportUndefinedIndex(const ReportUndefinedIndex &task);
  void afterEnumIndexResolved(const AfterEnumIndexResolved &task);

private:
  using ResultIndexIsInteger = Result<const Type *, CodeIndexingRegistryPositionTypeError>;
  using ResultUnknownNarrowed = Result<const Expr *, CodeUnknownValuesRegistryUnknownUseError>;

  void typeListParamIndex(IndexExpr *index, const Type *type);
  void reportIndex(IndexExpr *index, CodeIndexingRegistryCheckIndexErrorCode code,
                   const std::string &message);


  bool isGenericKey(const Type *key) const;
  bool isGenericIndex(const IndexExpr *index) const;
  bool isGenericKeyName(const NameExpr *keyName) const;


  Expr *chainLink(Expr *object);
  Expr *shortCircuitLink(Expr *object, const Type *type);
  Expr *memberNameAt(const EnumDecl *enumDecl, int64_t number, llvm::SMLoc loc);


  std::string hintName(const Expr *expr) const;
  const Type *indexedElement(const Type *type) const;
  ResultIndexIsInteger checkPositionType(const Type *type) const;
  ResultUnknownNarrowed unknownUse(const Expr *object, llvm::StringRef what) const;
  const EnumMember *lastMemberNumbered(const EnumDecl *enumDecl, int64_t number) const;
  llvm::SMLoc indexErrorLoc(const IndexExpr *index,
                            CodeIndexingRegistryCheckIndexErrorCode code) const;

  SemanticState &state;
};

}
