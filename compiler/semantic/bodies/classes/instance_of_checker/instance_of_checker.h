#pragma once

#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/classes/instance_of_checker/instance_of_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class Scopes;
class CallChecker;
class ExprChecker;
class TopLevelChecker;

class InstanceOfChecker {
public:
  using ResultInstanceTested = Result<Expr *, InstanceOfCheckerCheckInstanceOfError>;

  InstanceOfChecker(SemanticState &owner, SemanticParts &all);

  ResultInstanceTested checkInstanceOf(BinaryExpr *bin);

private:
  using ResultBytesKindTested = Result<Expr *, InstanceOfCheckerCheckBytesInstanceOfError>;

  ResultBytesKindTested checkBytesInstanceOf(BinaryExpr *bin, const Type *bytes);
  std::optional<GlobalName> collectionGlobal(const Expr *rhs) const;
  static bool isCollection(GlobalName global, const Type *t);
  static ResultInstanceTested checkCollectionInstanceOf(BinaryExpr *bin, GlobalName global);
  static ResultInstanceTested checkCollectionUnion(BinaryExpr *bin, GlobalName global);
  const Type *namedBytesType(const NameExpr *name) const;
  static ClassDecl *knownClassOf(const Type *rhs);
  static ResultInstanceTested checkUnknownRight(BinaryExpr *bin);
  static ResultInstanceTested checkGenericInstanceOf(BinaryExpr *bin, const ClassDecl *cls);
  static ResultInstanceTested
  genericMismatch(const Type *base, llvm::ArrayRef<const Type *> members, const ClassDecl *cls);
  static bool mayHoldClass(const Type *base, const ClassDecl *cls);
  static ResultInstanceTested checkClassInstanceOf(BinaryExpr *bin, const ClassDecl *cls);
  static ResultInstanceTested checkRelatedClass(BinaryExpr *bin, const Type *base,
                                                const ClassDecl *cls);

  SemanticState &state;
  Scopes *scopes = nullptr;
  CallChecker *callChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  TopLevelChecker *topLevelChecker = nullptr;
};

}
