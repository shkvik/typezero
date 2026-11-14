#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/types/types.h"
#include "compiler/semantic/flow_v2/measurements/builtin_lengths/builtin_lengths.h"
#include "compiler/semantic/flow_v2/measurements/linear_forms/linear_forms.h"
#include "compiler/semantic/flow_v2/measurements/value_ranges/value_ranges.h"

namespace tz {

class SemanticState;

class FlowMeasurements {
public:
  explicit FlowMeasurements(SemanticState &owner);

  bool isRawBuffer(const Type *type) const;


  IntegerRange valueRange(const Expr *expr) const;
  IntegerRange lengthRange(const Expr *value) const;


  std::optional<Linear> linear(const Expr *expr) const;
  std::optional<Linear> lengthOf(const Expr *expr) const;
  std::optional<Linear> knownLength(const Expr *value) const;
  std::optional<Linear> lengthAtMost(const Expr *expr) const;


  Linear settled(Linear value) const;
  const VarSymbol *variableOf(const Expr *expr) const;
  std::optional<int64_t> exactLength(const Expr *object) const;

private:
  BuiltinLengths builtinLengths;
  ValueRanges valueRanges;
  LinearForms linearForms;
};

}
