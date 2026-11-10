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

enum class ReferenceTaskKind { SAME, OBJECTS, WIDENS, FITS, FITS_WIDE, WIDE_CHOICE };

struct SameReferenceGoal {
  static constexpr ReferenceTaskKind kind = ReferenceTaskKind::SAME;
  const Type *from;
  const Type *to;
};

struct ObjectsShareGoal {
  static constexpr ReferenceTaskKind kind = ReferenceTaskKind::OBJECTS;
  const Type *from;
  const Type *to;
};

struct WidensGoal {
  static constexpr ReferenceTaskKind kind = ReferenceTaskKind::WIDENS;
  const Type *from;
  const Type *to;
};

struct ReferenceFitsGoal {
  static constexpr ReferenceTaskKind kind = ReferenceTaskKind::FITS;
  const Type *own;
  const Type *wide;
};

struct FitsWideGoal {
  static constexpr ReferenceTaskKind kind = ReferenceTaskKind::FITS_WIDE;
  const Type *own;
  const Type *wide;
};

struct WideMemberChoice {
  static constexpr ReferenceTaskKind kind = ReferenceTaskKind::WIDE_CHOICE;
  const Type *own;
  const Type *wide;
  size_t next;
};

using ReferenceTask = Union<SameReferenceGoal, ObjectsShareGoal, WidensGoal, ReferenceFitsGoal,
                            FitsWideGoal, WideMemberChoice>;

struct ReferenceStep {
  bool possible;
  std::vector<ReferenceTask> needs;
};

class WideningRelations {
public:
  explicit WideningRelations(SemanticState &owner);

  bool widensWrites(const Type *from, const Type *to);
  static bool sameReference(const Type *from, const Type *to);
  static bool referenceHolds(ReferenceTask goal);
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
  static ReferenceStep referenceStep(const ReferenceTask &task);
  static ReferenceStep sameStep(const Type *from, const Type *to);
  static ReferenceStep membersStep(const Type *own, const Type *wide);
  static ReferenceStep objectsStep(const Type *from, const Type *to);
  static bool sameShapeFields(const Type *from, const Type *to);
  static ReferenceStep widensStep(const Type *from, const Type *to);
  static ReferenceStep fitsStep(const Type *own, const Type *wide);
  static ReferenceStep fitsWideStep(const Type *own, const Type *wide);
  static ReferenceStep wideMemberTrial(WideMemberChoice choice);
  static bool retryWideMember(std::vector<ReferenceTask> &tasks);

  SemanticState &state;
};

}
