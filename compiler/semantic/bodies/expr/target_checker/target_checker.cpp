#include "compiler/semantic/bodies/expr/target_checker/target_checker.h"
#include "compiler/program/library/builtin_names/builtin_property_table/builtin_property_table.h"
#include "compiler/semantic/bodies/classes/class_member_checker/class_member_checker.h"
#include "compiler/semantic/bodies/classes/static_checker/static_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/expr/member_checker/member_checker.h"
#include "compiler/semantic/bodies/expr/name_checker/name_checker.h"
#include "compiler/semantic/bodies/functions/nested_functions/nested_functions.h"
#include "compiler/semantic/bodies/objects/dict_checker/dict_checker.h"
#include "compiler/semantic/bodies/objects/keyed_field_checker/keyed_field_checker.h"
#include "compiler/semantic/bodies/top_level_checker/top_level_checker.h"
#include "compiler/semantic/library/builtin_modules/runtime_object_checker/runtime_object_checker.h"
#include "compiler/semantic/library/json/json_checker/json_checker.h"
#include "compiler/semantic/names/scopes/scopes.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "compiler/semantic/types/union_checker/union_checker.h"
#include "compiler/semantic/types/utility_types/utility_types.h"
#include "llvm/ADT/STLExtras.h"
#include <cmath>

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

constexpr llvm::StringLiteral kThisName = "this";

}

TargetChecker::TargetChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->scopes = &all.scopes;
  this->dictChecker = &all.dictChecker;
  this->exprChecker = &all.exprChecker;
  this->jsonChecker = &all.jsonChecker;
  this->nameChecker = &all.nameChecker;
  this->unionChecker = &all.unionChecker;
  this->utilityTypes = &all.utilityTypes;
  this->memberChecker = &all.memberChecker;
  this->staticChecker = &all.staticChecker;
  this->typeRelations = &all.typeRelations;
  this->nestedFunctions = &all.nestedFunctions;
  this->topLevelChecker = &all.topLevelChecker;
  this->keyedFieldChecker = &all.keyedFieldChecker;
  this->classMemberChecker = &all.classMemberChecker;
  this->runtimeObjectChecker = &all.runtimeObjectChecker;
}

TargetChecker::ResultIndexIsNotBytes TargetChecker::checkIndexNotBytes(const Type *type) const {
  if (type->isBytes()) {
    bool isDataView = type->bytes() == Type::Bytes::DataView;
    auto message = ErrorTextIndexTarget::bytes(type->name(), isDataView);

    return err(TargetCheckerIndexNotBytesError{
        .code = TargetCheckerIndexNotBytesErrorCode::BYTES_INDEXED,
        .message = message,
    });
  }

  return ok(type);
}

TargetChecker::ResultObjectIsDefined TargetChecker::checkObjectDefined(const Type *type) const {
  bool isHolder = type->isOptional() && (type->inner()->isObject() || type->inner()->isDict());
  if (isHolder) {
    auto message = ErrorTextIndexTarget::undefined();

    return err(TargetCheckerObjectDefinedError{
        .code = TargetCheckerObjectDefinedErrorCode::MAY_BE_UNDEFINED,
        .message = message,
    });
  }

  return ok(type);
}

TargetChecker::ResultIndexIsWritable TargetChecker::checkIndexWritable(const Type *type) const {
  if (type->isString()) {
    auto message = ErrorTextIndexTarget::immutable();

    return err(TargetCheckerIndexWritableError{
        .code = TargetCheckerIndexWritableErrorCode::STRING_IMMUTABLE,
        .message = message,
    });
  }
  if (type->isOptional() && type->inner()->isArray()) {
    auto message = ErrorTextIndexTarget::arrayUndef();

    return err(TargetCheckerIndexWritableError{
        .code = TargetCheckerIndexWritableErrorCode::ARRAY_MAY_BE_UNDEFINED,
        .message = message,
    });
  }
  if (!type->isArray()) {
    auto message = ErrorTextIndexTarget::notIndexed(type->name());

    return err(TargetCheckerIndexWritableError{
        .code = TargetCheckerIndexWritableErrorCode::NOT_INDEXABLE,
        .message = message,
    });
  }

  return ok(type);
}

TargetChecker::ResultDefinedDictType
TargetChecker::checkDictDefined(const Type *type, const MemberExpr *member) const {
  if (type->isOptional() && type->inner()->isDict()) {
    auto message = ErrorTextMemberTarget::undefined(member->name);

    return err(TargetCheckerDictDefinedError{
        .code = TargetCheckerDictDefinedErrorCode::MAY_BE_UNDEFINED,
        .message = message,
    });
  }

  return ok(type);
}

TargetChecker::ResultNoBytesProperty
TargetChecker::checkBytesMember(const Type *type, const MemberExpr *member) const {
  const Type *bytes = type->nonOptional();
  bool isLength = BuiltinPropertyTable::isName(member->name, BuiltinProperty::LENGTH) ||
                  BuiltinPropertyTable::isName(member->name, BuiltinProperty::BYTE_LENGTH);
  if (bytes->isBytes() && isLength) {
    auto message = ErrorTextMemberTarget::fixedSize(bytes->name());

    return err(TargetCheckerBytesMemberError{
        .code = TargetCheckerBytesMemberErrorCode::LENGTH_FIXED,
        .message = message,
    });
  }
  if (bytes->isBytes()) {
    auto message = ErrorTextMemberTarget::noAssign(member->name, bytes->name());

    return err(TargetCheckerBytesMemberError{
        .code = TargetCheckerBytesMemberErrorCode::NOT_WRITABLE,
        .message = message,
    });
  }

  return ok(type);
}

TargetChecker::ResultFieldIsWritable TargetChecker::checkMemberWritable(const Type *type,
                                                                        const MemberExpr *member,
                                                                        AssignForm form) const {
  bool isLength = BuiltinPropertyTable::isName(member->name, BuiltinProperty::LENGTH);
  bool isArrayLength = type->isArray() && isLength;
  if (isArrayLength && form == AssignForm::COMPOUND) {
    auto message = ErrorTextMemberTarget::onlyAssign();

    return err(TargetCheckerMemberWritableError{
        .code = TargetCheckerMemberWritableErrorCode::COMPOUND_LENGTH,
        .message = message,
    });
  }
  if (type->isString() && isLength) {
    auto message = ErrorTextMemberTarget::textLength();

    return err(TargetCheckerMemberWritableError{
        .code = TargetCheckerMemberWritableErrorCode::STRING_LENGTH,
        .message = message,
    });
  }
  if (!isArrayLength) {
    auto message = ErrorTextMemberTarget::noAssign(member->name, type->name());

    return err(TargetCheckerMemberWritableError{
        .code = TargetCheckerMemberWritableErrorCode::NOT_WRITABLE,
        .message = message,
    });
  }

  return ok(Type::intTy());
}

TargetChecker::ResultTargetIsNotThis TargetChecker::checkNotThis(const NameExpr *name) const {
  if (name->name == kThisName) {
    auto message = ErrorTextVarTarget::toThis();

    return err(TargetCheckerNotThisError{
        .code = TargetCheckerNotThisErrorCode::ASSIGN_TO_THIS,
        .message = message,
    });
  }

  return ok(name);
}

TargetChecker::ResultVariableMutable
TargetChecker::checkVariableMutable(const NameExpr *name, const VarSymbol *variable) const {
  if (variable->isConst) {
    bool isFunction = variable->function() != nullptr;
    auto message = ErrorTextVarTarget::constant(name->name, isFunction);

    return err(TargetCheckerVariableMutableError{
        .code = TargetCheckerVariableMutableErrorCode::CONSTANT,
        .message = message,
    });
  }

  return ok(variable);
}

const Type *TargetChecker::checkAssignTarget(Expr *&target, AssignForm form) {
  auto *name = dyn_cast<NameExpr>(target);
  auto *index = dyn_cast<IndexExpr>(target);
  auto *member = dyn_cast<MemberExpr>(target);
  if (name != nullptr) {
    auto *variable = this->checkVarTarget(name);
    return variable != nullptr ? variable->type : Type::error();
  }
  if (index != nullptr) {
    return this->indexTargetType(target, index, form);
  }
  if (member != nullptr) {
    return this->memberTargetType(target, member, form);
  }
  auto message = this->wrongTargetMessage(target);
  this->state.getDiagnostics().error(target->loc(), message);

  return Type::error();
}

std::string TargetChecker::wrongTargetMessage(const Expr *target) const {
  if (isa<ArrayLitExpr>(target)) {
    return ErrorTextAssignTarget::arrayForm();
  }
  if (isa<ObjectLitExpr>(target)) {
    return ErrorTextAssignTarget::objectForm();
  }

  return ErrorTextAssignTarget::notTarget();
}

const Type *TargetChecker::dictTargetType(Expr *target, AssignForm form) {
  auto value = this->dictChecker->checkDictTarget(target, form);
  if (value.isErr()) {
    this->state.getDiagnostics().error(target->loc(), value.error().message);
    return Type::error();
  }

  return value.value();
}

const Type *TargetChecker::indexTargetType(Expr *&target, IndexExpr *index, AssignForm form) {
  index->object = this->exprChecker->checkExpr(index->object);
  const Type *type = index->object->type;
  index->type = Type::error();
  auto generic = this->genericIndexType(index);
  if (generic.has_value()) {
    return *generic;
  }
  if (type->nonOptional()->isJson()) {
    return this->jsonChecker->checkJsonTarget(index);
  }
  if (type->isDict()) {
    return this->dictTargetType(index, form);
  }
  if (type->isTypedArray()) {
    return this->typedArrayTargetType(index, type);
  }
  auto notBytes = this->checkIndexNotBytes(type);
  if (notBytes.isErr()) {
    index->index = this->exprChecker->checkExpr(index->index);
    this->state.getDiagnostics().error(index->loc(), notBytes.error().message);
    return Type::error();
  }
  if (type->isObject()) {
    return this->keyedTargetType(target, index, form);
  }

  return this->arrayTargetType(index);
}

const Type *TargetChecker::typedArrayTargetType(IndexExpr *index, const Type *type) {
  index->index = this->memberChecker->checkPosition(index->index);
  bool isClamped = type->bytes() == Type::Bytes::Uint8Clamped;
  index->type = isClamped ? Type::intTy() : type->elementType();

  return index->type;
}

std::optional<const Type *> TargetChecker::genericIndexType(IndexExpr *index) {
  auto generic = this->utilityTypes->checkGenericIndex(index);
  bool isGenericIndex =
      generic.isOk() ||
      generic.error().code != UtilityTypesCheckGenericIndexErrorCode::NOT_A_GENERIC_INDEX;
  if (!isGenericIndex) {
    return std::nullopt;
  }
  if (generic.isErr()) {
    this->state.getDiagnostics().error(index->index->loc(), generic.error().message);
  }

  return index->type;
}

const Type *TargetChecker::keyedTargetType(Expr *&target, IndexExpr *index, AssignForm form) {
  auto written = this->keyedFieldChecker->checkKeyedField(index, KeyedAccess::WRITE);
  bool isReportable =
      written.isErr() &&
      written.error().code != KeyedFieldCheckerCheckKeyedFieldErrorCode::ALREADY_REPORTED;
  if (isReportable) {
    bool isWrongKey = this->keyedFieldChecker->wrongKey(written.error().code);
    llvm::SMLoc at = isWrongKey ? index->index->loc() : index->loc();
    this->state.getDiagnostics().error(at, written.error().message);
  }
  Expr *checked = written.isOk() ? written.value() : index;
  if (index->keyed()) {
    return index->type;
  }
  auto *member = dyn_cast<MemberExpr>(checked);
  if (member == nullptr) {
    return Type::error();
  }
  target = member;

  return this->classMemberChecker->checkFieldTarget(member, form);
}

const Type *TargetChecker::arrayTargetType(IndexExpr *index) {
  const Type *type = index->object->type;
  auto defined = this->checkObjectDefined(type);
  if (defined.isErr()) {
    index->index = this->exprChecker->checkExpr(index->index);
    this->state.getDiagnostics().error(index->object->loc(), defined.error().message);
    return Type::error();
  }
  index->index = this->memberChecker->checkPosition(index->index);
  if (type->isError()) {
    return type;
  }
  auto writable = this->checkIndexWritable(type);
  if (writable.isErr()) {
    bool isArrayUndefined =
        writable.error().code == TargetCheckerIndexWritableErrorCode::ARRAY_MAY_BE_UNDEFINED;
    llvm::SMLoc at = isArrayUndefined ? index->object->loc() : index->loc();
    this->state.getDiagnostics().error(at, writable.error().message);
    return Type::error();
  }
  index->type = type->inner();

  return index->type;
}

const Type *TargetChecker::memberTargetType(Expr *&target, MemberExpr *member, AssignForm form) {
  auto *name = dyn_cast<NameExpr>(member->object);
  ClassDecl *named = name != nullptr ? this->scopes->lookup(name->name).cls() : nullptr;
  if (named != nullptr) {
    return this->staticChecker->checkStaticTarget(target, named);
  }
  if (name != nullptr && name->name == kThisName) {
    this->state.getClassContext().rememberThisMember(member->name);
  }
  member->object = this->exprChecker->checkExpr(member->object);
  this->state.getClassContext().forgetThisMember();
  const Type *type = member->object->type;
  member->type = Type::error();
  if (type->isError()) {
    return type;
  }
  if (type->nonOptional()->isJson()) {
    return this->jsonChecker->checkJsonTarget(member);
  }
  auto *classDecl = const_cast<ClassDecl *>(type->classRef());
  if (classDecl != nullptr && classDecl->type != nullptr && isa<NameExpr>(member->object)) {
    return this->staticChecker->checkStaticTarget(target, classDecl);
  }
  if (type->isDict()) {
    return this->dictTargetType(member, form);
  }
  auto dict = this->checkDictDefined(type, member);
  if (dict.isErr()) {
    this->state.getDiagnostics().error(member->object->loc(), dict.error().message);
    return Type::error();
  }
  if (type->isUnion()) {
    return this->unionTargetType(member, type, form);
  }

  return this->plainMemberTargetType(member, type, form);
}

const Type *TargetChecker::unionTargetType(MemberExpr *member, const Type *type, AssignForm form) {
  const auto *common = this->unionChecker->commonBaseWith(type, member->name);
  if (common != nullptr) {
    member->object = this->typeRelations->convert(member->object, common);
    return this->classMemberChecker->checkFieldTarget(member, form);
  }
  auto unionField = this->unionChecker->checkUnionFieldTarget(member);
  if (unionField.isErr()) {
    this->state.getDiagnostics().error(member->loc(), unionField.error().message);
    return Type::error();
  }

  return unionField.value();
}

const Type *TargetChecker::plainMemberTargetType(MemberExpr *member, const Type *type,
                                                 AssignForm form) {
  auto notBytes = this->checkBytesMember(type, member);
  if (notBytes.isErr()) {
    this->state.getDiagnostics().error(member->loc(), notBytes.error().message);
    return Type::error();
  }
  if (type->isRuntimeObject()) {
    return this->runtimeObjectChecker->checkRuntimeTarget(member);
  }
  if (type->isObject() || (type->isOptional() && type->inner()->isObject())) {
    return this->classMemberChecker->checkFieldTarget(member, form);
  }
  auto writable = this->checkMemberWritable(type, member, form);
  if (writable.isErr()) {
    this->state.getDiagnostics().error(member->loc(), writable.error().message);
    return Type::error();
  }
  member->type = writable.value();

  return writable.value();
}

VarSymbol *TargetChecker::checkVarTarget(NameExpr *name) {
  auto thisTarget = this->checkNotThis(name);
  if (thisTarget.isErr()) {
    this->state.getDiagnostics().error(name->loc(), thisTarget.error().message);
    return nullptr;
  }
  auto entry = this->topLevelChecker->lookupValue(name->name);
  VarSymbol *variable = entry.var();
  if (variable == nullptr) {
    this->nameChecker->checkName(name);
    return nullptr;
  }
  name->binding = NamedVar{.var = variable};
  name->type = this->scopes->varType(variable);
  auto visible = this->scopes->checkVisible(name, variable);
  if (visible.isErr()) {
    this->state.getDiagnostics().error(name->loc(), visible.error().message);
  }
  this->walkVariableWritable(name, variable);
  variable->reassigned = true;
  this->nestedFunctions->noteUse(variable, name->loc());

  return variable;
}

void TargetChecker::walkVariableWritable(const NameExpr *name, VarSymbol *variable) {
  auto mutableVar = this->checkVariableMutable(name, variable);
  if (mutableVar.isErr()) {
    this->state.getDiagnostics().error(name->loc(), mutableVar.error().message);
    return;
  }
  auto writable = this->topLevelChecker->checkImportAssign(name, variable);
  if (writable.isErr()) {
    this->state.getDiagnostics().error(name->loc(), writable.error().message);
  }
}

}
