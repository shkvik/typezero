#pragma once

#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class Scopes;
class FlowChecker;

class LoopBounds {
public:
  LoopBounds(SemanticState &owner, SemanticParts &all);

  Flow loopHead(const LoopScan &scan, const Expr *cond, bool induction) const;
  std::optional<int64_t> condBoundsAbove(const Expr *cond, const VarSymbol *var,
                                         const LoopScan &scan, int64_t step) const;
  bool condBoundsBelow(const Expr *cond, const VarSymbol *var, const LoopScan &scan,
                       int64_t step) const;
  bool boundedAbove(const Expr *expr, int64_t limit, const LoopScan &scan) const;
  bool boundedBelow(const Expr *expr, int64_t limit, const LoopScan &scan) const;
  std::optional<std::pair<int64_t, int64_t>> syntacticIntRange(const Expr *expr) const;

private:
  std::optional<int64_t> comparisonBoundsAbove(const Expr *part, const VarSymbol *var,
                                               const LoopScan &scan, int64_t step) const;
  bool comparisonBoundsBelow(const Expr *part, const VarSymbol *var, const LoopScan &scan,
                             int64_t step) const;
  std::optional<std::pair<int64_t, int64_t>> memberRange(const MemberExpr *member) const;
  static std::optional<int64_t>
  varPlus(const Expr *expr, const VarSymbol *var,
          llvm::function_ref<const VarSymbol *(llvm::StringRef)> lookupVar);
  static bool isArrayLength(const Term &t);

  struct KeptBounds {
    bool lower = false;
    bool upper = false;
    std::optional<int64_t> invariant;
  };

  void forgetChanged(Flow &head, const LoopScan &scan, llvm::StringRef name,
                     const LoopScan::Change &change, const Expr *cond, bool induction) const;
  KeptBounds keptBounds(const LoopScan &scan, const LoopScan::Change &change, const VarSymbol *var,
                        const Expr *cond) const;
  static bool isStaleFact(const Facts::Fact &f, llvm::StringRef name, const VarSymbol *var,
                          const KeptBounds &kept);
  static bool termNames(const Term &t, llvm::StringRef name);
  static void forgetNamed(Flow &head, llvm::StringRef name);
  static void forgetWrittenFields(Flow &head, const LoopScan &scan);
  static bool isWrittenPath(const FieldPath &path, const LoopScan &scan);
  static void forgetArrayFacts(Flow &head, const LoopScan &scan);
  static bool isArrayPart(const Term &t);

  SemanticState &state;
  Scopes *scopes = nullptr;
  FlowChecker *flowChecker = nullptr;
};

}
