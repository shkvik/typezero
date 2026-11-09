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
  const Type *own = from;
  const Type *wide = to;
  while (own != wide && (wide->isOptional() || (own->isArray() && wide->isArray()))) {
    if (wide->isOptional()) {
      const Type *inner = wide->inner();
      bool reference = inner->isObject() || inner->isArray();
      if (!reference) {
        return false;
      }
      own = own->nonOptional();
      wide = inner;
    } else {
      own = own->inner();
      wide = wide->inner();
    }
  }
  if (own == wide) {
    return true;
  }
  if (own->isOptional()) {
    return false;
  }
  if (own->isUnion()) {
    return !own->isTagged() && WideningRelations::membersShareReference(own, wide);
  }

  return WideningRelations::objectsShareReference(own, wide);
}

bool WideningRelations::membersShareReference(const Type *own, const Type *wide) {
  for (const Type *member : own->members()) {
    bool same = member->isObject() &&
                (member == wide || WideningRelations::objectsShareReference(member, wide));
    if (!same) {
      return false;
    }
  }

  return true;
}

bool WideningRelations::objectsShareReference(const Type *from, const Type *to) {
  bool objects = from->isObject() && to->isObject() && !from->isAnyObject() && !to->isAnyObject();
  if (!objects) {
    return false;
  }
  if (from->isSubtypeOf(to) || GenericWidening::widens(from, to)) {
    return true;
  }
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

}
