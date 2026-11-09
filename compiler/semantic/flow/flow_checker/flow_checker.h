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

  SemanticState &state;
  ByteProofs *byteProofs = nullptr;
  IndexProofs *indexProofs = nullptr;
};

}
