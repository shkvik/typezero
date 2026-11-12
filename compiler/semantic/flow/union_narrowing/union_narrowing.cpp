#include "compiler/semantic/flow/union_narrowing/union_narrowing.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/flow/field_facts/field_facts.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/literal_types/literal_types.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "compiler/semantic/types/union_check_helpers/union_check_helpers.h"
#include "llvm/Support/ConvertUTF.h"
#include <functional>

using llvm::dyn_cast;
using llvm::dyn_cast_or_null;

namespace tz {

UnionNarrowing::UnionNarrowing(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->fieldFacts = &all.fieldFacts;
  this->literalTypes = &all.literalTypes;
}

llvm::SmallVector<const Type *, 8> UnionNarrowing::membersOf(const Type *type) const {
  if (type->isUnion()) {
    auto members = type->members();

    return llvm::SmallVector<const Type *, 8>(members.begin(), members.end());
  }

  return {type};
}

const Expr *UnionNarrowing::withoutReadCasts(const Expr *expr) const {
  const Expr *read = expr;
  auto *conversion = dyn_cast<CastExpr>(read);
  while (conversion != nullptr) {
    if (!conversion->isImplicit() || conversion->shortCircuit) {
      return nullptr;
    }
    read = conversion->operand;
    conversion = dyn_cast<CastExpr>(read);
  }

  return read;
}

const VarSymbol *UnionNarrowing::readVariable(const Expr *expr) const {
  const auto *read = this->withoutReadCasts(expr);
  auto *name = dyn_cast_or_null<NameExpr>(read);

  return name != nullptr ? name->var() : nullptr;
}

const Type *UnionNarrowing::keptType(const Type *type,
                                     llvm::function_ref<bool(const Type *)> keep) const {
  bool isUndefined = type->isOptional();
  const Type *value = type->nonOptional();
  auto kept = std::vector<const Type *>();
  for (const Type *member : this->membersOf(value)) {
    if (keep(member)) {
      kept.push_back(member);
    }
  }
  bool isKeepingUndefined = isUndefined && keep(Type::undefinedTy());
  if (kept.empty()) {
    return isKeepingUndefined ? type : nullptr;
  }
  const auto *result = Type::unionOf(kept);

  return isKeepingUndefined ? Type::optional(result) : result;
}

const Type *UnionNarrowing::keptUnion(const std::vector<const Type *> &kept) const {
  if (kept.empty()) {
    return nullptr;
  }

  return Type::unionOf(kept);
}

bool UnionNarrowing::isNarrowingOf(
    const VarSymbol *variable, const std::pair<const VarSymbol *, const Type *> &narrowed) const {
  return narrowed.first == variable;
}

void UnionNarrowing::narrowVariable(Flow &flow, const VarSymbol *variable, const Type *type) const {
  llvm::erase_if(flow.narrowed, std::bind_front(&UnionNarrowing::isNarrowingOf, this, variable));
  if (type == nullptr) {
    flow.narrowed.push_back({variable, Type::never()});
    return;
  }
  if (type == variable->type) {
    return;
  }
  if (variable->type->isOptional() && type == variable->type->inner()) {
    flow.defined.insert(variable);
    return;
  }
  flow.narrowed.push_back({variable, type});
  if (!type->isOptional()) {
    flow.defined.insert(variable);
  }
}

std::optional<NarrowTarget> UnionNarrowing::narrowTarget(const Expr *expr) const {
  auto target = this->readTarget(expr);
  if (!target.has_value()) {
    return std::nullopt;
  }
  const Type *base = target->declared->nonOptional();
  if (!base->isUnion() && !base->isLiteral() && !base->isBool()) {
    return std::nullopt;
  }

  return target;
}

std::optional<NarrowTarget> UnionNarrowing::readTarget(const Expr *expr) const {
  const auto *variable = this->readVariable(expr);
  if (variable != nullptr && variable->type != nullptr) {
    return NarrowTarget{.var = variable, .declared = variable->type};
  }

  return this->fieldTarget(expr);
}

std::optional<NarrowTarget> UnionNarrowing::fieldTarget(const Expr *expr) const {
  const auto *read = this->withoutReadCasts(expr);
  auto *member = dyn_cast_or_null<MemberExpr>(read);
  if (member == nullptr) {
    return std::nullopt;
  }
  auto path = this->fieldFacts->fieldPath(member);
  if (!path.has_value()) {
    return std::nullopt;
  }

  return NarrowTarget{.path = std::move(*path), .declared = member->type};
}

const Type *UnionNarrowing::targetType(const Flow &flow, const NarrowTarget &target) const {
  if (target.var != nullptr) {
    return this->literalTypes->typeIn(flow, target.var);
  }
  for (const auto &[path, type] : flow.narrowedFields) {
    if (path == *target.path) {
      return type;
    }
  }
  if (target.declared->isOptional() && llvm::is_contained(flow.definedFields, *target.path)) {
    return target.declared->inner();
  }

  return target.declared;
}

bool UnionNarrowing::isPathEntry(const FieldPath &path,
                                 const std::pair<FieldPath, const Type *> &entry) const {
  return entry.first == path;
}

void UnionNarrowing::narrowTargetTo(Flow &flow, const NarrowTarget &target,
                                    const Type *type) const {
  if (target.var != nullptr) {
    this->narrowVariable(flow, target.var, type);
    return;
  }
  const FieldPath &path = *target.path;
  auto isEntry = std::bind_front(&UnionNarrowing::isPathEntry, this, std::cref(path));
  llvm::erase_if(flow.narrowedFields, isEntry);
  if (type != nullptr && !type->isOptional()) {
    this->fieldFacts->defineField(flow, path);
  }
  bool isDefinedDeclared =
      type != nullptr && target.declared->isOptional() && type == target.declared->inner();
  if (type == target.declared || isDefinedDeclared) {
    return;
  }
  const Type *narrowed = type != nullptr ? type : Type::never();
  flow.narrowedFields.push_back({path, narrowed});
}

void UnionNarrowing::addUnionFacts(const Expr *cond, Flow &whenTrue, Flow &whenFalse) const {
  auto *call = dyn_cast<CallExpr>(cond);
  auto *member = dyn_cast<MemberExpr>(cond);
  auto *binary = dyn_cast<BinaryExpr>(cond);
  if (call != nullptr && call->builtin() == Builtin::ArrayIsArray) {
    this->addArrayFacts(call->args[0], whenTrue, whenFalse);
    return;
  }
  if (member != nullptr) {
    this->addTruthinessFacts(member, whenTrue, whenFalse);
    return;
  }
  if (binary != nullptr) {
    this->addBinaryFacts(binary, whenTrue, whenFalse);
  }
}

void UnionNarrowing::addTruthinessFacts(const MemberExpr *member, Flow &whenTrue,
                                        Flow &whenFalse) const {
  auto canBeTruthy = std::bind_front(&UnionNarrowing::canBeTruthy, this);
  auto canBeFalsy = std::bind_front(&UnionNarrowing::canBeFalsy, this);
  this->narrowByTag(member, whenTrue, canBeTruthy, UndefinedTag::KEPT);
  this->narrowByTag(member, whenFalse, canBeFalsy, UndefinedTag::KEPT);
}

void UnionNarrowing::addBinaryFacts(const BinaryExpr *binary, Flow &whenTrue,
                                    Flow &whenFalse) const {
  bool isEquality = binary->op == BinaryOp::Eq || binary->op == BinaryOp::Ne;
  if (binary->op == BinaryOp::InstanceOf) {
    this->addInstanceOfFacts(binary, whenTrue, whenFalse);
    return;
  }
  if (binary->op == BinaryOp::In) {
    this->addInFacts(binary, whenTrue, whenFalse);
    return;
  }
  if (!isEquality) {
    return;
  }
  Flow &equal = binary->op == BinaryOp::Eq ? whenTrue : whenFalse;
  Flow &different = binary->op == BinaryOp::Eq ? whenFalse : whenTrue;
  this->addEqualityFacts(binary->lhs, binary->rhs, equal, different);
  this->addEqualityFacts(binary->rhs, binary->lhs, equal, different);
}

void UnionNarrowing::narrowKept(Flow &flow, const NarrowTarget &target,
                                llvm::function_ref<bool(const Type *)> keep) const {
  const auto *now = this->targetType(flow, target);
  if (now->isNever()) {
    return;
  }
  const auto *kept = this->keptType(now, keep);
  this->narrowTargetTo(flow, target, kept);
}

const Type *UnionNarrowing::tagOf(const Type *member, llvm::StringRef name) const {
  if (!member->isObject()) {
    return nullptr;
  }
  const Field *field = member->field(name);

  return field != nullptr ? field->type : nullptr;
}

bool UnionNarrowing::areAllTagged(const Type *now, llvm::StringRef name) const {
  for (const Type *member : this->membersOf(now)) {
    if (this->tagOf(member, name) == nullptr) {
      return false;
    }
  }

  return true;
}

const Expr *UnionNarrowing::taggedObject(const MemberExpr *member) const {
  if (!member->optional) {
    return member->object;
  }
  auto *link = dyn_cast<CastExpr>(member->object);
  if (link == nullptr || !link->shortCircuit) {
    return nullptr;
  }

  return link->operand;
}

std::optional<NarrowTarget> UnionNarrowing::taggedTarget(const MemberExpr *member,
                                                         const Flow &flow) const {
  const auto *object = this->taggedObject(member);
  if (object == nullptr) {
    return std::nullopt;
  }
  auto target = this->narrowTarget(object);
  if (!target.has_value() || !target->declared->nonOptional()->isUnion()) {
    return std::nullopt;
  }
  const Type *now = this->targetType(flow, *target)->nonOptional();
  bool isTaggable = (now->isUnion() || now->isObject()) && this->areAllTagged(now, member->name);
  if (!isTaggable) {
    return std::nullopt;
  }

  return target;
}

bool UnionNarrowing::isTagNarrowable(const MemberExpr *member, const Flow &flow) const {
  auto target = this->taggedTarget(member, flow);

  return target.has_value();
}

void UnionNarrowing::narrowByTag(const MemberExpr *member, Flow &flow,
                                 llvm::function_ref<bool(const Type *)> keep,
                                 UndefinedTag undefinedTag) const {
  auto target = this->taggedTarget(member, flow);
  if (!target.has_value()) {
    return;
  }
  auto isKept = std::bind_front(&UnionNarrowing::isTagKept, this, member->name, keep, undefinedTag);
  this->narrowKept(flow, *target, isKept);
}

bool UnionNarrowing::isTagKept(llvm::StringRef name, llvm::function_ref<bool(const Type *)> keep,
                               UndefinedTag undefinedTag, const Type *member) const {
  if (member->isUndefined()) {
    return undefinedTag == UndefinedTag::KEPT;
  }
  const auto *tag = this->tagOf(member, name);

  return keep(tag);
}

bool UnionNarrowing::isTruthyLiteral(const Type *type) const {
  if (type->isStringLiteral()) {
    return !type->literalText().empty();
  }

  return type->literalNumber() != 0;
}

bool UnionNarrowing::canBeTruthy(const Type *type) const {
  return !type->isLiteral() || this->isTruthyLiteral(type);
}

bool UnionNarrowing::canBeFalsy(const Type *type) const {
  return !type->isLiteral() || !this->isTruthyLiteral(type);
}

bool UnionNarrowing::isArrayMember(const Type *member) const {
  return member->isArray();
}

bool UnionNarrowing::isNotArrayMember(const Type *member) const {
  return !member->isArray();
}

bool UnionNarrowing::isNotBufferMember(const Type *member) const {
  return !member->isBuffer();
}

void UnionNarrowing::addArrayFacts(const Expr *arg, Flow &whenTrue, Flow &whenFalse) const {
  auto target = this->narrowTarget(arg);
  if (!target.has_value()) {
    return;
  }
  auto isArray = std::bind_front(&UnionNarrowing::isArrayMember, this);
  auto isNotArray = std::bind_front(&UnionNarrowing::isNotArrayMember, this);
  this->narrowKept(whenTrue, *target, isArray);
  this->narrowKept(whenFalse, *target, isNotArray);
}

void UnionNarrowing::addInstanceOfFacts(const BinaryExpr *binary, Flow &whenTrue,
                                        Flow &whenFalse) const {
  const Type *written = binary->rhs->type;
  auto target = this->narrowTarget(binary->lhs);
  if (!target.has_value() || written == nullptr) {
    return;
  }
  bool isConstructor = written->isConstructor();
  const Type *classType = isConstructor ? written->constructed() : written;
  if (isConstructor && !classType->isClass()) {
    return;
  }
  if (classType->isBuffer()) {
    this->addBufferFacts(*target, classType, whenTrue, whenFalse);
    return;
  }
  if (!classType->isClass()) {
    this->addMemberSetFacts(*target, classType, whenTrue, whenFalse);
    return;
  }
  this->addClassFacts(*target, classType, whenTrue, whenFalse);
}

void UnionNarrowing::addBufferFacts(const NarrowTarget &target, const Type *classType,
                                    Flow &whenTrue, Flow &whenFalse) const {
  const Type *now = this->targetType(whenTrue, target)->nonOptional();
  auto kept = std::vector<const Type *>();
  for (const Type *member : this->membersOf(now)) {
    if (member->isBuffer() || member->bytes() == Type::Bytes::Uint8) {
      kept.push_back(classType);
    }
  }
  const auto *buffers = this->keptUnion(kept);
  auto isNotBuffer = std::bind_front(&UnionNarrowing::isNotBufferMember, this);
  this->narrowTargetTo(whenTrue, target, buffers);
  this->narrowKept(whenFalse, target, isNotBuffer);
}

bool UnionNarrowing::isOneOf(llvm::ArrayRef<const Type *> matching, const Type *member) const {
  return llvm::is_contained(matching, member);
}

bool UnionNarrowing::isNoneOf(llvm::ArrayRef<const Type *> matching, const Type *member) const {
  return !llvm::is_contained(matching, member);
}

void UnionNarrowing::addMemberSetFacts(const NarrowTarget &target, const Type *classType,
                                       Flow &whenTrue, Flow &whenFalse) const {
  auto members = this->membersOf(classType);
  llvm::ArrayRef<const Type *> matching = members;
  auto isMatching = std::bind_front(&UnionNarrowing::isOneOf, this, matching);
  auto isOther = std::bind_front(&UnionNarrowing::isNoneOf, this, matching);
  this->narrowKept(whenTrue, target, isMatching);
  this->narrowKept(whenFalse, target, isOther);
}

bool UnionNarrowing::isSubclassMember(const Type *classType, const Type *member) const {
  return member->isObject() && member->isSubtypeOf(classType);
}

bool UnionNarrowing::isNotSubclassMember(const Type *classType, const Type *member) const {
  return !this->isSubclassMember(classType, member);
}

void UnionNarrowing::addClassFacts(const NarrowTarget &target, const Type *classType,
                                   Flow &whenTrue, Flow &whenFalse) const {
  const Type *now = this->targetType(whenTrue, target)->nonOptional();
  auto kept = std::vector<const Type *>();
  for (const Type *member : this->membersOf(now)) {
    if (this->isSubclassMember(classType, member)) {
      kept.push_back(member);
    } else if (member->isObject() && classType->isSubtypeOf(member)) {
      kept.push_back(classType);
    }
  }
  const auto *instances = this->keptUnion(kept);
  auto isOther = std::bind_front(&UnionNarrowing::isNotSubclassMember, this, classType);
  this->narrowTargetTo(whenTrue, target, instances);
  this->narrowKept(whenFalse, target, isOther);
}

bool UnionNarrowing::hasField(llvm::StringRef name, const Type *member) const {
  return member->isObject() && member->field(name) != nullptr;
}

bool UnionNarrowing::isMissingField(llvm::StringRef name, const Type *member) const {
  return !this->hasField(name, member) && !member->isUndefined();
}

void UnionNarrowing::addInFacts(const BinaryExpr *binary, Flow &whenTrue, Flow &whenFalse) const {
  auto target = this->narrowTarget(binary->rhs);
  auto *key = dyn_cast<StringLitExpr>(binary->lhs);
  if (!target.has_value() || key == nullptr) {
    return;
  }
  auto name = ToUtf8::toUtf8(key->value);
  auto hasField = std::bind_front(&UnionNarrowing::hasField, this, llvm::StringRef(name));
  auto isMissing = std::bind_front(&UnionNarrowing::isMissingField, this, llvm::StringRef(name));
  this->narrowKept(whenTrue, *target, hasField);
  this->narrowKept(whenFalse, *target, isMissing);
}

void UnionNarrowing::addEqualityFacts(const Expr *side, const Expr *other, Flow &equal,
                                      Flow &different) const {
  auto *typeOf = dyn_cast<UnaryExpr>(side);
  if (typeOf != nullptr && typeOf->op == UnaryOp::TypeOf) {
    this->addTypeOfFacts(typeOf, other, equal, different);
    return;
  }
  const auto *literal = this->comparedLiteral(other);
  if (literal != nullptr) {
    this->addLiteralFacts(side, literal, equal, different);
  }
}

bool UnionNarrowing::isTypeOfNamed(llvm::StringRef kind, const Type *member) const {
  return llvm::StringRef(TypeSpelling::typeOfName(member)) == kind;
}

bool UnionNarrowing::isNotTypeOfNamed(llvm::StringRef kind, const Type *member) const {
  return !this->isTypeOfNamed(kind, member);
}

void UnionNarrowing::addTypeOfFacts(const UnaryExpr *typeOf, const Expr *other, Flow &equal,
                                    Flow &different) const {
  auto *text = dyn_cast<StringLitExpr>(other);
  if (text == nullptr) {
    return;
  }
  auto target = this->narrowTarget(typeOf->operand);
  if (!target.has_value() || !target->declared->nonOptional()->isUnion()) {
    return;
  }
  auto kind = ToUtf8::toUtf8(text->value);
  auto isNamed = std::bind_front(&UnionNarrowing::isTypeOfNamed, this, llvm::StringRef(kind));
  auto isOther = std::bind_front(&UnionNarrowing::isNotTypeOfNamed, this, llvm::StringRef(kind));
  this->narrowKept(equal, *target, isNamed);
  this->narrowKept(different, *target, isOther);
}

const Type *UnionNarrowing::comparedLiteral(const Expr *other) const {
  const auto *literal = UnionCheckHelpers::writtenLiteral(other);
  if (literal != nullptr) {
    return literal;
  }
  const Type *type = other->type;
  bool isLiteral = type != nullptr && type->isLiteral();

  return isLiteral ? type : nullptr;
}

const MemberExpr *UnionNarrowing::taggedMember(const Expr *side) const {
  auto *chain = dyn_cast<OptionalChainExpr>(side);
  if (chain != nullptr) {
    return dyn_cast<MemberExpr>(chain->chain);
  }

  return dyn_cast<MemberExpr>(side);
}

bool UnionNarrowing::isConvertibleFrom(const Type *literal, const Type *type) const {
  return TypeRelations::isImplicitlyConvertible(literal, type);
}

bool UnionNarrowing::isOtherThan(const Type *literal, const Type *type) const {
  return type != literal;
}

void UnionNarrowing::addLiteralFacts(const Expr *side, const Type *literal, Flow &equal,
                                     Flow &different) const {
  const auto *member = this->taggedMember(side);
  if (member != nullptr && this->isTagNarrowable(member, equal)) {
    auto isConvertible = std::bind_front(&UnionNarrowing::isConvertibleFrom, this, literal);
    auto isOther = std::bind_front(&UnionNarrowing::isOtherThan, this, literal);
    this->narrowByTag(member, equal, isConvertible, UndefinedTag::DROPPED);
    this->narrowByTag(member, different, isOther, UndefinedTag::KEPT);
    return;
  }
  auto target = this->narrowTarget(side);
  if (target.has_value()) {
    this->narrowToLiteral(*target, literal, equal, different);
    return;
  }
  const auto *variable = this->readVariable(side);
  if (variable != nullptr && this->isLiteralNarrowing(variable, literal, equal)) {
    this->narrowVariable(equal, variable, literal);
  }
}

bool UnionNarrowing::isLiteralNarrowing(const VarSymbol *variable, const Type *literal,
                                        const Flow &flow) const {
  if (variable->type == nullptr || literal->literalBase() != variable->type->nonOptional()) {
    return false;
  }
  bool isNarrowable = literal->isStringLiteral() || literal->literalBase()->isInt();

  return isNarrowable && this->literalTypes->typeIn(flow, variable) == variable->type;
}

void UnionNarrowing::narrowToLiteral(const NarrowTarget &target, const Type *literal, Flow &equal,
                                     Flow &different) const {
  const Type *now = this->targetType(equal, target)->nonOptional();
  auto members = this->membersOf(now);
  if (llvm::is_contained(members, literal)) {
    auto isOther = std::bind_front(&UnionNarrowing::isOtherThan, this, literal);
    this->narrowTargetTo(equal, target, literal);
    this->narrowKept(different, target, isOther);
    return;
  }
  if (now->isBool() && literal->literalBase()->isBool()) {
    bool isFalse = literal->literalNumber() == 0;
    const auto *opposite = Type::boolLiteral(isFalse);
    this->narrowTargetTo(equal, target, literal);
    this->narrowTargetTo(different, target, opposite);
  }
}

}
