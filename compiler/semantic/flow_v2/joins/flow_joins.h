#pragma once

#include "compiler/semantic/semantic_state/flow_state/flow_state_types.h"
#include "llvm/ADT/ArrayRef.h"
#include <optional>
#include <utility>

namespace tz {

class FlowJoins {
public:
  Flow join(const Flow &left, const Flow &right) const;

private:
  using NarrowedVar = std::pair<const VarSymbol *, const Type *>;

  void joinMarks(const Flow &left, const Flow &right, Flow &result) const;
  void joinDefined(const Flow &left, const Flow &right, Flow &result) const;
  void widenWithUndefined(const Flow &empty, const Flow &other, Flow &result) const;


  bool isPlainUnion(const Type *either, const VarSymbol *variable) const;
  bool hasPending(llvm::ArrayRef<std::pair<EmitterKey, llvm::SMLoc>> pending,
                  const EmitterKey &key) const;


  const NarrowedVar *narrowedIn(const Flow &flow, const VarSymbol *variable) const;
  std::optional<NarrowedVar> narrowedOnBoth(const NarrowedVar &narrowed, const Flow &other) const;
  std::optional<NarrowedVar> widenedWithUndefined(const VarSymbol *variable, const Flow &other,
                                                  const Flow &result) const;
};

}
