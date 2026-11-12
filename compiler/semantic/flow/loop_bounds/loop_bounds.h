#pragma once

#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class Scopes;
class FlowChecker;

class LoopBounds {
public:
  LoopBounds(SemanticState &owner, SemanticParts &all);

  Flow loopHead(const LoopScan &scan) const;
  Flow countedLoopHead(const LoopScan &scan, const Expr *cond) const;

private:
  struct IntRange {
    int64_t low;
    int64_t high;
  };

  struct Comparison {
    const Expr *small;
    const Expr *large;
    bool isStrict;
  };

  struct KeptBounds {
    bool isLowerKept = false;
    bool isUpperKept = false;
    std::optional<int64_t> invariant;
  };

  const VarSymbol *namedVar(const Expr *expr) const;
  std::optional<int64_t> varPlus(const Expr *expr, const VarSymbol *variable) const;
  bool isArrayLength(const Term &term) const;
  std::optional<IntRange> syntacticIntRange(const Expr *expr) const;
  std::optional<IntRange> literalRange(const IntLitExpr *literal, int64_t sign) const;
  std::optional<IntRange> negatedRange(const UnaryExpr *unary) const;
  std::optional<IntRange> nameRange(const NameExpr *name) const;
  std::optional<IntRange> lengthMinusRange(const BinaryExpr *binary) const;
  std::optional<IntRange> memberRange(const MemberExpr *member) const;
  bool isSized(const Type *type) const;
  const VarSymbol *unchangedInt(const Expr *expr, const LoopScan &scan) const;
  bool isBoundedAbove(const Expr *expr, int64_t limit, const LoopScan &scan) const;
  bool isBoundedBelow(const Expr *expr, int64_t limit, const LoopScan &scan) const;
  std::optional<Comparison> ordered(const Expr *part) const;
  std::optional<int64_t> condBoundsAbove(const Expr *cond, const VarSymbol *variable,
                                         const LoopScan &scan, int64_t step) const;
  std::optional<int64_t> comparisonBoundsAbove(const Expr *part, const VarSymbol *variable,
                                               const LoopScan &scan, int64_t step) const;
  bool isCondBoundingBelow(const Expr *cond, const VarSymbol *variable, const LoopScan &scan,
                           int64_t step) const;
  bool isComparisonBoundingBelow(const Expr *part, const VarSymbol *variable, const LoopScan &scan,
                                 int64_t step) const;
  void forgetLoopEffects(Flow &head, const LoopScan &scan) const;
  void forgetChanged(Flow &head, llvm::StringRef name, const VarSymbol *variable,
                     const KeptBounds &kept) const;
  bool isInvariantBelow(const VarSymbol *variable, int64_t offset) const;
  KeptBounds keptBounds(const LoopScan &scan, const LoopScan::Change &change,
                        const VarSymbol *variable, const Expr *cond) const;
  bool isStaleFact(llvm::StringRef name, const VarSymbol *variable, const KeptBounds &kept,
                   const Facts::Fact &fact) const;
  bool isTermNaming(const Term &term, llvm::StringRef name) const;
  bool isNarrowedNamed(llvm::StringRef name,
                       const std::pair<const VarSymbol *, const Type *> &narrowed) const;
  bool isPathNamed(llvm::StringRef name, const FieldPath &path) const;
  bool isNarrowedPathNamed(llvm::StringRef name,
                           const std::pair<FieldPath, const Type *> &narrowed) const;
  void forgetNamedIn(llvm::SmallPtrSetImpl<const VarSymbol *> &variables,
                     llvm::StringRef name) const;
  void forgetNamed(Flow &head, llvm::StringRef name) const;
  bool isNarrowedPathWritten(const LoopScan &scan,
                             const std::pair<FieldPath, const Type *> &narrowed) const;
  void forgetWrittenFields(Flow &head, const LoopScan &scan) const;
  bool isWrittenPath(const LoopScan &scan, const FieldPath &path) const;
  bool isResizedArrayFact(const LoopScan &scan, const Facts::Fact &fact) const;
  void forgetArrayFacts(Flow &head, const LoopScan &scan) const;
  bool isArrayPart(const Term &term) const;

  SemanticState &state;
  Scopes *scopes = nullptr;
  FlowChecker *flowChecker = nullptr;
};

}
