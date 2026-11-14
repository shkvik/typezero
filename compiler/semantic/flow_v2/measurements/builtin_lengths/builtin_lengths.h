#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/types/types.h"
#include "compiler/semantic/semantic_state/flow_state/flow_state_types.h"
#include <optional>
#include <vector>

namespace tz {

class SemanticState;
class AwaitExpr;

class BuiltinLengths {
public:
  explicit BuiltinLengths(SemanticState &owner);

  bool isSizedAllocation(const CallExpr *call) const;


  const Expr *measuredCall(const CallExpr *call) const;
  const Expr *sameLengthSource(const Expr *object) const;


  const CallExpr *freshBufferSource(const CallExpr *call) const;
  std::optional<Linear> callLength(const CallExpr *call, std::vector<Linear> &answers) const;

private:
  bool isFreshTypedArray(const CallExpr *made) const;
  bool isSurrogatePair(const std::u16string &text, size_t index) const;


  int64_t utf8UnitSize(char16_t unit) const;
  int64_t utf8Size(const std::u16string &text) const;


  const Expr *copiedSource(const CallExpr *call) const;
  const Expr *sameLengthInner(const Expr *object) const;
  const Expr *singleAllArgument(const AwaitExpr *awaited) const;


  std::optional<Linear> utf8Length(const CallExpr *call) const;
  std::optional<Linear> embeddedLength(const CallExpr *call) const;
  std::optional<Linear> allocatedCount(std::vector<Linear> &answers) const;
  std::optional<Linear> freshBufferLength(const CallExpr *call, std::vector<Linear> &answers) const;

  SemanticState &state;
};

}
