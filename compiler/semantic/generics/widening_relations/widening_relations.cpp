#include "compiler/semantic/generics/widening_relations/widening_relations.h"
#include "compiler/semantic/generics/generic_widening/generic_widening.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

WideningRelations::WideningRelations(SemanticState &owner)
    : state(owner) {}

void WideningRelations::collectNarrowings(const Type *from, const Type *to, int depth,
                                          std::vector<NarrowedPart> &out, TypePairs &seen) {
  std::vector<NarrowingStep> steps = {VisitNarrowing{from, to, depth}};
  while (!steps.empty()) {
    NarrowingStep step = steps.back();
    steps.pop_back();
    if (const VisitNarrowing *visit = step.as<VisitNarrowing>()) {
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
  bool firstVisit = seen.insert({from, to}).second;
  if (!firstVisit) {
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
    steps.push_back(VisitNarrowing{from->inner(), to->inner(), depth + 1});
    return;
  }
  if (from->isUnion()) {
    if (!llvm::is_contained(from->members(), to)) {
      for (const Type *member : llvm::reverse(from->members())) {
        steps.push_back(VisitNarrowing{member, to, depth});
      }
    }
    return;
  }
  bool objects = from->isObject() && to->isObject() && !from->isAnyObject() && !to->isAnyObject();
  if (objects) {
    WideningRelations::pushFieldNarrowings(from, to, depth, steps);
  }
}

void WideningRelations::pushFieldNarrowings(const Type *from, const Type *to, int depth,
                                            std::vector<NarrowingStep> &steps) {
  std::vector<NarrowingStep> fieldSteps;
  for (const Field &wide : to->fields()) {
    const Field *own = from->field(wide.name);
    if (!own) {
      continue;
    }
    if (own->readonly && !wide.readonly) {
      fieldSteps.push_back(EmitNarrowing{NarrowedPart{
          .depth = depth,
          .element = false,
          .name = wide.name,
          .narrow = nullptr,
          .owner = from,
      }});
    } else if (own->type != wide.type) {
      fieldSteps.push_back(EmitNarrowing{NarrowedPart{
          .depth = depth,
          .element = false,
          .name = wide.name,
          .narrow = own->type,
          .owner = from,
      }});
    }
    if (own->type != wide.type) {
      fieldSteps.push_back(VisitNarrowing{own->type, wide.type, depth + 1});
    }
  }
  steps.insert(steps.end(), fieldSteps.rbegin(), fieldSteps.rend());
}

std::vector<NarrowedPart> WideningRelations::narrowingsOf(const Type *from, const Type *to) {
  std::vector<NarrowedPart> out;
  TypePairs seen;
  WideningRelations::collectNarrowings(from, to, 0, out, seen);

  return out;
}

bool WideningRelations::widensWrites(const Type *from, const Type *to) {
  const Type *bareFrom = from->nonOptional();
  const Type *bareTo = to->nonOptional();
  if (bareFrom == bareTo || GenericWidening::widens(bareFrom, bareTo)) {
    return false;
  }
  bool arrays = bareFrom->isArray() && bareTo->isArray();
  bool objects = bareFrom->isObject() && bareTo->isObject() && !bareFrom->isAnyObject() &&
                 !bareTo->isAnyObject();
  bool unionToObject = bareFrom->isUnion() && bareTo->isObject();
  if (!arrays && !objects && !unionToObject) {
    return false;
  }
  WideningStore &widenings = this->state.getWideningStore();
  if (widenings.hasKnownWidening(bareFrom, bareTo)) {
    return widenings.isNarrowingWidening(bareFrom, bareTo);
  }
  std::vector<NarrowedPart> narrowings = WideningRelations::narrowingsOf(bareFrom, bareTo);
  bool narrowed = !narrowings.empty();
  widenings.rememberWidening(bareFrom, bareTo, narrowed);

  return narrowed;
}

bool WideningRelations::sameReference(const Type *from, const Type *to) {
  return WideningRelations::referenceHolds(SameReferenceGoal{from, to});
}

bool WideningRelations::referenceHolds(ReferenceTask goal) {
  std::vector<ReferenceTask> tasks = {std::move(goal)};
  while (!tasks.empty()) {
    ReferenceTask task = std::move(tasks.back());
    tasks.pop_back();
    ReferenceStep step = WideningRelations::referenceStep(task);
    if (step.possible) {
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
      return WideningRelations::sameStep(task.as<SameReferenceGoal>()->from,
                                         task.as<SameReferenceGoal>()->to);
    case ReferenceTaskKind::OBJECTS:
      return WideningRelations::objectsStep(task.as<ObjectsShareGoal>()->from,
                                            task.as<ObjectsShareGoal>()->to);
    case ReferenceTaskKind::WIDENS:
      return WideningRelations::widensStep(task.as<WidensGoal>()->from,
                                           task.as<WidensGoal>()->to);
    case ReferenceTaskKind::FITS:
      return WideningRelations::fitsStep(task.as<ReferenceFitsGoal>()->own,
                                         task.as<ReferenceFitsGoal>()->wide);
    case ReferenceTaskKind::FITS_WIDE:
      return WideningRelations::fitsWideStep(task.as<FitsWideGoal>()->own,
                                             task.as<FitsWideGoal>()->wide);
    case ReferenceTaskKind::WIDE_CHOICE:
      return ReferenceStep{true, {}};
  }
  llvm_unreachable("unknown reference task");
}

ReferenceStep WideningRelations::sameStep(const Type *from, const Type *to) {
  const Type *own = from;
  const Type *wide = to;
  while (own != wide && (wide->isOptional() || (own->isArray() && wide->isArray()))) {
    if (wide->isOptional()) {
      const Type *inner = wide->inner();
      bool reference = inner->isObject() || inner->isArray();
      if (!reference) {
        return ReferenceStep{false, {}};
      }
      own = own->nonOptional();
      wide = inner;
    } else {
      own = own->inner();
      wide = wide->inner();
    }
  }
  if (own == wide) {
    return ReferenceStep{true, {}};
  }
  if (own->isOptional() || (own->isUnion() && own->isTagged())) {
    return ReferenceStep{false, {}};
  }
  if (own->isUnion()) {
    return WideningRelations::membersStep(own, wide);
  }

  return ReferenceStep{true, {ObjectsShareGoal{own, wide}}};
}

ReferenceStep WideningRelations::membersStep(const Type *own, const Type *wide) {
  std::vector<ReferenceTask> needs;
  for (const Type *member : own->members()) {
    if (!member->isObject()) {
      return ReferenceStep{false, {}};
    }
    if (member != wide) {
      needs.push_back(ObjectsShareGoal{member, wide});
    }
  }

  return ReferenceStep{true, std::move(needs)};
}

ReferenceStep WideningRelations::objectsStep(const Type *from, const Type *to) {
  bool objects = from->isObject() && to->isObject() && !from->isAnyObject() && !to->isAnyObject();
  if (!objects) {
    return ReferenceStep{false, {}};
  }
  if (from->isSubtypeOf(to) || WideningRelations::sameShapeFields(from, to)) {
    return ReferenceStep{true, {}};
  }

  return ReferenceStep{true, {WidensGoal{from, to}}};
}

bool WideningRelations::sameShapeFields(const Type *from, const Type *to) {
  bool otherShape = from->isClass() || to->isClass() || from->isTuple() != to->isTuple() ||
                    from->fields().size() != to->fields().size() || !from->overloads().empty() ||
                    !to->overloads().empty() || from->isBytes() || to->isBytes() ||
                    from->isRuntimeObject() || to->isRuntimeObject();
  if (otherShape) {
    return false;
  }
  auto sameField = [&](const Field &field) {
    const Field *other = to->field(field.name);
    return other && other->type == field.type;
  };

  return llvm::all_of(from->fields(), sameField);
}

ReferenceStep WideningRelations::widensStep(const Type *from, const Type *to) {
  if (!from->isClass() || !to->isClass()) {
    return ReferenceStep{false, {}};
  }
  const ClassDecl *target = to->classDecl();
  const ClassDecl *source = from->classDecl();
  if (!target || !source || !target->generic()) {
    return ReferenceStep{false, {}};
  }
  const ClassDecl *instance = GenericWidening::instanceOf(source, target->generic());
  if (!instance || instance == target || instance->typeArgs.size() != target->typeArgs.size()) {
    return ReferenceStep{false, {}};
  }
  std::vector<ReferenceTask> needs;
  for (size_t i = 0; i < target->typeArgs.size(); ++i) {
    const Type *own = instance->typeArgs[i];
    const Type *wide = target->typeArgs[i];
    if (own == wide) {
      continue;
    }
    if (!GenericWidening::covariant(target->generic(), i)) {
      return ReferenceStep{false, {}};
    }
    needs.push_back(ReferenceFitsGoal{own, wide});
  }

  return ReferenceStep{true, std::move(needs)};
}

ReferenceStep WideningRelations::fitsStep(const Type *own, const Type *wide) {
  if (own == wide || own->isNever()) {
    return ReferenceStep{true, {}};
  }
  if (!own->isUnion() || own->isTagged()) {
    return ReferenceStep{true, {FitsWideGoal{own, wide}}};
  }
  std::vector<ReferenceTask> needs;
  for (const Type *member : own->members()) {
    needs.push_back(FitsWideGoal{member, wide});
  }

  return ReferenceStep{true, std::move(needs)};
}

ReferenceStep WideningRelations::fitsWideStep(const Type *own, const Type *wide) {
  if (own == wide || own->isNever()) {
    return ReferenceStep{true, {}};
  }
  if (!wide->isUnion() || wide->isTagged()) {
    return ReferenceStep{true, {SameReferenceGoal{own, wide}}};
  }
  if (llvm::is_contained(wide->members(), own)) {
    return ReferenceStep{true, {}};
  }

  return WideningRelations::wideMemberTrial(WideMemberChoice{own, wide, 0});
}

ReferenceStep WideningRelations::wideMemberTrial(WideMemberChoice choice) {
  llvm::ArrayRef<const Type *> members = choice.wide->members();
  if (choice.next == members.size()) {
    return ReferenceStep{false, {}};
  }
  const Type *member = members[choice.next];
  const Type *own = choice.own;
  choice.next += 1;

  return ReferenceStep{true, {SameReferenceGoal{own, member}, choice}};
}

bool WideningRelations::retryWideMember(std::vector<ReferenceTask> &tasks) {
  while (!tasks.empty()) {
    ReferenceTask task = std::move(tasks.back());
    tasks.pop_back();
    const WideMemberChoice *choice = task.as<WideMemberChoice>();
    if (choice == nullptr) {
      continue;
    }
    ReferenceStep trial = WideningRelations::wideMemberTrial(*choice);
    if (trial.possible) {
      tasks.insert(tasks.end(), trial.needs.rbegin(), trial.needs.rend());
      return true;
    }
  }

  return false;
}

}
