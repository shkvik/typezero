#include "compiler/semantic/types/union_checker/union_checker.h"
#include "compiler/semantic/bodies/classes/class_bases/class_bases.h"
#include "compiler/semantic/bodies/classes/class_links/class_links.h"
#include "compiler/semantic/bodies/classes/class_member_checker/class_member_checker.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/objects/object_checker/object_checker.h"
#include "compiler/semantic/declare/class_declarer/class_declarer.h"
#include "compiler/semantic/flow/field_facts/field_facts.h"
#include "compiler/semantic/infer/return_inferrer/return_inferrer.h"
#include "compiler/semantic/library/bytes/bytes_checker/bytes_checker.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/union_check_helpers/union_check_helpers.h"

namespace tz {

UnionChecker::UnionChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->fieldFacts = &all.fieldFacts;
  this->classLinks = &all.classLinks;
  this->classBases = &all.classBases;
  this->bytesChecker = &all.bytesChecker;
  this->classDeclarer = &all.classDeclarer;
  this->objectChecker = &all.objectChecker;
  this->returnInferrer = &all.returnInferrer;
  this->argumentChecker = &all.argumentChecker;
  this->classMemberChecker = &all.classMemberChecker;
}

llvm::ArrayRef<const Type *> UnionChecker::membersOf(const Type *&type) const {
  return type->isUnion() ? type->members() : llvm::ArrayRef<const Type *>(type);
}

bool UnionChecker::isLiteralSet(const Type *type) const {
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

bool UnionChecker::isAllBytes(const Type *type) const {
  for (const Type *member : type->members()) {
    if (!member->isBytes()) {
      return false;
    }
  }

  return true;
}

bool UnionChecker::isAllObjects(const Type *type) const {
  for (const Type *member : type->members()) {
    if (!member->isObject()) {
      return false;
    }
  }

  return true;
}

ClassDecl *UnionChecker::completedClassOf(const Type *memberType) {
  auto *classDecl = this->classDeclarer->classOf(memberType);
  if (classDecl != nullptr) {
    this->classDeclarer->completeClass(classDecl);
  }

  return classDecl;
}

UnionChecker::ResultUnionMemberTyped UnionChecker::checkUnionMember(MemberExpr *member) {
  const Type *type = member->object->type;
  auto narrowed = this->checkNotTagged(member, type);
  if (narrowed.isErr()) {
    return err(narrowed.error());
  }
  if (this->isAllBytes(type)) {
    auto *bytes = this->bytesChecker->checkBytesUnionMember(member);

    return ok(bytes);
  }
  auto fieldTypes = this->memberFieldTypes(member, type);
  if (fieldTypes.isErr()) {
    return err(fieldTypes.error());
  }
  const auto *joined = Type::unionOf(fieldTypes.value());
  auto joinable = this->checkJoinable(member, type, joined);
  if (joinable.isErr()) {
    return err(joinable.error());
  }
  member->type = joined;

  return ok(member);
}

UnionChecker::ResultMemberAllowed UnionChecker::checkNotTagged(const MemberExpr *member,
                                                               const Type *type) const {
  if (type->isTagged()) {
    const char *typeOf = TypeSpelling::typeOfName(type->members().front());
    auto message = ErrorTextUnionMember::narrow(member->name, type->name(), typeOf);

    return err(UnionCheckerCheckUnionMemberError{
        .code = UnionCheckerCheckUnionMemberErrorCode::TAGGED_UNION,
        .message = message,
    });
  }

  return ok();
}

UnionChecker::ResultMemberFieldList UnionChecker::memberFieldTypes(const MemberExpr *member,
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

UnionChecker::ResultMemberFieldType
UnionChecker::memberFieldType(const MemberExpr *member, const Type *type, const Type *memberType) {
  auto *classDecl = this->completedClassOf(memberType);
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

  return err(UnionCheckerCheckUnionMemberError{
      .code = UnionCheckerCheckUnionMemberErrorCode::MAY_LACK_FIELD,
      .message = message,
  });
}

UnionChecker::ResultMemberAllowed UnionChecker::checkFieldAccessible(const MemberExpr *member,
                                                                     const ClassDecl *classDecl) {
  if (classDecl == nullptr) {
    return ok();
  }
  const ClassDecl *owner = nullptr;
  const auto *field = this->classBases->findField(classDecl, member->name, &owner);
  if (field == nullptr) {
    return ok();
  }
  auto accessible = this->classMemberChecker->checkAccess(owner, field->access, member->name);
  if (accessible.isErr()) {
    return err(UnionCheckerCheckUnionMemberError{
        .code = UnionCheckerCheckUnionMemberErrorCode::NOT_ACCESSIBLE,
        .message = accessible.error().message,
    });
  }

  return ok();
}

const Field *UnionChecker::unionMemberField(const MemberExpr *member, const Type *memberType,
                                            const ClassDecl *classDecl) const {
  if (!memberType->isObject()) {
    return nullptr;
  }
  bool isDeclared =
      classDecl == nullptr || this->classBases->findField(classDecl, member->name) != nullptr;
  if (!isDeclared) {
    return nullptr;
  }

  return memberType->field(member->name);
}

UnionChecker::ResultMemberAllowed UnionChecker::checkFieldKnown(const MemberExpr *member,
                                                                const Type *type) const {
  if (this->isUnknownToPlainObjects(member, type)) {
    auto message = ErrorTextUnionMember::noField(type->name(), member->name);

    return err(UnionCheckerCheckUnionMemberError{
        .code = UnionCheckerCheckUnionMemberErrorCode::NO_SUCH_FIELD,
        .message = message,
    });
  }

  return ok();
}

bool UnionChecker::isUnknownToPlainObjects(const MemberExpr *member, const Type *type) const {
  for (const Type *memberType : type->members()) {
    bool isPlain = memberType->isObject() && !memberType->isClass();
    if (!isPlain || memberType->field(member->name) != nullptr) {
      return false;
    }
  }

  return true;
}

UnionChecker::ResultMemberAllowed
UnionChecker::checkJoinable(const MemberExpr *member, const Type *type, const Type *joined) const {
  auto problem = UnionSlots::unionProblem(joined);
  if (!problem.empty()) {
    auto message = ErrorTextUnionMember::clash(type->name(), member->name, problem);

    return err(UnionCheckerCheckUnionMemberError{
        .code = UnionCheckerCheckUnionMemberErrorCode::FIELD_TYPES_CLASH,
        .message = message,
    });
  }

  return ok();
}

std::string UnionChecker::lackingHint(const MemberExpr *member, const Type *type) const {
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

std::optional<UnionChecker::TagHint>
UnionChecker::tagHint(const MemberExpr *member, const Type *type, llvm::StringRef tagName) const {
  if (!this->isTagField(type, tagName)) {
    return std::nullopt;
  }
  const Type *holder = this->taggedHolder(member, type, tagName);
  if (holder == nullptr) {
    return std::nullopt;
  }
  const Type *tag = holder->field(tagName)->type;
  auto field = this->objectChecker->fieldText(member->object, tagName);
  auto text = ErrorTextUnionMember::tagHint(field, tag->name());
  bool isUnique = this->isTagUnique(member, type, tagName, tag);

  return TagHint{.text = text, .isUnique = isUnique};
}

bool UnionChecker::isTagField(const Type *type, llvm::StringRef tagName) const {
  for (const Type *memberType : type->members()) {
    const Field *field = memberType->field(tagName);
    if (field == nullptr || !this->isLiteralSet(field->type)) {
      return false;
    }
  }

  return true;
}

const Type *UnionChecker::taggedHolder(const MemberExpr *member, const Type *type,
                                       llvm::StringRef tagName) const {
  for (const Type *memberType : type->members()) {
    const Type *tag = memberType->field(tagName)->type;
    if (memberType->field(member->name) != nullptr && tag->isLiteral()) {
      return memberType;
    }
  }

  return nullptr;
}

bool UnionChecker::isTagUnique(const MemberExpr *member, const Type *type, llvm::StringRef tagName,
                               const Type *tag) const {
  for (const Type *other : type->members()) {
    bool isLacking = other->field(member->name) == nullptr;
    if (isLacking && this->isHoldingLiteral(other->field(tagName)->type, tag)) {
      return false;
    }
  }

  return true;
}

bool UnionChecker::isHoldingLiteral(const Type *set, const Type *literal) const {
  return set == literal || (set->isUnion() && llvm::is_contained(set->members(), literal));
}

std::string UnionChecker::holderHint(const MemberExpr *member, const Type *type) const {
  const Type *holder = this->lastHolder(member, type);
  if (holder == nullptr) {
    return "";
  }
  if (holder->isClass()) {
    return ErrorTextUnionMember::classHint(holder->name());
  }

  return ErrorTextUnionMember::inHint(member->name);
}

const Type *UnionChecker::lastHolder(const MemberExpr *member, const Type *type) const {
  for (const Type *memberType : llvm::reverse(type->members())) {
    if (memberType->isObject() && memberType->field(member->name) != nullptr) {
      return memberType;
    }
  }

  return nullptr;
}

UnionChecker::ResultUnionFieldTarget UnionChecker::checkUnionFieldTarget(MemberExpr *member) {
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
  this->fieldFacts->fieldsAssigned(member->name);
  member->type = common;

  return ok(common);
}

UnionChecker::ResultUnionFieldTarget
UnionChecker::memberTargetType(const MemberExpr *member, const Type *type, const Type *memberType) {
  auto *classDecl = this->completedClassOf(memberType);
  const Field *field = this->assignableField(member, memberType, classDecl);
  if (field == nullptr) {
    auto message =
        ErrorTextUnionMember::mayLack(member->name, memberType->name(), type->name(), "");

    return err(UnionCheckerCheckUnionFieldTargetError{
        .code = UnionCheckerCheckUnionFieldTargetErrorCode::MAY_LACK_FIELD,
        .message = message,
    });
  }
  auto accessible = this->checkTargetAccessible(member, classDecl);
  if (accessible.isErr()) {
    return err(accessible.error());
  }
  if (field->readonly) {
    auto message = ErrorTextUnionTarget::readOnly(member->name, memberType->name());

    return err(UnionCheckerCheckUnionFieldTargetError{
        .code = UnionCheckerCheckUnionFieldTargetErrorCode::READ_ONLY_FIELD,
        .message = message,
    });
  }

  return ok(field->type);
}

const Field *UnionChecker::assignableField(const MemberExpr *member, const Type *memberType,
                                           const ClassDecl *classDecl) const {
  if (!memberType->isObject()) {
    return nullptr;
  }
  if (classDecl != nullptr && !this->hasDataField(classDecl, member->name)) {
    return nullptr;
  }

  return memberType->field(member->name);
}

bool UnionChecker::hasDataField(const ClassDecl *classDecl, llvm::StringRef name) const {
  const ClassField *declared = this->classBases->findField(classDecl, name);

  return declared != nullptr && declared->method() == nullptr;
}

UnionChecker::ResultTargetAllowed UnionChecker::checkTargetAccessible(const MemberExpr *member,
                                                                      const ClassDecl *classDecl) {
  if (classDecl == nullptr) {
    return ok();
  }
  const ClassDecl *owner = nullptr;
  const ClassField *declared = this->classBases->findField(classDecl, member->name, &owner);
  if (declared == nullptr) {
    return ok();
  }
  auto accessible = this->classMemberChecker->checkAccess(owner, declared->access, member->name);
  if (accessible.isErr()) {
    return err(UnionCheckerCheckUnionFieldTargetError{
        .code = UnionCheckerCheckUnionFieldTargetErrorCode::NOT_ACCESSIBLE,
        .message = accessible.error().message,
    });
  }

  return ok();
}

UnionChecker::ResultTargetAllowed UnionChecker::checkSameTarget(const MemberExpr *member,
                                                                const Type *type,
                                                                const Type *common,
                                                                const Type *fieldType) const {
  if (common != nullptr && fieldType != common) {
    auto message =
        ErrorTextUnionTarget::differ(type->name(), member->name, common->name(), fieldType->name());

    return err(UnionCheckerCheckUnionFieldTargetError{
        .code = UnionCheckerCheckUnionFieldTargetErrorCode::FIELD_TYPES_DIFFER,
        .message = message,
    });
  }

  return ok();
}

const Type *UnionChecker::commonBaseWith(const Type *type, llvm::StringRef name) const {
  if (!this->isAllObjects(type)) {
    return nullptr;
  }
  llvm::ArrayRef<const Type *> members = type->members();
  for (const Type *candidate : this->baseChain(members.front())) {
    if (this->isCommonBase(candidate, members) && this->hasMember(candidate, name)) {
      return candidate;
    }
  }

  return nullptr;
}

std::vector<const Type *> UnionChecker::baseChain(const Type *type) const {
  auto chain = std::vector<const Type *>{type};
  for (size_t index = 0; index < chain.size(); ++index) {
    for (const Type *base : chain[index]->bases()) {
      if (!llvm::is_contained(chain, base)) {
        chain.push_back(base);
      }
    }
  }

  return chain;
}

bool UnionChecker::isCommonBase(const Type *candidate, llvm::ArrayRef<const Type *> members) const {
  bool isOneOfSeveral = llvm::is_contained(members, candidate) && members.size() > 1;
  if (isOneOfSeveral) {
    return false;
  }
  for (const Type *member : members.drop_front()) {
    if (!member->isSubtypeOf(candidate)) {
      return false;
    }
  }

  return true;
}

bool UnionChecker::hasMember(const Type *candidate, llvm::StringRef name) const {
  const auto *classDecl = this->classDeclarer->classOf(candidate);
  if (classDecl == nullptr) {
    return candidate->field(name) != nullptr || candidate->overload(name) != nullptr;
  }
  if (this->classBases->findField(classDecl, name) != nullptr) {
    return true;
  }
  auto methods = this->classDeclarer->methodsNamed(classDecl, name);

  return !methods.empty();
}

std::optional<std::vector<const Type *>>
UnionChecker::unhandledCases(const SwitchStmt *stmt) const {
  const Type *type = stmt->discriminant->type;
  bool isChecked = type != nullptr && !type->isError() && !type->isOptional();
  if (!isChecked || !this->isLiteralSet(type)) {
    return std::nullopt;
  }
  auto members = this->membersOf(type);
  auto left = std::vector<const Type *>(members.begin(), members.end());
  for (const SwitchCase &switchCase : stmt->cases) {
    if (switchCase.test == nullptr) {
      return std::nullopt;
    }
    const Type *handled = this->caseLiteral(switchCase.test);
    if (handled != nullptr) {
      llvm::erase(left, handled);
    }
  }

  return left;
}

const Type *UnionChecker::caseLiteral(const Expr *test) const {
  const Type *written = UnionCheckHelpers::writtenLiteral(test);
  if (written != nullptr) {
    return written;
  }
  if (test->type != nullptr && test->type->isLiteral()) {
    return test->type;
  }

  return nullptr;
}

UnionChecker::ResultUnionMethodTyped UnionChecker::checkUnionMethodCall(CallExpr *call,
                                                                        MemberExpr *member) {
  const Type *receiver = member->object->type;
  call->type = Type::error();
  member->type = Type::error();
  auto resolved = this->resolveUnionMethods(member, receiver);
  if (resolved.isErr()) {
    this->argumentChecker->walkArgsLoosely(call);

    return err(resolved.error());
  }
  const Type *functionType = resolved.value().type;
  call->target = UnionMethodCall{.unionMethods = resolved.value().methods};
  member->type = functionType;
  this->argumentChecker->checkArguments(call, functionType->params(), functionType->minArgs(),
                                        functionType->hasRest(), member->loc());
  call->type = functionType->ret();

  return ok(call);
}

UnionChecker::ResultUnionMethodList UnionChecker::resolveUnionMethods(const MemberExpr *member,
                                                                      const Type *receiver) {
  auto narrowed = this->checkNarrowedObjects(member, receiver);
  if (narrowed.isErr()) {
    return err(narrowed.error());
  }
  const Type *functionType = nullptr;
  auto methods = std::vector<UnionMethod>();
  for (const Type *memberType : receiver->members()) {
    auto part = this->memberMethod(member, memberType);
    if (part.isErr()) {
      return err(part.error());
    }
    auto callable = this->checkCallablePart(member, memberType, part.value().type);
    if (callable.isErr()) {
      return err(callable.error());
    }
    auto same = this->checkSameMethodType(member, functionType, part.value().type);
    if (same.isErr()) {
      return err(same.error());
    }
    functionType = part.value().type;
    methods.push_back(part.value().entry);
  }

  return ok(UnionMethods{
      .type = functionType,
      .methods = methods,
  });
}

UnionChecker::ResultMethodAllowed UnionChecker::checkNarrowedObjects(const MemberExpr *member,
                                                                     const Type *receiver) const {
  if (receiver->isTagged() || !this->isAllObjects(receiver)) {
    const char *typeOf = TypeSpelling::typeOfName(receiver->members().front());
    auto message = ErrorTextUnionMember::narrow(member->name, receiver->name(), typeOf);

    return err(UnionCheckerCheckUnionMethodCallError{
        .code = UnionCheckerCheckUnionMethodCallErrorCode::NOT_NARROWED,
        .message = message,
    });
  }

  return ok();
}

UnionChecker::ResultMethodAllowed UnionChecker::checkCallablePart(const MemberExpr *member,
                                                                  const Type *memberType,
                                                                  const Type *partType) const {
  if (partType == nullptr || !partType->isFunction()) {
    const Type *receiver = member->object->type;
    bool isField = partType != nullptr;
    auto message = ErrorTextUnionMethod::notMethod(receiver->name(), memberType->name(),
                                                   member->name, isField);

    return err(UnionCheckerCheckUnionMethodCallError{
        .code = UnionCheckerCheckUnionMethodCallErrorCode::NOT_A_METHOD,
        .message = message,
    });
  }

  return ok();
}

UnionChecker::ResultMethodAllowed UnionChecker::checkSameMethodType(const MemberExpr *member,
                                                                    const Type *common,
                                                                    const Type *partType) const {
  if (common != nullptr && partType != common) {
    const Type *receiver = member->object->type;
    auto message = ErrorTextUnionMethod::differ(receiver->name(), member->name, common->name(),
                                                partType->name());

    return err(UnionCheckerCheckUnionMethodCallError{
        .code = UnionCheckerCheckUnionMethodCallErrorCode::METHOD_TYPES_DIFFER,
        .message = message,
    });
  }

  return ok();
}

UnionChecker::ResultUnionMethodPart UnionChecker::memberMethod(const MemberExpr *member,
                                                               const Type *memberType) {
  auto *classDecl = this->classDeclarer->classOf(memberType);
  if (classDecl != nullptr) {
    return this->classMemberMethod(member, memberType, classDecl);
  }
  bool hasField = memberType->field(member->name) != nullptr;
  if (hasField && memberType->isBase()) {
    const Type *receiver = member->object->type;
    auto message = ErrorTextUnionMethod::isBase(member->name, receiver->name(), memberType->name());

    return err(UnionCheckerCheckUnionMethodCallError{
        .code = UnionCheckerCheckUnionMethodCallErrorCode::INTERFACE_MEMBER,
        .message = message,
    });
  }

  return ok(this->fieldPart(memberType, member->name));
}

UnionChecker::UnionMethodPart UnionChecker::fieldPart(const Type *memberType,
                                                      llvm::StringRef name) const {
  const Field *field = memberType->field(name);
  if (field == nullptr) {
    return this->missingPart(memberType);
  }

  return UnionMethodPart{
      .type = field->type,
      .entry = UnionMethod{.member = memberType, .method = nullptr, .virtualCall = false},
  };
}

UnionChecker::UnionMethodPart UnionChecker::missingPart(const Type *memberType) const {
  return UnionMethodPart{
      .type = nullptr,
      .entry = UnionMethod{.member = memberType, .method = nullptr, .virtualCall = false},
  };
}

UnionChecker::ResultUnionMethodPart UnionChecker::classMemberMethod(const MemberExpr *member,
                                                                    const Type *memberType,
                                                                    ClassDecl *classDecl) {
  this->classDeclarer->completeClass(classDecl);
  if (this->hasDataField(classDecl, member->name)) {
    return this->classFieldPart(member, memberType, classDecl);
  }
  auto variants = this->classDeclarer->methodsNamed(classDecl, member->name);
  if (variants.size() > 1) {
    const Type *receiver = member->object->type;
    auto message =
        ErrorTextUnionMethod::variants(member->name, memberType->name(), receiver->name());

    return err(UnionCheckerCheckUnionMethodCallError{
        .code = UnionCheckerCheckUnionMethodCallErrorCode::SEVERAL_VARIANTS,
        .message = message,
    });
  }
  if (variants.empty()) {
    return ok(this->missingPart(memberType));
  }

  return this->classVariantMethod(member, memberType, classDecl, variants[0]);
}

UnionChecker::ResultUnionMethodPart UnionChecker::classFieldPart(const MemberExpr *member,
                                                                 const Type *memberType,
                                                                 const ClassDecl *classDecl) {
  const ClassDecl *owner = nullptr;
  const ClassField *field = this->classBases->findField(classDecl, member->name, &owner);
  auto accessible = this->classMemberChecker->checkAccess(owner, field->access, member->name);
  if (accessible.isErr()) {
    return err(UnionCheckerCheckUnionMethodCallError{
        .code = UnionCheckerCheckUnionMethodCallErrorCode::NOT_ACCESSIBLE,
        .message = accessible.error().message,
    });
  }

  return ok(this->fieldPart(memberType, member->name));
}

UnionChecker::ResultUnionMethodPart UnionChecker::classVariantMethod(const MemberExpr *member,
                                                                     const Type *memberType,
                                                                     ClassDecl *classDecl,
                                                                     FunctionDecl *variant) {
  auto accessible =
      this->classMemberChecker->checkAccess(variant->cls(), variant->access(), member->name);
  if (accessible.isErr()) {
    return err(UnionCheckerCheckUnionMethodCallError{
        .code = UnionCheckerCheckUnionMethodCallErrorCode::NOT_ACCESSIBLE,
        .message = accessible.error().message,
    });
  }
  FuncSymbol *function = variant->sym;
  auto entry = this->variantEntry(memberType, classDecl, function);
  const auto *functionType = this->returnInferrer->functionType(function);

  return ok(UnionMethodPart{
      .type = functionType,
      .entry = entry,
  });
}

UnionMethod UnionChecker::variantEntry(const Type *memberType, ClassDecl *classDecl,
                                       FuncSymbol *function) const {
  auto *implementation = this->classLinks->soleImplementation(classDecl, function->decl);
  if (implementation != nullptr) {
    return UnionMethod{.member = memberType, .method = implementation->sym, .virtualCall = false};
  }

  return UnionMethod{.member = memberType, .method = function, .virtualCall = true};
}

}
