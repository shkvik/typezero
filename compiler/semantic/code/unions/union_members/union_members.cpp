#include "compiler/semantic/code/unions/union_members/union_members.h"
#include "compiler/semantic/bodies/objects/object_check_helpers/object_check_helpers.h"
#include "llvm/ADT/StringExtras.h"

namespace tz {

namespace {

constexpr const char *kObjectPlaceholder = "o";

}

UnionMembers::UnionMembers(SemanticState &owner, UnionRules &sharedUnionRules)
    : state(owner),
      unionRules(sharedUnionRules) {}

void UnionMembers::checkUnionMember(const CheckUnionMember &task) {
  MemberExpr *member = task.member;
  const Type *type = member->object->type;
  auto narrowed = this->checkNotTagged(member, type);
  if (narrowed.isErr()) {
    return this->reportUnionMember(member, narrowed.error().message);
  }
  if (this->isAllBytes(type)) {
    return this->state.walk.tasks.pushTask(CheckBytesUnionMember{.member = member});
  }
  auto fieldTypes = this->memberFieldTypes(member, type);
  if (fieldTypes.isErr()) {
    return this->reportUnionMember(member, fieldTypes.error().message);
  }
  const auto *joined = Type::unionOf(fieldTypes.value());
  auto joinable = this->checkJoinable(member, type, joined);
  if (joinable.isErr()) {
    return this->reportUnionMember(member, joinable.error().message);
  }
  member->type = joined;

  return this->state.walk.tasks.pushChecked(member);
}

void UnionMembers::reportUnionMember(MemberExpr *member, const std::string &message) {
  member->type = Type::error();
  this->state.diagnostics.error(member->loc(), message);

  return this->state.walk.tasks.pushChecked(member);
}

void UnionMembers::checkUnionFieldTarget(const CheckUnionFieldTarget &task) {
  auto target = this->unionFieldTarget(task.member);
  if (target.isErr()) {
    this->state.diagnostics.error(task.member->loc(), target.error().message);
    return this->state.walk.tasks.pushAnswer(Type::error());
  }

  return this->state.walk.tasks.pushAnswer(target.value());
}

std::string UnionMembers::fieldText(const Expr *object, llvm::StringRef field) const {
  auto chain = this->chainText(object);
  std::string text = chain.has_value() ? *chain : kObjectPlaceholder;
  bool isIdentifier = !field.empty() && !llvm::isDigit(field[0]) &&
                      llvm::all_of(field, std::bind_front(&UnionMembers::isIdentifierChar, this));

  return isIdentifier ? text + "." + field.str() : text + "[\"" + field.str() + "\"]";
}

std::optional<std::string> UnionMembers::chainText(const Expr *expr) const {
  auto names = std::vector<llvm::StringRef>();
  const auto *link = ObjectCheckHelpers::unnarrowed(expr);
  const auto *member = dyn_cast<MemberExpr>(link);
  while (member != nullptr) {
    if (member->optional) {
      return std::nullopt;
    }
    names.push_back(member->name);
    link = ObjectCheckHelpers::unnarrowed(member->object);
    member = dyn_cast<MemberExpr>(link);
  }
  const auto *root = dyn_cast<NameExpr>(link);
  if (root == nullptr) {
    return std::nullopt;
  }
  std::string text = root->name.str();
  for (llvm::StringRef name : llvm::reverse(names)) {
    text += "." + name.str();
  }

  return text;
}

bool UnionMembers::isIdentifierChar(char letter) const {
  return llvm::isAlnum(letter) || letter == '_' || letter == '$';
}

bool UnionMembers::isLiteralSet(const Type *type) const {
  if (type->isLiteral()) {
    return true;
  }
  if (!type->isUnion()) {
    return false;
  }
  for (const Type *member : type->members()) {
    if (!member->isLiteral()) {
      return false;
    }
  }

  return true;
}

bool UnionMembers::isAllBytes(const Type *type) const {
  for (const Type *member : type->members()) {
    if (!member->isBytes()) {
      return false;
    }
  }

  return true;
}

UnionMembers::ResultMemberAllowed UnionMembers::checkNotTagged(const MemberExpr *member,
                                                               const Type *type) const {
  if (type->isTagged()) {
    const char *typeOf = TypeSpelling::typeOfName(type->members().front());
    auto message = ErrorTextUnionMember::narrow(member->name, type->name(), typeOf);

    return err(CodeUnionsRegistryCheckUnionMemberError{
        .code = CodeUnionsRegistryCheckUnionMemberErrorCode::TAGGED_UNION,
        .message = message,
    });
  }

  return ok();
}

UnionMembers::ResultMemberFieldList UnionMembers::memberFieldTypes(const MemberExpr *member,
                                                                   const Type *type) {
  auto fieldTypes = std::vector<const Type *>();
  for (const Type *memberType : type->members()) {
    auto fieldType = this->memberFieldType(member, type, memberType);
    if (fieldType.isErr()) {
      return err(fieldType.error());
    }
    if (!llvm::is_contained(fieldTypes, fieldType.value())) {
      fieldTypes.push_back(fieldType.value());
    }
  }

  return ok(fieldTypes);
}

UnionMembers::ResultMemberFieldType
UnionMembers::memberFieldType(const MemberExpr *member, const Type *type, const Type *memberType) {
  auto *classDecl = this->unionRules.classOf(memberType);
  auto accessible = this->checkFieldAccessible(member, classDecl);
  if (accessible.isErr()) {
    return err(accessible.error());
  }
  const Field *field = this->unionMemberField(member, memberType, classDecl);
  if (field != nullptr) {
    return ok(field->type);
  }
  auto known = this->checkFieldKnown(member, type);
  if (known.isErr()) {
    return err(known.error());
  }
  auto hint = this->lackingHint(member, type);
  auto message =
      ErrorTextUnionMember::mayLack(member->name, memberType->name(), type->name(), hint);

  return err(CodeUnionsRegistryCheckUnionMemberError{
      .code = CodeUnionsRegistryCheckUnionMemberErrorCode::MAY_LACK_FIELD,
      .message = message,
  });
}

UnionMembers::ResultMemberAllowed UnionMembers::checkFieldAccessible(const MemberExpr *member,
                                                                     const ClassDecl *classDecl) {
  if (classDecl == nullptr) {
    return ok();
  }
  const ClassDecl *owner = classDecl->findFieldOwner(member->name);
  const auto *field = classDecl->findField(member->name);
  if (field == nullptr) {
    return ok();
  }
  auto accessible = this->unionRules.checkAccess(owner, field->access, member->name);
  if (accessible.isErr()) {
    return err(CodeUnionsRegistryCheckUnionMemberError{
        .code = CodeUnionsRegistryCheckUnionMemberErrorCode::NOT_ACCESSIBLE,
        .message = accessible.error().message,
    });
  }

  return ok();
}

const Field *UnionMembers::unionMemberField(const MemberExpr *member, const Type *memberType,
                                            const ClassDecl *classDecl) const {
  if (!memberType->isObject()) {
    return nullptr;
  }
  bool isDeclared = classDecl == nullptr || classDecl->findField(member->name) != nullptr;
  if (!isDeclared) {
    return nullptr;
  }

  return memberType->field(member->name);
}

UnionMembers::ResultMemberAllowed UnionMembers::checkFieldKnown(const MemberExpr *member,
                                                                const Type *type) const {
  if (this->isUnknownToPlainObjects(member, type)) {
    auto message = ErrorTextUnionMember::noField(type->name(), member->name);

    return err(CodeUnionsRegistryCheckUnionMemberError{
        .code = CodeUnionsRegistryCheckUnionMemberErrorCode::NO_SUCH_FIELD,
        .message = message,
    });
  }

  return ok();
}

bool UnionMembers::isUnknownToPlainObjects(const MemberExpr *member, const Type *type) const {
  for (const Type *memberType : type->members()) {
    bool isPlain = memberType->isObject() && !memberType->isClass();
    if (!isPlain || memberType->field(member->name) != nullptr) {
      return false;
    }
  }

  return true;
}

UnionMembers::ResultMemberAllowed
UnionMembers::checkJoinable(const MemberExpr *member, const Type *type, const Type *joined) const {
  auto problem = UnionSlots::unionProblem(joined);
  if (!problem.empty()) {
    auto message = ErrorTextUnionMember::clash(type->name(), member->name, problem);

    return err(CodeUnionsRegistryCheckUnionMemberError{
        .code = CodeUnionsRegistryCheckUnionMemberErrorCode::FIELD_TYPES_CLASH,
        .message = message,
    });
  }

  return ok();
}

std::string UnionMembers::lackingHint(const MemberExpr *member, const Type *type) const {
  auto fallback = std::string();
  for (const Field &tagField : type->members().front()->fields()) {
    auto hint = this->tagHint(member, type, tagField.name);
    if (hint.has_value() && hint->isUnique) {
      return hint->text;
    }
    if (hint.has_value() && fallback.empty()) {
      fallback = hint->text;
    }
  }
  if (!fallback.empty()) {
    return fallback;
  }

  return this->holderHint(member, type);
}

std::optional<UnionMembers::TagHint>
UnionMembers::tagHint(const MemberExpr *member, const Type *type, llvm::StringRef tagName) const {
  if (!this->isTagField(type, tagName)) {
    return std::nullopt;
  }
  const Type *holder = this->taggedHolder(member, type, tagName);
  if (holder == nullptr) {
    return std::nullopt;
  }
  const Type *tag = holder->field(tagName)->type;
  auto field = this->fieldText(member->object, tagName);
  auto text = ErrorTextUnionMember::tagHint(field, tag->name());
  bool isUnique = this->isTagUnique(member, type, tagName, tag);

  return TagHint{.text = text, .isUnique = isUnique};
}

bool UnionMembers::isTagField(const Type *type, llvm::StringRef tagName) const {
  for (const Type *memberType : type->members()) {
    const Field *field = memberType->field(tagName);
    if (field == nullptr || !this->isLiteralSet(field->type)) {
      return false;
    }
  }

  return true;
}

const Type *UnionMembers::taggedHolder(const MemberExpr *member, const Type *type,
                                       llvm::StringRef tagName) const {
  for (const Type *memberType : type->members()) {
    const Type *tag = memberType->field(tagName)->type;
    if (memberType->field(member->name) != nullptr && tag->isLiteral()) {
      return memberType;
    }
  }

  return nullptr;
}

bool UnionMembers::isTagUnique(const MemberExpr *member, const Type *type, llvm::StringRef tagName,
                               const Type *tag) const {
  for (const Type *other : type->members()) {
    bool isLacking = other->field(member->name) == nullptr;
    if (isLacking && this->isHoldingLiteral(other->field(tagName)->type, tag)) {
      return false;
    }
  }

  return true;
}

bool UnionMembers::isHoldingLiteral(const Type *set, const Type *literal) const {
  return set == literal || (set->isUnion() && llvm::is_contained(set->members(), literal));
}

std::string UnionMembers::holderHint(const MemberExpr *member, const Type *type) const {
  const Type *holder = this->lastHolder(member, type);
  if (holder == nullptr) {
    return "";
  }
  if (holder->isClass()) {
    return ErrorTextUnionMember::classHint(holder->name());
  }

  return ErrorTextUnionMember::inHint(member->name);
}

const Type *UnionMembers::lastHolder(const MemberExpr *member, const Type *type) const {
  for (const Type *memberType : llvm::reverse(type->members())) {
    if (memberType->isObject() && memberType->field(member->name) != nullptr) {
      return memberType;
    }
  }

  return nullptr;
}

UnionMembers::ResultUnionFieldTarget UnionMembers::unionFieldTarget(MemberExpr *member) {
  const Type *type = member->object->type;
  const Type *common = nullptr;
  for (const Type *memberType : type->members()) {
    auto target = this->memberTargetType(member, type, memberType);
    if (target.isErr()) {
      return err(target.error());
    }
    auto same = this->checkSameTarget(member, type, common, target.value());
    if (same.isErr()) {
      return err(same.error());
    }
    common = target.value();
  }
  this->state.walk.flow.forgetFieldsNamed(member->name);
  member->type = common;

  return ok(common);
}

UnionMembers::ResultUnionFieldTarget
UnionMembers::memberTargetType(const MemberExpr *member, const Type *type, const Type *memberType) {
  auto *classDecl = this->unionRules.classOf(memberType);
  const Field *field = this->assignableField(member, memberType, classDecl);
  if (field == nullptr) {
    auto message =
        ErrorTextUnionMember::mayLack(member->name, memberType->name(), type->name(), "");

    return err(CodeUnionsRegistryCheckUnionFieldTargetError{
        .code = CodeUnionsRegistryCheckUnionFieldTargetErrorCode::MAY_LACK_FIELD,
        .message = message,
    });
  }
  auto accessible = this->checkTargetAccessible(member, classDecl);
  if (accessible.isErr()) {
    return err(accessible.error());
  }
  if (field->readonly) {
    auto message = ErrorTextUnionTarget::readOnly(member->name, memberType->name());

    return err(CodeUnionsRegistryCheckUnionFieldTargetError{
        .code = CodeUnionsRegistryCheckUnionFieldTargetErrorCode::READ_ONLY_FIELD,
        .message = message,
    });
  }

  return ok(field->type);
}

const Field *UnionMembers::assignableField(const MemberExpr *member, const Type *memberType,
                                           const ClassDecl *classDecl) const {
  if (!memberType->isObject()) {
    return nullptr;
  }
  if (classDecl != nullptr && !this->unionRules.hasDataField(classDecl, member->name)) {
    return nullptr;
  }

  return memberType->field(member->name);
}

UnionMembers::ResultTargetAllowed UnionMembers::checkTargetAccessible(const MemberExpr *member,
                                                                      const ClassDecl *classDecl) {
  if (classDecl == nullptr) {
    return ok();
  }
  const ClassDecl *owner = classDecl->findFieldOwner(member->name);
  const ClassField *declared = classDecl->findField(member->name);
  if (declared == nullptr) {
    return ok();
  }
  auto accessible = this->unionRules.checkAccess(owner, declared->access, member->name);
  if (accessible.isErr()) {
    return err(CodeUnionsRegistryCheckUnionFieldTargetError{
        .code = CodeUnionsRegistryCheckUnionFieldTargetErrorCode::NOT_ACCESSIBLE,
        .message = accessible.error().message,
    });
  }

  return ok();
}

UnionMembers::ResultTargetAllowed UnionMembers::checkSameTarget(const MemberExpr *member,
                                                                const Type *type,
                                                                const Type *common,
                                                                const Type *fieldType) const {
  if (common != nullptr && fieldType != common) {
    auto message =
        ErrorTextUnionTarget::differ(type->name(), member->name, common->name(), fieldType->name());

    return err(CodeUnionsRegistryCheckUnionFieldTargetError{
        .code = CodeUnionsRegistryCheckUnionFieldTargetErrorCode::FIELD_TYPES_DIFFER,
        .message = message,
    });
  }

  return ok();
}

}
