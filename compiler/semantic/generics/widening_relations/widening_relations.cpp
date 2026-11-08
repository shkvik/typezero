#include "compiler/semantic/generics/widening_relations/widening_relations.h"
#include "compiler/semantic/generics/generic_widening/generic_widening.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

WideningRelations::WideningRelations(SemanticState &owner)
    : state(owner) {}

void WideningRelations::collectNarrowings(const Type *from, const Type *to, int depth,
                                          std::vector<NarrowedPart> &out, TypePairs &seen) {
  from = from->nonOptional();
  to = to->nonOptional();
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
    WideningRelations::collectNarrowings(from->inner(), to->inner(), depth + 1, out, seen);
    return;
  }
  if (from->isUnion()) {
    if (llvm::is_contained(from->members(), to)) {
      return;
    }
    for (const Type *member : from->members()) {
      WideningRelations::collectNarrowings(member, to, depth, out, seen);
    }
    return;
  }
  bool objects = from->isObject() && to->isObject() && !from->isAnyObject() && !to->isAnyObject();
  if (!objects) {
    return;
  }
  for (const Field &wide : to->fields()) {
    const Field *own = from->field(wide.name);
    if (!own) {
      continue;
    }
    if (own->readonly && !wide.readonly) {
      out.push_back(NarrowedPart{
          .depth = depth,
          .element = false,
          .name = wide.name,
          .narrow = nullptr,
          .owner = from,
      });
    } else if (own->type != wide.type) {
      out.push_back(NarrowedPart{
          .depth = depth,
          .element = false,
          .name = wide.name,
          .narrow = own->type,
          .owner = from,
      });
    }
    if (own->type != wide.type) {
      WideningRelations::collectNarrowings(own->type, wide.type, depth + 1, out, seen);
    }
  }
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
  if (from == to) {
    return true;
  }
  if (to->isOptional()) {
    const Type *inner = to->inner();
    bool reference = inner->isObject() || inner->isArray();
    return reference && WideningRelations::sameReference(from->nonOptional(), inner);
  }
  if (from->isOptional()) {
    return false;
  }
  if (from->isArray() && to->isArray()) {
    return WideningRelations::sameReference(from->inner(), to->inner());
  }
  auto sameObject = [&](const Type *member) {
    return member->isObject() && WideningRelations::sameReference(member, to);
  };
  if (from->isUnion()) {
    return !from->isTagged() && llvm::all_of(from->members(), sameObject);
  }
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
