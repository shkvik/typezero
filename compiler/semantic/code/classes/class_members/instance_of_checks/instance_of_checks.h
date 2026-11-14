#pragma once

#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/code/classes/code_classes_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <optional>

namespace tz {

class InstanceOfChecks {
public:
  explicit InstanceOfChecks(SemanticState &owner);

  void instanceOfRight(const InstanceOfRight &task);
  void afterInstanceOfLeft(const AfterInstanceOfLeft &task);
  void afterInstanceOfRight(const AfterInstanceOfRight &task);
  void checkInstanceOfOperator(const CheckInstanceOfOperator &task);

private:
  using ResultInstanceTested = Result<Expr *, CodeClassesRegistryCheckInstanceOfError>;
  using ResultBytesKindTested = Result<Expr *, CodeClassesRegistryCheckBytesInstanceOfError>;

  void pushInstanceOf(BinaryExpr *binary, const ResultInstanceTested &tested);


  bool isCollection(GlobalName global, const Type *type) const;
  bool isBytesKind(const Type *bytes, const Type *candidate) const;
  bool canBeBufferOf(const Type *bytes, const Type *candidate) const;
  bool canHoldClass(const Type *base, const ClassDecl *classDecl) const;
  bool isRelatedToGeneric(const Type *base, llvm::ArrayRef<const Type *> members,
                          const ClassDecl *classDecl) const;


  const Type *namedBytesType(const NameExpr *name) const;
  const Type *genericTestType(const Type *base, llvm::ArrayRef<const Type *> matching,
                              const ClassDecl *classDecl) const;


  ResultInstanceTested checkUnknownRight(BinaryExpr *binary) const;
  ResultInstanceTested checkConstructorRight(BinaryExpr *binary) const;
  ResultInstanceTested checkBytesTest(BinaryExpr *binary, const Type *bytes);
  ResultInstanceTested checkCollectionUnion(BinaryExpr *binary, GlobalName global) const;
  ResultInstanceTested checkKnownClass(BinaryExpr *binary, const ClassDecl *classDecl) const;
  ResultInstanceTested checkCollectionInstanceOf(BinaryExpr *binary, GlobalName global) const;
  ResultInstanceTested checkClassInstanceOf(BinaryExpr *binary, const ClassDecl *classDecl) const;
  ResultInstanceTested checkGenericInstanceOf(BinaryExpr *binary, const ClassDecl *classDecl) const;
  ResultInstanceTested checkRelatedClass(BinaryExpr *binary, const Type *base,
                                         const ClassDecl *classDecl) const;
  ResultInstanceTested checkGenericRelation(const Type *base, llvm::ArrayRef<const Type *> members,
                                            const ClassDecl *classDecl) const;


  ResultBytesKindTested checkBytesUnion(BinaryExpr *binary, const Type *bytes);
  ResultBytesKindTested checkBytesInstanceOf(BinaryExpr *binary, const Type *bytes);


  ClassDecl *knownClassOf(const Type *rhs) const;
  std::optional<GlobalName> collectionGlobal(const Expr *rhs) const;
  llvm::SMLoc instanceOfErrorPlace(const BinaryExpr *binary,
                                   CodeClassesRegistryCheckInstanceOfErrorCode code) const;

  SemanticState &state;
};

}
