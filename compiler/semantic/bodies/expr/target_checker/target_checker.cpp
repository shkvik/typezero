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

TargetChecker::ResultIndexIsNotBytes TargetChecker::indexNotBytes(const Type *type) {
  if (type->isBytes()) {
    bool isDataView = type->bytes() == Type::Bytes::DataView;
    std::string message = ErrorTextIndexTarget::bytes(type->name(), isDataView);

    return err(TargetCheckerIndexNotBytesError{
        .code = TargetCheckerIndexNotBytesErrorCode::BYTES_INDEXED,
        .message = message,
    });
  }

  return ok(type);
}

TargetChecker::ResultObjectIsDefined TargetChecker::objectDefined(const Type *type) {
  if (type->isOptional() && (type->inner()->isObject() || type->inner()->isDict())) {
    std::string message = ErrorTextIndexTarget::undefined();

    return err(TargetCheckerObjectDefinedError{
        .code = TargetCheckerObjectDefinedErrorCode::MAY_BE_UNDEFINED,
        .message = message,
    });
  }

  return ok(type);
}

TargetChecker::ResultIndexIsWritable TargetChecker::indexWritable(const Type *type) {
  if (type->isString()) {
    std::string message = ErrorTextIndexTarget::immutable();

    return err(TargetCheckerIndexWritableError{
        .code = TargetCheckerIndexWritableErrorCode::STRING_IMMUTABLE,
        .message = message,
    });
  }
  if (type->isOptional() && type->inner()->isArray()) {
    std::string message = ErrorTextIndexTarget::arrayUndef();

    return err(TargetCheckerIndexWritableError{
        .code = TargetCheckerIndexWritableErrorCode::ARRAY_MAY_BE_UNDEFINED,
        .message = message,
    });
  }
  if (!type->isArray()) {
    std::string message = ErrorTextIndexTarget::notIndexed(type->name());

    return err(TargetCheckerIndexWritableError{
        .code = TargetCheckerIndexWritableErrorCode::NOT_INDEXABLE,
        .message = message,
    });
  }

  return ok(type);
}

TargetChecker::ResultDefinedDictType TargetChecker::dictDefined(const Type *type,
                                                                const MemberExpr *member) {
  if (type->isOptional() && type->inner()->isDict()) {
    std::string message = ErrorTextMemberTarget::undefined(member->name);

    return err(TargetCheckerDictDefinedError{
        .code = TargetCheckerDictDefinedErrorCode::MAY_BE_UNDEFINED,
        .message = message,
    });
  }

  return ok(type);
}

TargetChecker::ResultNoBytesProperty TargetChecker::bytesMember(const Type *type,
                                                                const MemberExpr *member) {
  const Type *bytes = type->nonOptional();
  bool isLength = BuiltinPropertyTable::isName(member->name, BuiltinProperty::LENGTH) ||
                  BuiltinPropertyTable::isName(member->name, BuiltinProperty::BYTE_LENGTH);
  if (bytes->isBytes() && isLength) {
    std::string message = ErrorTextMemberTarget::fixedSize(bytes->name());

    return err(TargetCheckerBytesMemberError{
        .code = TargetCheckerBytesMemberErrorCode::LENGTH_FIXED,
        .message = message,
    });
  }
  if (bytes->isBytes()) {
    std::string message = ErrorTextMemberTarget::noAssign(member->name, bytes->name());

    return err(TargetCheckerBytesMemberError{
        .code = TargetCheckerBytesMemberErrorCode::NOT_WRITABLE,
        .message = message,
    });
  }

  return ok(type);
}

TargetChecker::ResultFieldIsWritable
TargetChecker::memberWritable(const Type *type, const MemberExpr *member, bool compound) {
  bool isLength = BuiltinPropertyTable::isName(member->name, BuiltinProperty::LENGTH);
  bool isArrayLength = type->isArray() && isLength;
  if (isArrayLength && compound) {
    std::string message = ErrorTextMemberTarget::onlyAssign();

    return err(TargetCheckerMemberWritableError{
        .code = TargetCheckerMemberWritableErrorCode::COMPOUND_LENGTH,
        .message = message,
    });
  }
  if (type->isString() && isLength) {
    std::string message = ErrorTextMemberTarget::textLength();

    return err(TargetCheckerMemberWritableError{
        .code = TargetCheckerMemberWritableErrorCode::STRING_LENGTH,
        .message = message,
    });
  }
  if (!isArrayLength) {
    std::string message = ErrorTextMemberTarget::noAssign(member->name, type->name());

    return err(TargetCheckerMemberWritableError{
        .code = TargetCheckerMemberWritableErrorCode::NOT_WRITABLE,
        .message = message,
    });
  }

  return ok(Type::intTy());
}

TargetChecker::ResultTargetIsNotThis TargetChecker::notThis(const NameExpr *name) {
  if (name->name == "this") {
    std::string message = ErrorTextVarTarget::toThis();

    return err(TargetCheckerNotThisError{
        .code = TargetCheckerNotThisErrorCode::ASSIGN_TO_THIS,
        .message = message,
    });
  }

  return ok(name);
}

TargetChecker::ResultVariableMutable TargetChecker::variableMutable(const NameExpr *name,
                                                                    const VarSymbol *var) {
  if (var->isConst) {
    bool isFunction = var->function() != nullptr;
    std::string message = ErrorTextVarTarget::constant(name->name, isFunction);

    return err(TargetCheckerVariableMutableError{
        .code = TargetCheckerVariableMutableErrorCode::CONSTANT,
        .message = message,
    });
  }

  return ok(var);
}

const Type *TargetChecker::checkAssignTarget(Expr *&target, bool compound) {
  auto *name = dyn_cast<NameExpr>(target);
  if (name != nullptr) {
    VarSymbol *var = this->checkVarTarget(name);
    return var != nullptr ? var->type : Type::error();
  }
  auto *index = dyn_cast<IndexExpr>(target);
  if (index != nullptr) {
    return this->indexTargetType(target, index, compound);
  }
  auto *member = dyn_cast<MemberExpr>(target);
  if (member != nullptr) {
    return this->memberTargetType(target, member, compound);
  }
  std::string message = TargetChecker::wrongTargetMessage(target);
  this->state.getDiagnostics().error(target->loc(), message);

  return Type::error();
}

std::string TargetChecker::wrongTargetMessage(const Expr *target) {
  if (isa<ArrayLitExpr>(target)) {
    return ErrorTextAssignTarget::arrayForm();
  }
  if (isa<ObjectLitExpr>(target)) {
    return ErrorTextAssignTarget::objectForm();
  }

  return ErrorTextAssignTarget::notTarget();
}

const Type *TargetChecker::dictTargetType(Expr *target, bool compound) {
  DictChecker::ResultRecordTargetType value = this->dictChecker->checkDictTarget(target, compound);
  if (value.isErr()) {
    this->state.getDiagnostics().error(target->loc(), value.error().message);
    return Type::error();
  }

  return value.value();
}

const Type *TargetChecker::indexTargetType(Expr *&target, IndexExpr *index, bool compound) {
  index->object = this->exprChecker->checkExpr(index->object);
  const Type *type = index->object->type;
  index->type = Type::error();
  std::optional<const Type *> generic = this->genericIndexType(index);
  if (generic.has_value()) {
    return *generic;
  }
  if (type->nonOptional()->isJson()) {
    return this->jsonChecker->checkJsonTarget(index);
  }
  if (type->isDict()) {
    return this->dictTargetType(index, compound);
  }
  if (type->isTypedArray()) {
    index->index = this->memberChecker->checkPosition(index->index);
    index->type = type->bytes() == Type::Bytes::Uint8Clamped ? Type::intTy() : type->elementType();
    return index->type;
  }
  ResultIndexIsNotBytes notBytes = TargetChecker::indexNotBytes(type);
  if (notBytes.isErr()) {
    index->index = this->exprChecker->checkExpr(index->index);
    this->state.getDiagnostics().error(index->loc(), notBytes.error().message);
    return Type::error();
  }
  if (type->isObject()) {
    return this->keyedTargetType(target, index, compound);
  }

  return this->arrayTargetType(index);
}

std::optional<const Type *> TargetChecker::genericIndexType(IndexExpr *index) {
  UtilityTypes::ResultGenericIndexed generic = this->utilityTypes->checkGenericIndex(index);
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

const Type *TargetChecker::keyedTargetType(Expr *&target, IndexExpr *index, bool compound) {
  KeyedFieldChecker::ResultKeyedFieldRead written =
      this->keyedFieldChecker->checkKeyedField(index, true);
  if (written.isErr() &&
      written.error().code != KeyedFieldCheckerCheckKeyedFieldErrorCode::ALREADY_REPORTED) {
    bool isWrongKey = KeyedFieldChecker::wrongKey(written.error().code);
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

  return this->classMemberChecker->checkFieldTarget(member, compound);
}

const Type *TargetChecker::arrayTargetType(IndexExpr *index) {
  const Type *type = index->object->type;
  ResultObjectIsDefined defined = TargetChecker::objectDefined(type);
  if (defined.isErr()) {
    index->index = this->exprChecker->checkExpr(index->index);
    this->state.getDiagnostics().error(index->object->loc(), defined.error().message);
    return Type::error();
  }
  index->index = this->memberChecker->checkPosition(index->index);
  if (type->isError()) {
    return type;
  }
  ResultIndexIsWritable writable = TargetChecker::indexWritable(type);
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

const Type *TargetChecker::memberTargetType(Expr *&target, MemberExpr *member, bool compound) {
  auto *name = dyn_cast<NameExpr>(member->object);
  ClassDecl *named = name != nullptr ? this->scopes->lookup(name->name).cls() : nullptr;
  if (named != nullptr) {
    return this->staticChecker->checkStaticTarget(target, named);
  }
  if (name != nullptr && name->name == "this") {
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
  auto *cls = const_cast<ClassDecl *>(type->classRef());
  if (cls != nullptr && cls->type != nullptr && isa<NameExpr>(member->object)) {
    return this->staticChecker->checkStaticTarget(target, cls);
  }
  if (type->isDict()) {
    return this->dictTargetType(member, compound);
  }
  ResultDefinedDictType dict = TargetChecker::dictDefined(type, member);
  if (dict.isErr()) {
    this->state.getDiagnostics().error(member->object->loc(), dict.error().message);
    return Type::error();
  }
  if (type->isUnion()) {
    return this->unionTargetType(member, type, compound);
  }

  return this->plainMemberTargetType(member, type, compound);
}

const Type *TargetChecker::unionTargetType(MemberExpr *member, const Type *type, bool compound) {
  const Type *common = this->unionChecker->commonBaseWith(type, member->name);
  if (common != nullptr) {
    member->object = this->typeRelations->convert(member->object, common);
    return this->classMemberChecker->checkFieldTarget(member, compound);
  }
  UnionChecker::ResultUnionFieldTarget unionField =
      this->unionChecker->checkUnionFieldTarget(member);
  if (unionField.isErr()) {
    this->state.getDiagnostics().error(member->loc(), unionField.error().message);
    return Type::error();
  }

  return unionField.value();
}

const Type *TargetChecker::plainMemberTargetType(MemberExpr *member, const Type *type,
                                                 bool compound) {
  ResultNoBytesProperty notBytes = TargetChecker::bytesMember(type, member);
  if (notBytes.isErr()) {
    this->state.getDiagnostics().error(member->loc(), notBytes.error().message);
    return Type::error();
  }
  if (type->isRuntimeObject()) {
    return this->runtimeObjectChecker->checkRuntimeTarget(member, compound);
  }
  if (type->isObject() || (type->isOptional() && type->inner()->isObject())) {
    return this->classMemberChecker->checkFieldTarget(member, compound);
  }
  ResultFieldIsWritable writable = TargetChecker::memberWritable(type, member, compound);
  if (writable.isErr()) {
    this->state.getDiagnostics().error(member->loc(), writable.error().message);
    return Type::error();
  }
  member->type = writable.value();

  return writable.value();
}

VarSymbol *TargetChecker::checkVarTarget(NameExpr *name) {
  ResultTargetIsNotThis thisTarget = TargetChecker::notThis(name);
  if (thisTarget.isErr()) {
    this->state.getDiagnostics().error(name->loc(), thisTarget.error().message);
    return nullptr;
  }
  ScopeEntry entry = this->topLevelChecker->lookupValue(name->name);
  if (!entry.var()) {
    this->nameChecker->checkName(name);
    return nullptr;
  }
  name->binding = NamedVar{.var = entry.var()};
  name->type = this->scopes->varType(entry.var());
  Scopes::ResultNameIsVisible visible = this->scopes->checkVisible(name, entry.var());
  if (visible.isErr()) {
    this->state.getDiagnostics().error(name->loc(), visible.error().message);
  }
  ResultVariableMutable mutableVar = TargetChecker::variableMutable(name, entry.var());
  if (mutableVar.isErr()) {
    this->state.getDiagnostics().error(name->loc(), mutableVar.error().message);
  } else {
    TopLevelChecker::ResultVarNotImported writable =
        this->topLevelChecker->checkImportAssign(name, entry.var());
    if (writable.isErr()) {
      this->state.getDiagnostics().error(name->loc(), writable.error().message);
    }
  }
  entry.var()->reassigned = true;
  this->nestedFunctions->noteUse(entry.var(), name->loc());

  return entry.var();
}

}
