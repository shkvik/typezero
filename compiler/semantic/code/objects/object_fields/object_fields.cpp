#include "compiler/semantic/code/objects/object_fields/object_fields.h"
#include "compiler/program/library/builtin_names/builtin_property_table/builtin_property_table.h"
#include "compiler/program/library/object/object_member_table/object_member_table.h"
#include "compiler/semantic/code/calls/code_calls_registry_errors.h"

namespace tz {

ObjectFields::ObjectFields(SemanticState &owner)
    : state(owner) {}

void ObjectFields::checkEnumObject(const CheckEnumObject &task) {
  this->state.walk.tasks.pushTask(AfterEnumObjectResolved{
      .name = task.name,
      .enumDecl = task.enumDecl,
      .expected = task.expected,
  });

  return this->state.walk.tasks.pushTask(ResolveEnum{.enumDecl = task.enumDecl});
}

void ObjectFields::afterEnumObjectResolved(const AfterEnumObjectResolved &task) {
  NameExpr *name = task.name;
  EnumDecl *enumDecl = task.enumDecl;
  auto *object = this->state.ast.make<ObjectLitExpr>(name->loc());
  for (const EnumMember &member : enumDecl->members) {
    auto *owner = this->state.ast.make<NameExpr>(name->loc(), enumDecl->name);
    auto *value = this->state.ast.make<MemberExpr>(name->loc(), owner, member.name);
    auto property = ObjectLitExpr::Property{
        .name = member.name,
        .loc = name->loc(),
        .value = value,
    };
    object->props.push_back(property);
  }
  this->state.walk.tasks.pushTask(RememberEnumObject{.name = name, .enumDecl = enumDecl});

  return this->state.walk.tasks.pushTask(CheckExpr{.expr = object, .expected = task.expected});
}

void ObjectFields::rememberEnumObject(const RememberEnumObject &task) {
  Expr *checked = this->state.walk.tasks.takeChecked();
  this->state.inventory.namedUses[checked] = {task.enumDecl, task.name->loc(), ""};

  return this->state.walk.tasks.pushChecked(checked);
}

void ObjectFields::checkField(const CheckField &task) {
  MemberExpr *member = task.member;
  const Type *type = member->object->type;
  if (type->isClass()) {
    return this->state.walk.tasks.pushTask(CheckClassMember{.member = member, .expected = nullptr});
  }
  const auto *field = type->field(member->name);
  if (field == nullptr) {
    member->type = Type::error();
    auto message = this->noFieldText(type, member->name);
    this->state.diagnostics.error(member->loc(), message);
    return this->state.walk.tasks.pushChecked(member);
  }

  return this->pushFieldValue(member, field);
}

void ObjectFields::pushFieldValue(MemberExpr *member, const Field *field) {
  member->type = field->type;
  this->state.walk.tasks.pushChecked(member);
  const auto *narrowed = this->state.flow.tracked.narrowedFieldType(member);
  if (narrowed != nullptr) {
    return this->state.walk.tasks.pushTask(Convert{.to = narrowed});
  }
  if (!field->type->isOptional()) {
    return;
  }
  auto path = this->state.flow.tracked.fieldPath(member);
  if (!path.has_value() || !this->state.walk.flow.isFieldDefined(*path)) {
    return;
  }

  return this->state.walk.tasks.pushTask(Convert{.to = field->type->inner()});
}

void ObjectFields::checkFieldCall(const CheckFieldCall &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  const Type *type = member->object->type;
  if (type->overload(member->name) != nullptr) {
    return this->state.walk.tasks.pushTask(CheckOverloadCall{.call = call, .member = member});
  }
  const auto *field = type->isClass() ? nullptr : type->field(member->name);
  if (!type->isClass() && field == nullptr) {
    member->type = Type::error();
    call->callee = member;
    call->type = Type::error();
    this->state.walk.tasks.pushChecked(call);
    auto message = this->noFieldText(type, member->name);
    this->state.walk.tasks.pushTask(ReportError{.loc = member->loc(), .message = message});
    return this->state.walk.tasks.pushTask(WalkArgsLoosely{.call = call});
  }
  this->state.walk.tasks.pushTask(AfterFieldCallee{.call = call, .member = member});
  if (type->isClass()) {
    return this->state.walk.tasks.pushTask(CheckClassMember{.member = member, .expected = nullptr});
  }

  return this->pushFieldValue(member, field);
}

void ObjectFields::afterFieldCallee(const AfterFieldCallee &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  call->callee = this->state.walk.tasks.takeChecked();
  this->state.walk.tasks.pushChecked(call);
  const Type *type = call->callee->type;
  if (type->isFunction()) {
    return this->startFieldValueCall(call, type);
  }
  call->type = Type::error();
  bool isOptionalFunction = type->isOptional() && type->inner()->isFunction();
  if (isOptionalFunction) {
    auto message = ErrorTextObjectField::maybeUndef();
    this->state.walk.tasks.pushTask(ReportError{.loc = member->loc(), .message = message});
  }
  bool isNotFunction = !isOptionalFunction && !type->isError();
  if (isNotFunction) {
    auto message = ErrorTextObjectField::notFunc(member->name, type->name());
    this->state.walk.tasks.pushTask(ReportError{.loc = member->loc(), .message = message});
  }

  return this->state.walk.tasks.pushTask(WalkArgsLoosely{.call = call});
}

void ObjectFields::startFieldValueCall(CallExpr *call, const Type *type) {
  call->type = type->ret();
  if (type->hasThis()) {
    auto message = ErrorTextValueCall::takesThis();
    this->state.walk.tasks.pushTask(ReportError{.loc = call->callee->loc(), .message = message});
    return this->state.walk.tasks.pushTask(WalkArgsLoosely{.call = call});
  }
  llvm::ArrayRef<const Type *> params = type->params();

  return this->state.walk.tasks.pushTask(CheckArguments{
      .call = call,
      .params = std::vector<const Type *>(params.begin(), params.end()),
      .minArgs = type->minArgs(),
      .hasRest = type->hasRest(),
      .loc = call->callee->loc(),
      .checked = {},
  });
}

void ObjectFields::checkObjectFieldTarget(const CheckObjectFieldTarget &task) {
  MemberExpr *member = task.member;
  const Type *type = member->object->type;
  auto defined = this->checkObjectDefined(type, member);
  if (defined.isErr()) {
    this->state.diagnostics.error(member->object->loc(), defined.error().message);
    return this->state.walk.tasks.pushAnswer(Type::error());
  }
  auto length = this->checkFixedLength(type, member);
  if (length.isErr()) {
    this->state.diagnostics.error(member->loc(), length.error().message);
    return this->state.walk.tasks.pushAnswer(Type::error());
  }
  auto found = this->checkFieldExists(type, member);
  if (found.isErr()) {
    this->state.diagnostics.error(member->loc(), found.error().message);
    return this->state.walk.tasks.pushAnswer(Type::error());
  }
  const Field *field = found.value();
  auto writable = this->checkFieldWritable(field, type, member);
  if (writable.isErr()) {
    this->state.diagnostics.error(member->loc(), writable.error().message);
  }
  member->type = field->type;

  return this->state.walk.tasks.pushAnswer(field->type);
}

bool ObjectFields::isInherited(llvm::StringRef name) const {
  auto member = ObjectMemberTable::find(name);

  return member.has_value();
}

std::string ObjectFields::noFieldText(const Type *type, llvm::StringRef name) const {
  bool isInherited = this->isInherited(name);

  return ErrorTextObjectField::noField(name, type->name(), isInherited);
}

ObjectFields::ResultTargetObjectType
ObjectFields::checkObjectDefined(const Type *type, const MemberExpr *member) const {
  if (type->isOptional()) {
    auto message = ErrorTextObjectField::setUndef(member->name);

    return err(CodeObjectsRegistryObjectDefinedError{
        .code = CodeObjectsRegistryObjectDefinedErrorCode::MAY_BE_UNDEFINED,
        .message = message,
    });
  }

  return ok(type);
}

ObjectFields::ResultLengthIsNotFixed
ObjectFields::checkFixedLength(const Type *type, const MemberExpr *member) const {
  if (type->isTuple() && BuiltinPropertyTable::isName(member->name, BuiltinProperty::LENGTH)) {
    auto message = ErrorTextObjectField::tupleLen();

    return err(CodeObjectsRegistryFixedLengthError{
        .code = CodeObjectsRegistryFixedLengthErrorCode::TUPLE_LENGTH,
        .message = message,
    });
  }

  return ok(type);
}

ObjectFields::ResultTargetFieldFound
ObjectFields::checkFieldExists(const Type *type, const MemberExpr *member) const {
  const auto *field = type->field(member->name);
  if (field == nullptr && type->overload(member->name) != nullptr) {
    auto message = ErrorTextObjectField::overloaded(member->name, type->name());

    return err(CodeObjectsRegistryFieldExistsError{
        .code = CodeObjectsRegistryFieldExistsErrorCode::OVERLOADED_METHOD,
        .message = message,
    });
  }
  if (field == nullptr) {
    auto message = this->noFieldText(type, member->name);

    return err(CodeObjectsRegistryFieldExistsError{
        .code = CodeObjectsRegistryFieldExistsErrorCode::NO_SUCH_FIELD,
        .message = message,
    });
  }

  return ok(field);
}

ObjectFields::ResultTargetIsWritable
ObjectFields::checkFieldWritable(const Field *field, const Type *type,
                                 const MemberExpr *member) const {
  if (field->readonly) {
    auto message = ErrorTextObjectField::readOnly(member->name, type->name());

    return err(CodeObjectsRegistryFieldWritableError{
        .code = CodeObjectsRegistryFieldWritableErrorCode::READ_ONLY,
        .message = message,
    });
  }

  return ok(field);
}

}
