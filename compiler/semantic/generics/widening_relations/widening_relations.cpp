#include "compiler/semantic/generics/widening_relations/widening_relations.h"
#include "compiler/semantic/generics/generic_widening/generic_widening.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

WideningRelations::WideningRelations(SemanticState &owner)
    : state(owner) {}

ReferenceStep WideningRelations::failedStep() {
  return ReferenceStep{.isPossible = false, .needs = {}};
}

ReferenceStep WideningRelations::plannedStep(std::vector<ReferenceTask> needs) {
  return ReferenceStep{.isPossible = true, .needs = std::move(needs)};
}

bool WideningRelations::areObjects(const Type *from, const Type *to) {
  bool isPlainFrom = from->isObject() && !from->isAnyObject();
  bool isPlainTo = to->isObject() && !to->isAnyObject();

  return isPlainFrom && isPlainTo;
}

void WideningRelations::collectNarrowings(const Type *from, const Type *to, int depth,
                                          std::vector<NarrowedPart> &out, TypePairs &seen) {
  std::vector<NarrowingStep> steps = {VisitNarrowing{.from = from, .to = to, .depth = depth}};
  while (!steps.empty()) {
    NarrowingStep step = steps.back();
    steps.pop_back();
    const VisitNarrowing *visit = step.as<VisitNarrowing>();
    if (visit != nullptr) {
      WideningRelations::visitNarrowing(*visit, out, seen, steps);
    } else {
      out.push_back(step.as<EmitNarrowing>()->part);
    }
  }
}

void WideningRelations::visitNarrowing(const VisitNarrowing &visit, std::vector<NarrowedPart> &out,
                                       TypePairs &seen, std::vector<NarrowingStep> &steps) {
  const Type *from = visit.from->nonOptional();
  const Type *to = visit.to->nonOptional();
  int depth = visit.depth;
  if (from == to || depth > WideningRelations::maxDepth) {
    return;
  }
  bool isFirstVisit = seen.insert({from, to}).second;
  if (!isFirstVisit) {
    return;
  }
  if (from->isArray() && to->isArray()) {
    if (from->inner() != to->inner()) {
      out.push_back(NarrowedPart{
          .depth = depth,
          .element = true,
          .name = "",
          .narrow = from->inner(),
          .owner = from,
      });
    }
    steps.push_back(VisitNarrowing{.from = from->inner(), .to = to->inner(), .depth = depth + 1});
    return;
  }
  if (from->isUnion() && !llvm::is_contained(from->members(), to)) {
    for (const Type *member : llvm::reverse(from->members())) {
      steps.push_back(VisitNarrowing{.from = member, .to = to, .depth = depth});
    }
  }
  if (!from->isUnion() && WideningRelations::areObjects(from, to)) {
    WideningRelations::pushFieldNarrowings(from, to, depth, steps);
  }
}

void WideningRelations::pushFieldNarrowings(const Type *from, const Type *to, int depth,
                                            std::vector<NarrowingStep> &steps) {
  auto fieldSteps = std::vector<NarrowingStep>();
  for (const Field &wide : to->fields()) {
    const auto *own = from->field(wide.name);
    if (own != nullptr) {
      WideningRelations::addFieldSteps(from, *own, wide, depth, fieldSteps);
    }
  }
  steps.insert(steps.end(), fieldSteps.rbegin(), fieldSteps.rend());
}

void WideningRelations::addFieldSteps(const Type *owner, const Field &own, const Field &wide,
                                      int depth, std::vector<NarrowingStep> &steps) {
  bool isOtherType = own.type != wide.type;
  if (own.readonly && !wide.readonly) {
    auto part = NarrowedPart{
        .depth = depth,
        .element = false,
        .name = wide.name,
        .narrow = nullptr,
        .owner = owner,
    };
    steps.push_back(EmitNarrowing{.part = part});
  } else if (isOtherType) {
    auto part = NarrowedPart{
        .depth = depth,
        .element = false,
        .name = wide.name,
        .narrow = own.type,
        .owner = owner,
    };
    steps.push_back(EmitNarrowing{.part = part});
  }
  if (isOtherType) {
    steps.push_back(VisitNarrowing{.from = own.type, .to = wide.type, .depth = depth + 1});
  }
}

std::vector<NarrowedPart> WideningRelations::narrowingsOf(const Type *from, const Type *to) {
  auto out = std::vector<NarrowedPart>();
  TypePairs seen;
  WideningRelations::collectNarrowings(from, to, 0, out, seen);

  return out;
}

bool WideningRelations::isWideningWrites(const Type *from, const Type *to) {
  const Type *bareFrom = from->nonOptional();
  const Type *bareTo = to->nonOptional();
  if (bareFrom == bareTo || GenericWidening::isWidening(bareFrom, bareTo)) {
    return false;
  }
  bool hasArrays = bareFrom->isArray() && bareTo->isArray();
  bool hasObjects = WideningRelations::areObjects(bareFrom, bareTo);
  bool isUnionToObject = bareFrom->isUnion() && bareTo->isObject();
  if (!hasArrays && !hasObjects && !isUnionToObject) {
    return false;
  }
  auto &widenings = this->state.getWideningStore();
  if (widenings.hasKnownWidening(bareFrom, bareTo)) {
    return widenings.isNarrowingWidening(bareFrom, bareTo);
  }
  auto narrowings = WideningRelations::narrowingsOf(bareFrom, bareTo);
  bool isNarrowed = !narrowings.empty();
  widenings.rememberWidening(bareFrom, bareTo, isNarrowed);

  return isNarrowed;
}

bool WideningRelations::isSameReference(const Type *from, const Type *to) {
  return WideningRelations::isReferenceHolding(SameReferenceGoal{.from = from, .to = to});
}

bool WideningRelations::isReferenceHolding(ReferenceTask goal) {
  std::vector<ReferenceTask> tasks = {std::move(goal)};
  while (!tasks.empty()) {
    ReferenceTask task = std::move(tasks.back());
    tasks.pop_back();
    auto step = WideningRelations::referenceStep(task);
    if (step.isPossible) {
      tasks.insert(tasks.end(), step.needs.rbegin(), step.needs.rend());
    } else if (!WideningRelations::retryWideMember(tasks)) {
      return false;
    }
  }

  return true;
}

ReferenceStep WideningRelations::referenceStep(const ReferenceTask &task) {
  switch (task.kind()) {
    case ReferenceTaskKind::SAME:
      return WideningRelations::sameStep(*task.as<SameReferenceGoal>());
    case ReferenceTaskKind::OBJECTS:
      return WideningRelations::objectsStep(*task.as<ObjectsShareGoal>());
    case ReferenceTaskKind::WIDENS:
      return WideningRelations::widensStep(*task.as<WidensGoal>());
    case ReferenceTaskKind::FITS:
      return WideningRelations::fitsStep(*task.as<ReferenceFitsGoal>());
    case ReferenceTaskKind::FITS_WIDE:
      return WideningRelations::fitsWideStep(*task.as<FitsWideGoal>());
    case ReferenceTaskKind::WIDE_CHOICE:
      return WideningRelations::plannedStep({});
  }

  return WideningRelations::failedStep();
}

ReferenceStep WideningRelations::sameStep(const SameReferenceGoal &goal) {
  const Type *own = goal.from;
  const Type *wide = goal.to;
  while (own != wide && (wide->isOptional() || (own->isArray() && wide->isArray()))) {
    if (wide->isOptional() && !WideningRelations::isReference(wide->inner())) {
      return WideningRelations::failedStep();
    }
    if (wide->isOptional()) {
      own = own->nonOptional();
      wide = wide->inner();
    } else {
      own = own->inner();
      wide = wide->inner();
    }
  }
  if (own == wide) {
    return WideningRelations::plannedStep({});
  }
  if (own->isOptional() || (own->isUnion() && own->isTagged())) {
    return WideningRelations::failedStep();
  }
  if (own->isUnion()) {
    return WideningRelations::membersStep(own, wide);
  }

  return WideningRelations::plannedStep({ObjectsShareGoal{.from = own, .to = wide}});
}

bool WideningRelations::isReference(const Type *type) {
  return type->isObject() || type->isArray();
}

ReferenceStep WideningRelations::membersStep(const Type *own, const Type *wide) {
  auto needs = std::vector<ReferenceTask>();
  for (const Type *member : own->members()) {
    if (!member->isObject()) {
      return WideningRelations::failedStep();
    }
    if (member != wide) {
      needs.push_back(ObjectsShareGoal{.from = member, .to = wide});
    }
  }

  return WideningRelations::plannedStep(std::move(needs));
}

ReferenceStep WideningRelations::objectsStep(const ObjectsShareGoal &goal) {
  if (!WideningRelations::areObjects(goal.from, goal.to)) {
    return WideningRelations::failedStep();
  }
  if (goal.from->isSubtypeOf(goal.to) || WideningRelations::isSameShape(goal.from, goal.to)) {
    return WideningRelations::plannedStep({});
  }

  return WideningRelations::plannedStep({WidensGoal{.from = goal.from, .to = goal.to}});
}

bool WideningRelations::isSpecialShape(const Type *type) {
  bool isOpaque = type->isBytes() || type->isRuntimeObject();

  return type->isClass() || !type->overloads().empty() || isOpaque;
}

bool WideningRelations::isSameShape(const Type *from, const Type *to) {
  if (WideningRelations::isSpecialShape(from) || WideningRelations::isSpecialShape(to)) {
    return false;
  }
  if (from->isTuple() != to->isTuple() || from->fields().size() != to->fields().size()) {
    return false;
  }
  for (const Field &field : from->fields()) {
    const auto *other = to->field(field.name);
    if (other == nullptr || other->type != field.type) {
      return false;
    }
  }

  return true;
}

ReferenceStep WideningRelations::widensStep(const WidensGoal &goal) {
  if (!goal.from->isClass() || !goal.to->isClass()) {
    return WideningRelations::failedStep();
  }
  const ClassDecl *target = goal.to->classDecl();
  const ClassDecl *source = goal.from->classDecl();
  if (target == nullptr || source == nullptr || target->generic() == nullptr) {
    return WideningRelations::failedStep();
  }
  const auto *instance = GenericWidening::instanceOf(source, target->generic());
  if (instance == nullptr || instance == target ||
      instance->typeArgs.size() != target->typeArgs.size()) {
    return WideningRelations::failedStep();
  }
  auto needs = std::vector<ReferenceTask>();
  for (size_t index = 0; index < target->typeArgs.size(); ++index) {
    const Type *own = instance->typeArgs[index];
    const Type *wide = target->typeArgs[index];
    if (own != wide && !GenericWidening::isCovariant(target->generic(), index)) {
      return WideningRelations::failedStep();
    }
    if (own != wide) {
      needs.push_back(ReferenceFitsGoal{.own = own, .wide = wide});
    }
  }

  return WideningRelations::plannedStep(std::move(needs));
}

ReferenceStep WideningRelations::fitsStep(const ReferenceFitsGoal &goal) {
  const Type *own = goal.own;
  const Type *wide = goal.wide;
  if (own == wide || own->isNever()) {
    return WideningRelations::plannedStep({});
  }
  if (!own->isUnion() || own->isTagged()) {
    return WideningRelations::plannedStep({FitsWideGoal{.own = own, .wide = wide}});
  }
  auto needs = std::vector<ReferenceTask>();
  for (const Type *member : own->members()) {
    needs.push_back(FitsWideGoal{.own = member, .wide = wide});
  }

  return WideningRelations::plannedStep(std::move(needs));
}

ReferenceStep WideningRelations::fitsWideStep(const FitsWideGoal &goal) {
  const Type *own = goal.own;
  const Type *wide = goal.wide;
  if (own == wide || own->isNever()) {
    return WideningRelations::plannedStep({});
  }
  if (!wide->isUnion() || wide->isTagged()) {
    return WideningRelations::plannedStep({SameReferenceGoal{.from = own, .to = wide}});
  }
  if (llvm::is_contained(wide->members(), own)) {
    return WideningRelations::plannedStep({});
  }
  auto choice = WideMemberChoice{.own = own, .wide = wide, .next = 0};

  return WideningRelations::wideMemberTrial(choice);
}

ReferenceStep WideningRelations::wideMemberTrial(WideMemberChoice choice) {
  llvm::ArrayRef<const Type *> members = choice.wide->members();
  if (choice.next == members.size()) {
    return WideningRelations::failedStep();
  }
  const Type *member = members[choice.next];
  const Type *own = choice.own;
  choice.next += 1;

  return WideningRelations::plannedStep({SameReferenceGoal{.from = own, .to = member}, choice});
}

bool WideningRelations::retryWideMember(std::vector<ReferenceTask> &tasks) {
  while (!tasks.empty()) {
    ReferenceTask task = std::move(tasks.back());
    tasks.pop_back();
    const WideMemberChoice *choice = task.as<WideMemberChoice>();
    auto trial = choice != nullptr ? WideningRelations::wideMemberTrial(*choice)
                                   : WideningRelations::failedStep();
    if (trial.isPossible) {
      tasks.insert(tasks.end(), trial.needs.rbegin(), trial.needs.rend());
      return true;
    }
  }

  return false;
}

}
