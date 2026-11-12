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
  bool isPossible;
  std::vector<ReferenceTask> needs;
};

class WideningRelations {
public:
  explicit WideningRelations(SemanticState &owner);

  bool isWideningWrites(const Type *from, const Type *to);
  static bool isSameReference(const Type *from, const Type *to);
  static bool isReferenceHolding(ReferenceTask goal);
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
  static void addFieldSteps(const Type *owner, const Field &own, const Field &wide, int depth,
                            std::vector<NarrowingStep> &steps);
  static ReferenceStep failedStep();
  static bool isReference(const Type *type);
  static bool isSpecialShape(const Type *type);
  static bool areObjects(const Type *from, const Type *to);
  static bool isSameShape(const Type *from, const Type *to);
  static ReferenceStep widensStep(const WidensGoal &goal);
  static ReferenceStep fitsStep(const ReferenceFitsGoal &goal);
  static ReferenceStep fitsWideStep(const FitsWideGoal &goal);
  static ReferenceStep sameStep(const SameReferenceGoal &goal);
  static ReferenceStep objectsStep(const ObjectsShareGoal &goal);
  static ReferenceStep wideMemberTrial(WideMemberChoice choice);
  static ReferenceStep referenceStep(const ReferenceTask &task);
  static ReferenceStep plannedStep(std::vector<ReferenceTask> needs);
  static ReferenceStep membersStep(const Type *own, const Type *wide);
  static bool retryWideMember(std::vector<ReferenceTask> &tasks);

  SemanticState &state;
};

}
