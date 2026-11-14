#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/code/loops/code_loops_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <optional>
#include <string>

namespace tz {

class ForOfLoops {
public:
  explicit ForOfLoops(SemanticState &owner);

  void walkForOf(const WalkForOf &task);
  void bindForOf(const BindForOf &task);
  void afterForAwait(const AfterForAwait &task);
  void afterIterable(const AfterIterable &task);
  void afterForInObject(const AfterForInObject &task);
  void afterForOfSource(const AfterForOfSource &task);
  void afterForOfBinding(const AfterForOfBinding &task);

private:
  using ResultBindingShape = Result<const Type *, CodeLoopsRegistryBindingShapeError>;
  using ResultForInKeysAllowed = Result<const Type *, CodeLoopsRegistryWalkableError>;
  using ResultIterableType = Result<const Type *, CodeLoopsRegistryIterableTypeError>;
  using ResultBindingTyped = Result<const VarDeclarator *, CodeLoopsRegistryBindingTypeError>;

  void startForIn(ForOfStmt *stmt);
  void startIterable(ForOfStmt *stmt);
  void pushTarget(JumpTarget::Kind kind);
  void startScannedBody(Stmt *body, const LoopScan &scan);
  void addKeyGuard(ForOfStmt *stmt, const NameExpr *variable);
  void addLiteralBounds(const ForOfStmt *stmt, const Type *element);


  bool isListParam(const NameExpr *name);
  bool isAsyncOnly(const Type *type) const;
  bool isConcurrentMap(const Type *type) const;
  bool isOptionalIterable(const Type *type) const;


  Expr *arrayCopy(Expr *tuple);
  Expr *forInKeys(Expr *object, bool isWalking);
  Expr *recordAgain(llvm::SMLoc loc, const NameExpr *variable);


  const Type *iteratedElement(ForOfStmt *stmt);
  const Type *listElementsType(const NameExpr *name) const;
  const Type *boundElement(const ForOfStmt *stmt, const Type *element);


  std::string indexedKindText(const Type *type) const;
  std::string forInText(const Expr *object, const Type *type) const;


  ArrayLitExpr *listElements(const NameExpr *name);
  IntegerRange literalRange(const ArrayLitExpr *literal) const;


  ResultIterableType iterableType(const Type *type) const;
  ResultBindingTyped bindingType(const ForOfStmt *stmt) const;
  ResultBindingShape bindingShape(const ForOfStmt *stmt, const Type *element) const;


  std::optional<int64_t> elementConstant(const ArrayLitExpr::Element &element) const;
  ResultForInKeysAllowed checkWalkable(const Type *type, llvm::StringRef text) const;


  BlockStmt *bodyBlock(ForOfStmt *stmt);

  SemanticState &state;
};

}
