#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/types/types.h"
#include "compiler/semantic/semantic_state/flow_state/flow_state_types.h"

namespace tz {

class SemanticState;
class PlacedNodes;

class FlowChanges {
public:
  explicit FlowChanges(SemanticState &owner);

  void forgetCallEffects(Flow &flow) const;
  void forgetFieldsOf(Flow &flow, const VarSymbol *variable) const;


  bool isChangedByCalls(const VarSymbol *variable) const;
  bool isStayingLocal(const FunctionDecl *functionDecl, llvm::StringRef name) const;

private:
  using NarrowedVar = std::pair<const VarSymbol *, const Type *>;

  void forgetCallChanged(llvm::SmallPtrSetImpl<const VarSymbol *> &variables) const;


  bool isArrayLength(const Term &term) const;
  bool isCallChangedPart(const Term &term) const;
  bool isCallChangedTerm(const Term &term) const;
  bool isCallChangedPath(const FieldPath &path) const;
  bool isCallChangedFact(const Facts::Fact &fact) const;
  bool isName(const Expr *expr, llvm::StringRef name) const;
  bool isIteratingOver(const Stmt *stmt, llvm::StringRef name) const;
  bool isReturningName(const Stmt *stmt, llvm::StringRef name) const;
  bool isReadingElement(const Expr *expr, llvm::StringRef name) const;
  bool isPathOf(const VarSymbol *variable, const FieldPath &path) const;
  bool isCallingArrayMethod(const Expr *expr, llvm::StringRef name) const;
  bool isArrayHiddenFromCallbacks(const CallExpr *call, size_t arrayParam) const;
  bool isNarrowedPathOf(const VarSymbol *variable,
                        const std::pair<FieldPath, const Type *> &entry) const;


  size_t countSpreads(const Expr *expr, llvm::StringRef name) const;
  size_t localExprUses(const Expr *expr, llvm::StringRef name) const;
  size_t localStmtUses(const Stmt *stmt, llvm::StringRef name) const;
  size_t countUses(const PlacedNodes &nodes, llvm::StringRef name) const;
  size_t countLocalUses(const PlacedNodes &nodes, llvm::StringRef name) const;


  const Expr *unwrapResult(const Expr *value) const;

  SemanticState &state;
};

}
