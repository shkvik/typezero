#pragma once

#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class FieldFacts;
class SemanticParts;
class ByteProofs;
class IndexProofs;
class PlacedNodes;
class UnionNarrowing;

enum class Resize {
  GROWS,
  SHRINKS,
  CHANGES,
};

class FlowChecker {
public:
  FlowChecker(SemanticState &owner, SemanticParts &all);

  void calledFunction();
  void markDefined(const VarSymbol *variable);
  void forgetCallEffects(Flow &flow) const;
  bool isChangedByCalls(const VarSymbol *variable) const;
  Flow join(const Flow &left, const Flow &right) const;
  void assigned(const VarSymbol *variable, const Expr *value);
  void incremented(const VarSymbol *variable, int64_t delta);
  bool isStayingLocal(const FunctionDecl *functionDecl, llvm::StringRef name) const;
  void arrayResized(const Type *type, Resize resize, const VarSymbol *except = nullptr);

private:
  using NarrowedVar = std::pair<const VarSymbol *, const Type *>;

  bool isArrayLength(const Term &term) const;
  bool isCallChangedPart(const Term &term) const;
  bool isCallChangedTerm(const Term &term) const;
  const Expr *unwrapResult(const Expr *value) const;
  bool isCallChangedPath(const FieldPath &path) const;
  bool isCallChangedFact(const Facts::Fact &fact) const;
  bool isShiftSafe(const Term &term, int64_t delta) const;
  bool isName(const Expr *expr, llvm::StringRef name) const;
  bool isReturningName(const Stmt *stmt, llvm::StringRef name) const;
  bool isIteratingOver(const Stmt *stmt, llvm::StringRef name) const;
  size_t countSpreads(const Expr *expr, llvm::StringRef name) const;
  bool isReadingElement(const Expr *expr, llvm::StringRef name) const;
  size_t localExprUses(const Expr *expr, llvm::StringRef name) const;
  size_t localStmtUses(const Stmt *stmt, llvm::StringRef name) const;
  void rememberResultState(const VarSymbol *variable, const Type *given);
  bool isCallingArrayMethod(const Expr *expr, llvm::StringRef name) const;
  size_t countUses(const PlacedNodes &nodes, llvm::StringRef name) const;
  void resetAssignedFlow(const VarSymbol *variable, const Expr *value);
  void assignedInteger(const VarSymbol *variable, const Expr *value);
  void addMinMaxBounds(const VarSymbol *variable, const Expr *value);
  void assignedLength(const VarSymbol *variable, const Expr *value);
  bool isPlainUnion(const Type *either, const VarSymbol *variable) const;
  size_t countLocalUses(const PlacedNodes &nodes, llvm::StringRef name) const;
  bool isArrayHiddenFromCallbacks(const CallExpr *call, size_t arrayParam) const;
  void joinMarks(const Flow &left, const Flow &right, Flow &result) const;
  void joinDefined(const Flow &left, const Flow &right, Flow &result) const;
  void forgetCallChanged(llvm::SmallPtrSetImpl<const VarSymbol *> &variables) const;
  const NarrowedVar *narrowedIn(const Flow &flow, const VarSymbol *variable) const;
  void addAtMost(Facts &facts, const Linear &smaller, const Linear &larger) const;
  void widenWithUndefined(const Flow &empty, const Flow &other, Flow &result) const;
  std::optional<NarrowedVar> narrowedOnBoth(const NarrowedVar &narrowed, const Flow &other) const;
  bool isResizedPart(const Term &term, const Type *array, const VarSymbol *except) const;
  bool isResizedTerm(const Term &term, const Type *array, const VarSymbol *except) const;
  bool hasPending(llvm::ArrayRef<std::pair<EmitterKey, llvm::SMLoc>> pending,
                  const EmitterKey &key) const;
  std::optional<NarrowedVar> widenedWithUndefined(const VarSymbol *variable, const Flow &other,
                                                  const Flow &result) const;
  bool isResizedFact(const Type *array, Resize resize, const VarSymbol *except,
                     const Facts::Fact &fact) const;

  SemanticState &state;
  ByteProofs *byteProofs = nullptr;
  FieldFacts *fieldFacts = nullptr;
  IndexProofs *indexProofs = nullptr;
  UnionNarrowing *unionNarrowing = nullptr;
};

}
