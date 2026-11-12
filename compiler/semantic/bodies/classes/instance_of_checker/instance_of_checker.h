#pragma once

#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/classes/instance_of_checker/instance_of_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class BytesChecker;
class SemanticParts;
class Scopes;
class CallChecker;
class ExprChecker;
class TopLevelChecker;

class InstanceOfChecker {
public:
  using ResultInstanceTested = Result<Expr *, InstanceOfCheckerCheckInstanceOfError>;

  InstanceOfChecker(SemanticState &owner, SemanticParts &all);

  ResultInstanceTested checkInstanceOf(BinaryExpr *binary);

private:
  using ResultBytesKindTested = Result<Expr *, InstanceOfCheckerCheckBytesInstanceOfError>;

  ResultInstanceTested checkBytesTest(BinaryExpr *binary, const Type *bytes);
  ResultInstanceTested checkValueRight(BinaryExpr *binary);
  ResultInstanceTested checkKnownClass(BinaryExpr *binary, const ClassDecl *classDecl) const;
  std::optional<GlobalName> collectionGlobal(const Expr *rhs) const;
  bool isCollection(GlobalName global, const Type *type) const;
  ResultInstanceTested checkCollectionInstanceOf(BinaryExpr *binary, GlobalName global) const;
  ResultInstanceTested checkCollectionUnion(BinaryExpr *binary, GlobalName global) const;
  const Type *namedBytesType(const NameExpr *name) const;
  ClassDecl *knownClassOf(const Type *rhs) const;
  ResultInstanceTested checkUnknownRight(BinaryExpr *binary) const;
  ResultInstanceTested checkConstructorRight(BinaryExpr *binary) const;
  ResultInstanceTested checkGenericInstanceOf(BinaryExpr *binary, const ClassDecl *classDecl) const;
  const Type *genericTestType(const Type *base, llvm::ArrayRef<const Type *> matching,
                              const ClassDecl *classDecl) const;
  ResultInstanceTested checkGenericRelation(const Type *base, llvm::ArrayRef<const Type *> members,
                                            const ClassDecl *classDecl) const;
  bool isRelatedToGeneric(const Type *base, llvm::ArrayRef<const Type *> members,
                          const ClassDecl *classDecl) const;
  bool canHoldClass(const Type *base, const ClassDecl *classDecl) const;
  ResultInstanceTested checkClassInstanceOf(BinaryExpr *binary, const ClassDecl *classDecl) const;
  ResultInstanceTested checkRelatedClass(BinaryExpr *binary, const Type *base,
                                         const ClassDecl *classDecl) const;
  ResultBytesKindTested checkBytesInstanceOf(BinaryExpr *binary, const Type *bytes);
  ResultBytesKindTested checkBytesUnion(BinaryExpr *binary, const Type *bytes);
  bool isBytesKind(const Type *bytes, const Type *candidate) const;
  bool canBeBufferOf(const Type *bytes, const Type *candidate) const;

  SemanticState &state;
  Scopes *scopes = nullptr;
  CallChecker *callChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  BytesChecker *bytesChecker = nullptr;
  TopLevelChecker *topLevelChecker = nullptr;
};

}
