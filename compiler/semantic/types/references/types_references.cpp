#include "compiler/semantic/types/references/types_references.h"
#include "compiler/semantic/semantic_state/types/type_storage.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

namespace {

constexpr int kMaxDepth = 6;

}

TypesReferences::TypesReferences(TypeStorage &owner)
    : types(owner) {}

ReferenceStep TypesReferences::failedStep() {
  return ReferenceStep{.isPossible = false, .needs = {}};
}

ReferenceStep TypesReferences::plannedStep(std::vector<ReferenceTask> needs) {
  return ReferenceStep{.isPossible = true, .needs = std::move(needs)};
}

bool TypesReferences::areObjects(const Type *from, const Type *to) {
  bool isPlainFrom = from->isObject() && !from->isAnyObject();
  bool isPlainTo = to->isObject() && !to->isAnyObject();

  return isPlainFrom && isPlainTo;
}

void TypesReferences::collectNarrowings(const Type *from, const Type *to, int depth,
                                        std::vector<NarrowedPart> &out, TypePairs &seen) {
  std::vector<NarrowingStep> steps = {VisitNarrowing{.from = from, .to = to, .depth = depth}};
  while (!steps.empty()) {
    NarrowingStep step = steps.back();
    steps.pop_back();
    const VisitNarrowing *visit = step.as<VisitNarrowing>();
    if (visit != nullptr) {
      this->visitNarrowing(*visit, out, seen, steps);
    } else {
      out.push_back(step.as<EmitNarrowing>()->part);
    }
  }
}

void TypesReferences::visitNarrowing(const VisitNarrowing &visit, std::vector<NarrowedPart> &out,
                                     TypePairs &seen, std::vector<NarrowingStep> &steps) {
  const Type *from = visit.from->nonOptional();
  const Type *to = visit.to->nonOptional();
  int depth = visit.depth;
  if (from == to || depth > kMaxDepth) {
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
  if (!from->isUnion() && this->areObjects(from, to)) {
    this->pushFieldNarrowings(from, to, depth, steps);
  }
}

void TypesReferences::pushFieldNarrowings(const Type *from, const Type *to, int depth,
                                          std::vector<NarrowingStep> &steps) {
  auto fieldSteps = std::vector<NarrowingStep>();
  for (const Field &wide : to->fields()) {
    const auto *own = from->field(wide.name);
    if (own != nullptr) {
      this->addFieldSteps(from, *own, wide, depth, fieldSteps);
    }
  }
  steps.insert(steps.end(), fieldSteps.rbegin(), fieldSteps.rend());
}

void TypesReferences::addFieldSteps(const Type *owner, const Field &own, const Field &wide,
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

std::vector<NarrowedPart> TypesReferences::narrowingsOf(const Type *from, const Type *to) {
  auto out = std::vector<NarrowedPart>();
  TypePairs seen;
  this->collectNarrowings(from, to, 0, out, seen);

  return out;
}

bool TypesReferences::isSameReference(const Type *from, const Type *to) {
  return this->isReferenceHolding(SameReferenceGoal{.from = from, .to = to});
}

bool TypesReferences::isReferenceHolding(ReferenceTask goal) {
  std::vector<ReferenceTask> tasks = {std::move(goal)};
  while (!tasks.empty()) {
    ReferenceTask task = std::move(tasks.back());
    tasks.pop_back();
    auto step = this->referenceStep(task);
    if (step.isPossible) {
      tasks.insert(tasks.end(), step.needs.rbegin(), step.needs.rend());
    } else if (!this->retryWideMember(tasks)) {
      return false;
    }
  }

  return true;
}

ReferenceStep TypesReferences::referenceStep(const ReferenceTask &task) {
  switch (task.kind()) {
    case ReferenceTaskKind::SAME:
      return this->sameStep(*task.as<SameReferenceGoal>());
    case ReferenceTaskKind::OBJECTS:
      return this->objectsStep(*task.as<ObjectsShareGoal>());
    case ReferenceTaskKind::WIDENS:
      return this->widensStep(*task.as<WidensGoal>());
    case ReferenceTaskKind::FITS:
      return this->fitsStep(*task.as<ReferenceFitsGoal>());
    case ReferenceTaskKind::FITS_WIDE:
      return this->fitsWideStep(*task.as<FitsWideGoal>());
    case ReferenceTaskKind::WIDE_CHOICE:
      return this->plannedStep({});
  }

  return this->failedStep();
}

ReferenceStep TypesReferences::sameStep(const SameReferenceGoal &goal) {
  const Type *own = goal.from;
  const Type *wide = goal.to;
  while (own != wide && (wide->isOptional() || (own->isArray() && wide->isArray()))) {
    if (wide->isOptional() && !this->isReference(wide->inner())) {
      return this->failedStep();
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
    return this->plannedStep({});
  }
  if (own->isOptional() || (own->isUnion() && own->isTagged())) {
    return this->failedStep();
  }
  if (own->isUnion()) {
    return this->membersStep(own, wide);
  }

  return this->plannedStep({ObjectsShareGoal{.from = own, .to = wide}});
}

bool TypesReferences::isReference(const Type *type) {
  return type->isObject() || type->isArray();
}

ReferenceStep TypesReferences::membersStep(const Type *own, const Type *wide) {
  auto needs = std::vector<ReferenceTask>();
  for (const Type *member : own->members()) {
    if (!member->isObject()) {
      return this->failedStep();
    }
    if (member != wide) {
      needs.push_back(ObjectsShareGoal{.from = member, .to = wide});
    }
  }

  return this->plannedStep(std::move(needs));
}

ReferenceStep TypesReferences::objectsStep(const ObjectsShareGoal &goal) {
  if (!this->areObjects(goal.from, goal.to)) {
    return this->failedStep();
  }
  if (goal.from->isSubtypeOf(goal.to) || this->isSameShape(goal.from, goal.to)) {
    return this->plannedStep({});
  }

  return this->plannedStep({WidensGoal{.from = goal.from, .to = goal.to}});
}

bool TypesReferences::isSpecialShape(const Type *type) {
  bool isOpaque = type->isBytes() || type->isRuntimeObject();

  return type->isClass() || !type->overloads().empty() || isOpaque;
}

bool TypesReferences::isSameShape(const Type *from, const Type *to) {
  if (this->isSpecialShape(from) || this->isSpecialShape(to)) {
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

ReferenceStep TypesReferences::widensStep(const WidensGoal &goal) {
  if (!goal.from->isClass() || !goal.to->isClass()) {
    return this->failedStep();
  }
  const ClassDecl *target = goal.to->classDecl();
  const ClassDecl *source = goal.from->classDecl();
  if (target == nullptr || source == nullptr || target->generic() == nullptr) {
    return this->failedStep();
  }
  const auto *instance = this->types.variance.instanceOf(source, target->generic());
  if (instance == nullptr || instance == target ||
      instance->typeArgs.size() != target->typeArgs.size()) {
    return this->failedStep();
  }
  auto needs = std::vector<ReferenceTask>();
  for (size_t index = 0; index < target->typeArgs.size(); ++index) {
    const Type *own = instance->typeArgs[index];
    const Type *wide = target->typeArgs[index];
    if (own != wide && !this->types.variance.isCovariant(target->generic(), index)) {
      return this->failedStep();
    }
    if (own != wide) {
      needs.push_back(ReferenceFitsGoal{.own = own, .wide = wide});
    }
  }

  return this->plannedStep(std::move(needs));
}

ReferenceStep TypesReferences::fitsStep(const ReferenceFitsGoal &goal) {
  const Type *own = goal.own;
  const Type *wide = goal.wide;
  if (own == wide || own->isNever()) {
    return this->plannedStep({});
  }
  if (!own->isUnion() || own->isTagged()) {
    return this->plannedStep({FitsWideGoal{.own = own, .wide = wide}});
  }
  auto needs = std::vector<ReferenceTask>();
  for (const Type *member : own->members()) {
    needs.push_back(FitsWideGoal{.own = member, .wide = wide});
  }

  return this->plannedStep(std::move(needs));
}

ReferenceStep TypesReferences::fitsWideStep(const FitsWideGoal &goal) {
  const Type *own = goal.own;
  const Type *wide = goal.wide;
  if (own == wide || own->isNever()) {
    return this->plannedStep({});
  }
  if (!wide->isUnion() || wide->isTagged()) {
    return this->plannedStep({SameReferenceGoal{.from = own, .to = wide}});
  }
  if (llvm::is_contained(wide->members(), own)) {
    return this->plannedStep({});
  }
  auto choice = WideMemberChoice{.own = own, .wide = wide, .next = 0};

  return this->wideMemberTrial(choice);
}

ReferenceStep TypesReferences::wideMemberTrial(WideMemberChoice choice) {
  llvm::ArrayRef<const Type *> members = choice.wide->members();
  if (choice.next == members.size()) {
    return this->failedStep();
  }
  const Type *member = members[choice.next];
  const Type *own = choice.own;
  choice.next += 1;

  return this->plannedStep({SameReferenceGoal{.from = own, .to = member}, choice});
}

bool TypesReferences::retryWideMember(std::vector<ReferenceTask> &tasks) {
  while (!tasks.empty()) {
    ReferenceTask task = std::move(tasks.back());
    tasks.pop_back();
    const WideMemberChoice *choice = task.as<WideMemberChoice>();
    auto trial = choice != nullptr ? this->wideMemberTrial(*choice) : this->failedStep();
    if (trial.isPossible) {
      tasks.insert(tasks.end(), trial.needs.rbegin(), trial.needs.rend());
      return true;
    }
  }

  return false;
}

}
