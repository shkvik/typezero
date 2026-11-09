#pragma once

#include "compiler/semantic/generics/narrowed_part/narrowed_part.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include "compiler/union/union/union.h"
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace tz {

enum class NarrowingStepKind { VISIT, EMIT };

struct VisitNarrowing {
  static constexpr NarrowingStepKind kind = NarrowingStepKind::VISIT;
  const Type *from;
  const Type *to;
  int depth;
};

struct EmitNarrowing {
  static constexpr NarrowingStepKind kind = NarrowingStepKind::EMIT;
  NarrowedPart part;
};

using NarrowingStep = Union<VisitNarrowing, EmitNarrowing>;

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
  static void visitNarrowing(const VisitNarrowing &visit, std::vector<NarrowedPart> &out,
                             TypePairs &seen, std::vector<NarrowingStep> &steps);
  static void pushFieldNarrowings(const Type *from, const Type *to, int depth,
                                  std::vector<NarrowingStep> &steps);
  static bool membersShareReference(const Type *own, const Type *wide);
  static bool objectsShareReference(const Type *from, const Type *to);

  SemanticState &state;
};

}
