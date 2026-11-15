#pragma once

#include "compiler/program/types/types.h"
#include "compiler/semantic/generics/narrowed_part/narrowed_part.h"
#include "compiler/union/union/union.h"
#include <set>
#include <utility>
#include <vector>

namespace tz {

struct TypeStorage;

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

class TypesReferences {
public:
  explicit TypesReferences(TypeStorage &owner);

  bool areObjects(const Type *from, const Type *to);
  bool isSameReference(const Type *from, const Type *to);
  bool isReferenceHolding(ReferenceTask goal);
  std::vector<NarrowedPart> narrowingsOf(const Type *from, const Type *to);

private:
  using TypePairs = std::set<std::pair<const Type *, const Type *>>;

  void collectNarrowings(const Type *from, const Type *to, int depth,
                         std::vector<NarrowedPart> &out, TypePairs &seen);
  void visitNarrowing(const VisitNarrowing &visit, std::vector<NarrowedPart> &out, TypePairs &seen,
                      std::vector<NarrowingStep> &steps);
  void pushFieldNarrowings(const Type *from, const Type *to, int depth,
                           std::vector<NarrowingStep> &steps);
  void addFieldSteps(const Type *owner, const Field &own, const Field &wide, int depth,
                     std::vector<NarrowingStep> &steps);


  bool isReference(const Type *type);
  bool isSpecialShape(const Type *type);
  bool isSameShape(const Type *from, const Type *to);
  bool retryWideMember(std::vector<ReferenceTask> &tasks);


  ReferenceStep failedStep();
  ReferenceStep widensStep(const WidensGoal &goal);
  ReferenceStep fitsStep(const ReferenceFitsGoal &goal);
  ReferenceStep fitsWideStep(const FitsWideGoal &goal);
  ReferenceStep sameStep(const SameReferenceGoal &goal);
  ReferenceStep objectsStep(const ObjectsShareGoal &goal);
  ReferenceStep wideMemberTrial(WideMemberChoice choice);
  ReferenceStep referenceStep(const ReferenceTask &task);
  ReferenceStep plannedStep(std::vector<ReferenceTask> needs);
  ReferenceStep membersStep(const Type *own, const Type *wide);

  TypeStorage &types;
};

}
