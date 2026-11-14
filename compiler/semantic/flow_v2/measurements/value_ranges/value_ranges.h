#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/types/types.h"
#include "compiler/semantic/flow/children_first/children_first.h"
#include "compiler/semantic/semantic_state/flow_state/flow_state_types.h"
#include "llvm/ADT/ArrayRef.h"
#include <vector>

namespace tz {

class SemanticState;

class ValueRanges {
public:
  explicit ValueRanges(SemanticState &owner);

  IntegerRange valueRange(const Expr *expr) const;

private:
  bool isKnownRange(const Range &r) const;


  Range exact(int64_t v) const;
  Range unite(const Range &a, const Range &b) const;
  Range clampToType(const Expr *expr, Range r) const;
  Range nodeRange(const Expr *expr, std::vector<Range> &answers) const;


  IntegerRange nameRange(const NameExpr *name) const;
  IntegerRange elementRangeOf(const Type *type) const;
  IntegerRange memberRange(const MemberExpr *member) const;
  IntegerRange lowerOf(const IntegerRange &a, const IntegerRange &b) const;
  IntegerRange upperOf(const IntegerRange &a, const IntegerRange &b) const;
  IntegerRange computedRange(const Expr *expr, llvm::ArrayRef<Range> parts) const;
  IntegerRange unaryRange(const UnaryExpr *unary, const IntegerRange &operand) const;
  IntegerRange castRange(const CastExpr *conversion, const IntegerRange &operand) const;
  IntegerRange callRange(const CallExpr *call, llvm::ArrayRef<IntegerRange> parts) const;
  IntegerRange minMaxRange(const CallExpr *call, llvm::ArrayRef<IntegerRange> parts) const;
  IntegerRange binaryRange(const BinaryExpr *binary, llvm::ArrayRef<IntegerRange> parts) const;


  NodeChildren<const Expr *> rangeParts(const Expr *expr) const;
  NodeChildren<const Expr *> callRangeParts(const CallExpr *call) const;

  SemanticState &state;
};

}
