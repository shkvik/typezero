#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/types/types.h"
#include "compiler/semantic/semantic_state/flow_state/flow_state_types.h"
#include <optional>
#include <utility>

namespace tz {

class SemanticState;

class LoopHeads {
public:
  explicit LoopHeads(SemanticState &owner);

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

  void forgetNamed(Flow &head, llvm::StringRef name) const;
  void forgetArrayFacts(Flow &head, const LoopScan &scan) const;
  void forgetLoopEffects(Flow &head, const LoopScan &scan) const;
  void forgetWrittenFields(Flow &head, const LoopScan &scan) const;
  void forgetNamedIn(llvm::SmallPtrSetImpl<const VarSymbol *> &variables,
                     llvm::StringRef name) const;
  void forgetChanged(Flow &head, llvm::StringRef name, const VarSymbol *variable,
                     const KeptBounds &kept) const;


  bool isSized(const Type *type) const;
  bool isArrayPart(const Term &term) const;
  bool isArrayLength(const Term &term) const;
  bool isTermNaming(const Term &term, llvm::StringRef name) const;
  bool isPathNamed(llvm::StringRef name, const FieldPath &path) const;
  bool isWrittenPath(const LoopScan &scan, const FieldPath &path) const;
  bool isInvariantBelow(const VarSymbol *variable, int64_t offset) const;
  bool isResizedArrayFact(const LoopScan &scan, const Facts::Fact &fact) const;
  bool isBoundedAbove(const Expr *expr, int64_t limit, const LoopScan &scan) const;
  bool isBoundedBelow(const Expr *expr, int64_t limit, const LoopScan &scan) const;
  bool isNarrowedPathNamed(llvm::StringRef name,
                           const std::pair<FieldPath, const Type *> &narrowed) const;
  bool isNarrowedPathWritten(const LoopScan &scan,
                             const std::pair<FieldPath, const Type *> &narrowed) const;
  bool isNarrowedNamed(llvm::StringRef name,
                       const std::pair<const VarSymbol *, const Type *> &narrowed) const;
  bool isCondBoundingBelow(const Expr *cond, const VarSymbol *variable, const LoopScan &scan,
                           int64_t step) const;
  bool isComparisonBoundingBelow(const Expr *part, const VarSymbol *variable, const LoopScan &scan,
                                 int64_t step) const;
  bool isStaleFact(llvm::StringRef name, const VarSymbol *variable, const KeptBounds &kept,
                   const Facts::Fact &fact) const;


  const VarSymbol *namedVar(const Expr *expr) const;
  const VarSymbol *unchangedInt(const Expr *expr, const LoopScan &scan) const;


  std::optional<int64_t> varPlus(const Expr *expr, const VarSymbol *variable) const;
  std::optional<int64_t> condBoundsAbove(const Expr *cond, const VarSymbol *variable,
                                         const LoopScan &scan, int64_t step) const;
  std::optional<int64_t> comparisonBoundsAbove(const Expr *part, const VarSymbol *variable,
                                               const LoopScan &scan, int64_t step) const;


  std::optional<IntRange> nameRange(const NameExpr *name) const;
  std::optional<IntRange> syntacticIntRange(const Expr *expr) const;
  std::optional<IntRange> negatedRange(const UnaryExpr *unary) const;
  std::optional<IntRange> memberRange(const MemberExpr *member) const;
  std::optional<IntRange> lengthMinusRange(const BinaryExpr *binary) const;
  std::optional<IntRange> literalRange(const IntLitExpr *literal, int64_t sign) const;


  std::optional<Comparison> ordered(const Expr *part) const;
  KeptBounds keptBounds(const LoopScan &scan, const LoopScan::Change &change,
                        const VarSymbol *variable, const Expr *cond) const;

  SemanticState &state;
};

}
