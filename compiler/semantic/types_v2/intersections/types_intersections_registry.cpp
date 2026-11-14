#include "compiler/semantic/types_v2/intersections/types_intersections_registry.h"
#include "llvm/ADT/STLExtras.h"
#include <cmath>

namespace tz {

TypesIntersectionsRegistry::TypesIntersectionsRegistry(SemanticState &owner)
    : state(owner) {}

llvm::ArrayRef<const Type *> TypesIntersectionsRegistry::membersOf(const Type *&type) const {
  return type->isUnion() ? type->members() : llvm::ArrayRef<const Type *>(type);
}

void TypesIntersectionsRegistry::startIntersection(const StartIntersection &task) {
  auto draft = IntersectionDraft{
      .ref = task.ref,
      .next = 0,
      .parts = {},
  };

  return this->nextIntersectionPart(NextIntersectionPart{.draft = draft});
}

void TypesIntersectionsRegistry::nextIntersectionPart(const NextIntersectionPart &task) {
  const IntersectionDraft &draft = task.draft;
  const TypeRef &ref = *draft.ref;
  if (draft.next == ref.args.size()) {
    return this->finishIntersection(draft);
  }
  this->state.walk.tasks.pushTask(AfterIntersectionPart{.draft = draft});
  this->state.walk.tasks.pushTask(
      ResolveTypeAnswer{.ref = &ref.args[draft.next], .use = TypeUse::INNER});
}

void TypesIntersectionsRegistry::afterIntersectionPart(const AfterIntersectionPart &task) {
  IntersectionDraft draft = task.draft;
  const Type *part = this->state.walk.tasks.takeAnswer();
  if (part->isError()) {
    return this->state.walk.tasks.pushAnswer(part);
  }
  draft.parts.push_back(part);
  draft.next += 1;

  return this->nextIntersectionPart(NextIntersectionPart{.draft = draft});
}

void TypesIntersectionsRegistry::finishIntersection(const IntersectionDraft &draft) {
  const std::vector<const Type *> &parts = draft.parts;
  if (parts.size() == 2) {
    for (size_t index : {0, 1}) {
      if (this->isFilteringBy(parts[index])) {
        return this->pushFilter(parts[index], parts[1 - index]);
      }
    }
  }
  auto joined = this->joinedParts(parts);
  if (joined.isErr()) {
    this->state.diagnostics.error(draft.ref->loc, joined.error().message);

    return this->state.walk.tasks.pushAnswer(Type::error());
  }
  this->state.walk.tasks.pushAnswer(joined.value());
}

bool TypesIntersectionsRegistry::isFilteringBy(const Type *filter) const {
  return this->isPrimitive(filter) || this->isEmptyObject(filter);
}

void TypesIntersectionsRegistry::pushFilter(const Type *filter, const Type *other) {
  if (this->isPrimitive(filter)) {
    const auto *unevaluated = Type::typeOp(TypeOp::Extract, {other, filter});

    return this->state.walk.tasks.pushTask(
        EvalTypeOp{.op = unevaluated, .operands = {other, filter}});
  }
  const auto *unevaluated = Type::typeOp(TypeOp::NonNullable, {other});
  this->state.walk.tasks.pushTask(EvalTypeOp{.op = unevaluated, .operands = {other}});
}

TypesIntersectionsRegistry::ResultIntersectionJoined
TypesIntersectionsRegistry::joinedParts(const std::vector<const Type *> &parts) const {
  auto objects = this->checkAllObjects(parts);
  if (objects.isErr()) {
    return err(objects.error());
  }
  auto merged = this->fieldCopies(parts.front());
  for (const Type *part : llvm::ArrayRef<const Type *>(parts).drop_front()) {
    auto next = this->mergedWith(merged, part);
    if (next.isErr()) {
      return err(next.error());
    }
    merged = next.value();
  }
  const auto *joined = Type::unionOf(merged);

  return ok(joined);
}

bool TypesIntersectionsRegistry::isPrimitive(const Type *type) const {
  return (type->isString() && !type->isPattern()) || type->isNumeric() || type->isBool();
}

bool TypesIntersectionsRegistry::isPlainObject(const Type *type) const {
  return type->isObject() && !type->isClass() && !type->isTuple() && !type->isAnyObject() &&
         type->overloads().empty();
}

bool TypesIntersectionsRegistry::isEmptyObject(const Type *type) const {
  return this->isPlainObject(type) && type->fields().empty();
}

TypesIntersectionsRegistry::ResultObjectsAgreed
TypesIntersectionsRegistry::checkAllObjects(llvm::ArrayRef<const Type *> parts) const {
  const Type *other = this->firstNonObject(parts);
  if (other != nullptr) {
    auto message = ErrorTextIntersection::notObject(other->name());

    return err(TypesIntersectionsRegistryResolveIntersectionError{
        .code = TypesIntersectionsRegistryResolveIntersectionErrorCode::NOT_AN_OBJECT,
        .message = message,
    });
  }

  return ok();
}

std::vector<const Type *> TypesIntersectionsRegistry::fieldCopies(const Type *part) const {
  auto copies = std::vector<const Type *>();
  for (const Type *member : this->membersOf(part)) {
    auto fields = std::vector<Field>(member->fields().begin(), member->fields().end());
    copies.push_back(Type::object(std::move(fields)));
  }

  return copies;
}

const Type *TypesIntersectionsRegistry::firstNonObject(llvm::ArrayRef<const Type *> parts) const {
  for (const Type *part : parts) {
    for (const Type *member : this->membersOf(part)) {
      if (!this->isPlainObject(member)) {
        return member;
      }
    }
  }

  return nullptr;
}

TypesIntersectionsRegistry::ResultObjectsMerged
TypesIntersectionsRegistry::mergedWith(llvm::ArrayRef<const Type *> merged,
                                       const Type *part) const {
  auto next = std::vector<const Type *>();
  for (const Type *left : merged) {
    for (const Type *right : this->membersOf(part)) {
      auto joined = this->joinObjects(left, right);
      if (joined.isErr()) {
        return err(joined.error());
      }
      next.push_back(joined.value());
    }
  }

  return ok(next);
}

TypesIntersectionsRegistry::ResultIntersectionJoined
TypesIntersectionsRegistry::joinObjects(const Type *left, const Type *right) const {
  auto fields = std::vector<Field>(left->fields().begin(), left->fields().end());
  for (const Field &field : right->fields()) {
    auto added = this->joinField(fields, field);
    if (added.isErr()) {
      return err(added.error());
    }
  }
  const auto *object = Type::object(std::move(fields));

  return ok(object);
}

TypesIntersectionsRegistry::ResultObjectsAgreed
TypesIntersectionsRegistry::joinField(std::vector<Field> &fields, const Field &field) const {
  Field *same = this->fieldNamed(fields, field.name);
  if (same == nullptr) {
    fields.push_back(field);

    return ok();
  }
  const Type *first = same->type->nonOptional();
  const Type *second = field.type->nonOptional();
  const Type *both = this->commonFieldType(first, second);
  if (both == nullptr) {
    auto message = ErrorTextIntersection::fieldTypes(field.name, first->name(), second->name());

    return err(TypesIntersectionsRegistryResolveIntersectionError{
        .code = TypesIntersectionsRegistryResolveIntersectionErrorCode::FIELD_TYPES_DIFFER,
        .message = message,
    });
  }
  bool isOptional = same->type->isOptional() && field.type->isOptional();
  same->type = isOptional ? Type::optional(both) : both;
  same->readonly |= field.readonly;

  return ok();
}

Field *TypesIntersectionsRegistry::fieldNamed(std::vector<Field> &fields,
                                              llvm::StringRef name) const {
  for (Field &field : fields) {
    if (field.name == name) {
      return &field;
    }
  }

  return nullptr;
}

const Type *TypesIntersectionsRegistry::commonFieldType(const Type *first,
                                                        const Type *second) const {
  if (first == second) {
    return first;
  }
  if (first->isLiteral() && first->literalBase() == second) {
    return first;
  }
  if (second->isLiteral() && second->literalBase() == first) {
    return second;
  }

  return nullptr;
}

}
