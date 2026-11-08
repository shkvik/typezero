#pragma once

#include "compiler/semantic/generics/narrowed_part/narrowed_part.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace tz {

class WideningRelations {
public:
  explicit WideningRelations(SemanticState &owner);

  bool widensWrites(const Type *from, const Type *to);
  static bool sameReference(const Type *from, const Type *to);
  static std::vector<NarrowedPart> narrowingsOf(const Type *from, const Type *to);

private:
  using TypePairs = std::set<std::pair<const Type *, const Type *>>;

  static constexpr int maxDepth = 6;

  static void collectNarrowings(const Type *from, const Type *to, int depth,
                                std::vector<NarrowedPart> &out, TypePairs &seen);

  SemanticState &state;
};

}
