#pragma once

#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <optional>
#include <vector>

namespace tz {

class GenericsVarianceRegistry {
public:
  explicit GenericsVarianceRegistry(SemanticState &owner);

  void joinInstances();
  void afterJoinedInstance(const AfterJoinedInstance &task);

private:
  struct JoinedClass {
    ClassDecl *generic;
    std::vector<const Type *> args;
  };

  std::optional<JoinedClass> joinedClass(const Type *type);

  SemanticState &state;
};

}
