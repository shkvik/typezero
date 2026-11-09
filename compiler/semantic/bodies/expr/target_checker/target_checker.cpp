#include "compiler/semantic/bodies/expr/target_checker/target_checker.h"
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
  this->scopes = &all.getScopes();
  this->dictChecker = &all.getDictChecker();
  this->exprChecker = &all.getExprChecker();
  this->jsonChecker = &all.getJsonChecker();
  this->nameChecker = &all.getNameChecker();
  this->unionChecker = &all.getUnionChecker();
  this->utilityTypes = &all.getUtilityTypes();
  this->memberChecker = &all.getMemberChecker();
  this->staticChecker = &all.getStaticChecker();
  this->typeRelations = &all.getTypeRelations();
  this->nestedFunctions = &all.getNestedFunctions();
  this->topLevelChecker = &all.getTopLevelChecker();
  this->keyedFieldChecker = &all.getKeyedFieldChecker();
  this->classMemberChecker = &all.getClassMemberChecker();
  this->runtimeObjectChecker = &all.getRuntimeObjectChecker();
}

TargetChecker::ResultIndexIsNotBytes TargetChecker::indexNotBytes(const Type *type) {
  if (type->isBytes()) {
    bool dataView = type->bytes() == Type::Bytes::DataView;
    std::string message = ErrorTextIndexTarget::bytes(type->name(), dataView);

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
  bool length = member->name == "length" || member->name == "byteLength";
  if (bytes->isBytes() && length) {
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
  bool length = member->name == "length";
  bool arrayLength = type->isArray() && length;
  if (arrayLength && compound) {
    std::string message = ErrorTextMemberTarget::onlyAssign();

    return err(TargetCheckerMemberWritableError{
        .code = TargetCheckerMemberWritableErrorCode::COMPOUND_LENGTH,
        .message = message,
    });
  }
  if (type->isString() && length) {
    std::string message = ErrorTextMemberTarget::textLength();

    return err(TargetCheckerMemberWritableError{
        .code = TargetCheckerMemberWritableErrorCode::STRING_LENGTH,
        .message = message,
    });
  }
  if (!arrayLength) {
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
    bool function = var->function() != nullptr;
    std::string message = ErrorTextVarTarget::constant(name->name, function);

    return err(TargetCheckerVariableMutableError{
        .code = TargetCheckerVariableMutableErrorCode::CONSTANT,
        .message = message,
    });
  }

  return ok(var);
}

const Type *TargetChecker::checkAssignTarget(Expr *&target, bool compound) {
  if (auto *name = dyn_cast<NameExpr>(target)) {
    VarSymbol *var = this->checkVarTarget(name);
    return var ? var->type : Type::error();
  }

  if (auto *index = dyn_cast<IndexExpr>(target)) {
    index->object = this->exprChecker->checkExpr(index->object);
    const Type *type = index->object->type;
    index->type = Type::error();
    UtilityTypes::ResultGenericIndexed generic = this->utilityTypes->checkGenericIndex(index);
    bool genericIndex =
        generic.isOk() ||
        generic.error().code != UtilityTypesCheckGenericIndexErrorCode::NOT_A_GENERIC_INDEX;
    if (generic.isErr() && genericIndex) {
      this->state.getDiagnostics().error(index->index->loc(), generic.error().message);
    }
    if (genericIndex) {
      return index->type;
    }
    if (type->nonOptional()->isJson()) {
      return this->jsonChecker->checkJsonTarget(index);
    }
    if (type->isDict()) {
      DictChecker::ResultRecordTargetType value =
          this->dictChecker->checkDictTarget(index, compound);
      if (value.isErr()) {
        this->state.getDiagnostics().error(index->loc(), value.error().message);
        return Type::error();
      }
      return value.value();
    }
    if (type->isTypedArray()) {
      index->index = this->memberChecker->checkPosition(index->index);
      index->type =
          type->bytes() == Type::Bytes::Uint8Clamped ? Type::intTy() : type->elementType();
      return index->type;
    }
    ResultIndexIsNotBytes notBytes = TargetChecker::indexNotBytes(type);
    if (notBytes.isErr()) {
      index->index = this->exprChecker->checkExpr(index->index);
      this->state.getDiagnostics().error(index->loc(), notBytes.error().message);
      return Type::error();
    }
    if (type->isObject()) {
      KeyedFieldChecker::ResultKeyedFieldRead written =
          this->keyedFieldChecker->checkKeyedField(index, true);
      if (written.isErr() &&
          written.error().code != KeyedFieldCheckerCheckKeyedFieldErrorCode::ALREADY_REPORTED) {
        bool wrongKey = KeyedFieldChecker::wrongKey(written.error().code);
        llvm::SMLoc at = wrongKey ? index->index->loc() : index->loc();
        this->state.getDiagnostics().error(at, written.error().message);
      }
      Expr *checked = written.isOk() ? written.value() : index;
      if (index->keyed()) {
        return index->type;
      }
      auto *member = dyn_cast<MemberExpr>(checked);
      if (!member) {
        return Type::error();
      }
      target = member;
      return this->classMemberChecker->checkFieldTarget(member, compound);
    }
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
      bool arrayUndefined =
          writable.error().code == TargetCheckerIndexWritableErrorCode::ARRAY_MAY_BE_UNDEFINED;
      llvm::SMLoc at = arrayUndefined ? index->object->loc() : index->loc();
      this->state.getDiagnostics().error(at, writable.error().message);
      return Type::error();
    }
    index->type = type->inner();
    return index->type;
  }

  if (auto *member = dyn_cast<MemberExpr>(target)) {
    auto *name = dyn_cast<NameExpr>(member->object);
    if (ClassDecl *cls = name ? this->scopes->lookup(name->name).cls() : nullptr) {
      return this->staticChecker->checkStaticTarget(target, cls);
    }
    if (name && name->name == "this") {
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
    if (ClassDecl *cls = const_cast<ClassDecl *>(type->classRef());
        cls && cls->type && isa<NameExpr>(member->object)) {
      return this->staticChecker->checkStaticTarget(target, cls);
    }
    if (type->isDict()) {
      DictChecker::ResultRecordTargetType value =
          this->dictChecker->checkDictTarget(member, compound);
      if (value.isErr()) {
        this->state.getDiagnostics().error(member->loc(), value.error().message);
        return Type::error();
      }
      return value.value();
    }
    ResultDefinedDictType dict = TargetChecker::dictDefined(type, member);
    if (dict.isErr()) {
      this->state.getDiagnostics().error(member->object->loc(), dict.error().message);
      return Type::error();
    }
    if (type->isUnion()) {
      if (const Type *common = this->unionChecker->commonBaseWith(type, member->name)) {
        member->object = this->typeRelations->convert(member->object, common);
        return this->classMemberChecker->checkFieldTarget(member, compound);
      }
      UnionChecker::ResultUnionFieldTarget unionField =
          this->unionChecker->checkUnionFieldTarget(member);
      if (unionField.isErr()) {
        this->state.getDiagnostics().error(member->loc(), unionField.error().message);
      }
      return unionField.isOk() ? unionField.value() : Type::error();
    }
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

  std::string message = isa<ArrayLitExpr>(target)    ? ErrorTextAssignTarget::arrayForm()
                        : isa<ObjectLitExpr>(target) ? ErrorTextAssignTarget::objectForm()
                                                     : ErrorTextAssignTarget::notTarget();
  this->state.getDiagnostics().error(target->loc(), message);

  return Type::error();
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
