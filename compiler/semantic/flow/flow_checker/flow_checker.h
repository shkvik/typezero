#pragma once

#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class ByteProofs;
class IndexProofs;

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
  static void addLess(Facts &facts, const Linear &x, const Linear &y, bool orEqual);
  static bool isArrayLength(const Term &t);

  SemanticState &state;
  ByteProofs *byteProofs = nullptr;
  IndexProofs *indexProofs = nullptr;
};

}
