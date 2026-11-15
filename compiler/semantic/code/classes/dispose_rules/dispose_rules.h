#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/code/classes/dispose_rules/dispose_rules_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include "llvm/ADT/ArrayRef.h"
#include <vector>

namespace tz {

class DisposeRules {
public:
  explicit DisposeRules(SemanticState &owner);

  void walkDispose(const ClassDecl *classDecl);
  void walkUsing(const VarDeclStmt *varDecl, const Type *type,
                 llvm::ArrayRef<const Stmt *> following);
  bool isInsideDispose() const;
  const FunctionDecl *findDispose(const ClassDecl *classDecl) const;

private:
  using ResultDisposeShape = Result<const FunctionDecl *, DisposeRulesError>;
  using ResultSuperDispose = Result<const FunctionDecl *, DisposeRulesError>;
  using ResultThisIsKeptIn = Result<const FunctionDecl *, DisposeRulesError>;
  using ResultUsingIsValid = Result<const Type *, DisposeRulesError>;
  using ResultUsingStaysIn = Result<llvm::StringRef, DisposeRulesError>;

  ResultDisposeShape checkDisposeShape(const FunctionDecl *dispose) const;
  ResultSuperDispose checkSuperDisposed(const ClassDecl *classDecl,
                                        const FunctionDecl *dispose) const;
  ResultThisIsKeptIn checkThisKeptIn(const FunctionDecl *dispose) const;
  ResultUsingIsValid checkUsingType(const Type *type) const;
  ResultUsingStaysIn checkUsingStays(llvm::StringRef name,
                                     llvm::ArrayRef<const Stmt *> following) const;

  const FunctionDecl *ownDispose(const ClassDecl *classDecl) const;
  bool hasSuperDisposeCall(const FunctionDecl *dispose) const;
  bool isSuperDisposeCall(const Expr *expr) const;
  bool isSuperName(const Expr *expr) const;
  bool isNamed(const Expr *expr, llvm::StringRef name) const;
  bool isNamedAs(llvm::StringRef name, const Expr *expr) const;
  bool isLeavingThis(const FunctionDecl *dispose) const;
  bool isEscaping(llvm::StringRef name, const Stmt *stmt) const;
  bool isCaptured(const Expr *expr, llvm::StringRef name) const;
  std::vector<const Expr *> heldParts(const Expr *expr) const;

  SemanticState &state;
};

}
