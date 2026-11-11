#pragma once

#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class ByteProofs;
class IndexProofs;
class PlacedNodes;

class FlowChecker {
public:
  FlowChecker(SemanticState &owner, SemanticParts &all);

  static Flow join(const Flow &a, const Flow &b);
  void assigned(const VarSymbol *var, const Expr *value);
  void incremented(const VarSymbol *var, int64_t delta);
  void calledFunction();
  void forgetCallEffects(Flow &flow) const;
  bool changedByCalls(const VarSymbol *var) const;
  static bool staysLocal(const FunctionDecl *fn, llvm::StringRef name);
  void arrayResized(const Type *type, bool grew, bool shrank, const VarSymbol *except = nullptr);
  void setDefined(const VarSymbol *var, bool defined);

private:
  static bool isArrayLength(const Term &t);
  static const Expr *unwrapResult(const Expr *value);
  static bool isName(const Expr *expr, llvm::StringRef name);
  static bool returnsName(const Stmt *stmt, llvm::StringRef name);
  static bool readsElement(const Expr *expr, llvm::StringRef name);
  static bool iteratesOver(const Stmt *stmt, llvm::StringRef name);
  static size_t countSpreads(const Expr *expr, llvm::StringRef name);
  static bool callsArrayMethod(const Expr *expr, llvm::StringRef name);
  static size_t countUses(const PlacedNodes &nodes, llvm::StringRef name);
  static bool callbacksSkipArray(const CallExpr *call, size_t arrayParam);
  static size_t countLocalUses(const PlacedNodes &nodes, llvm::StringRef name);
  static void addLess(Facts &facts, const Linear &x, const Linear &y, bool orEqual);

  using NarrowedVar = std::pair<const VarSymbol *, const Type *>;

  static void joinDefined(const Flow &a, const Flow &b, Flow &result);
  static const NarrowedVar *narrowedIn(const Flow &flow, const VarSymbol *var);
  static std::optional<NarrowedVar> narrowedOnBoth(const NarrowedVar &narrowed, const Flow &b);
  static void widenWithUndefined(const Flow &empty, const Flow &other, Flow &result);
  static void joinMarks(const Flow &a, const Flow &b, Flow &result);
  static bool hasPending(llvm::ArrayRef<std::pair<EmitterKey, llvm::SMLoc>> pending,
                         const EmitterKey &key);
  void resetAssignedFlow(const VarSymbol *var, const Expr *value);
  void rememberResultState(const VarSymbol *var, const Type *given);
  void assignedInteger(const VarSymbol *var, const Expr *value);
  void addMinMaxBounds(const VarSymbol *var, const Expr *value);
  void assignedLength(const VarSymbol *var, const Expr *value);

  SemanticState &state;
  ByteProofs *byteProofs = nullptr;
  IndexProofs *indexProofs = nullptr;
};

}
