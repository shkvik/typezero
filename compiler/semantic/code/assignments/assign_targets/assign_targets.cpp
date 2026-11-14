#include "compiler/semantic/code/assignments/assign_targets/assign_targets.h"
#include "compiler/program/library/builtin_names/builtin_property_table/builtin_property_table.h"
#include "compiler/semantic/declaration/names/declaration_names_registry_errors.h"
#include "compiler/semantic/library/bytes/bytes_check_helpers/bytes_check_helpers.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

constexpr const char *kAssignSuffix = "=";

}

AssignTargets::AssignTargets(SemanticState &owner, AssignmentRules &sharedAssignmentRules)
    : state(owner),
      assignmentRules(sharedAssignmentRules) {}

void AssignTargets::checkAssign(const CheckAssign &task) {
  AssignExpr *assign = task.assign;
  auto *member = dyn_cast<MemberExpr>(assign->target);
  auto *index = dyn_cast<IndexExpr>(assign->target);
  this->state.walk.tasks.pushTask(AfterTargetAlias{.assign = assign});
  if (member != nullptr) {
    return this->state.walk.tasks.pushTask(ProcessAliasSlot{.slot = &member->object});
  }
  if (index != nullptr) {
    return this->state.walk.tasks.pushTask(ProcessAliasSlot{.slot = &index->object});
  }
}

void AssignTargets::afterTargetAlias(const AfterTargetAlias &task) {
  AssignExpr *assign = task.assign;
  auto *name = dyn_cast<NameExpr>(assign->target);
  if (name != nullptr) {
    auto assignable = this->checkNotImported(name);
    if (assignable.isErr()) {
      this->state.diagnostics.error(name->loc(), assignable.error().message);
      assign->type = Type::error();
      this->state.walk.tasks.pushChecked(assign);
      return this->state.walk.tasks.pushTask(
          CheckExprSlot{.slot = &assign->value, .expected = nullptr});
    }
  }
  this->state.walk.tasks.pushTask(AfterProcessAssign{.assign = assign});

  return this->state.walk.tasks.pushTask(CheckProcessAssign{.assign = assign});
}

void AssignTargets::afterProcessAssign(const AfterProcessAssign &task) {
  AssignExpr *assign = task.assign;
  Expr *exitCode = this->state.walk.tasks.takeChecked();
  if (exitCode != nullptr) {
    return this->state.walk.tasks.pushChecked(exitCode);
  }
  bool isCompound = assign->op.has_value();
  BinaryOp op = isCompound ? *assign->op : BinaryOp::Add;
  if (isCompound && (op == BinaryOp::LogOr || op == BinaryOp::LogAnd)) {
    return this->startLogicalAssign(assign);
  }
  if (isCompound && op == BinaryOp::Nullish) {
    return this->startNullishAssign(assign);
  }
  bool isUnfolded = isCompound && this->unfoldNarrowedCompound(assign, op);
  bool isStillCompound = isCompound && !isUnfolded;
  auto form = isStillCompound ? AssignForm::COMPOUND : AssignForm::PLAIN;
  this->state.walk.tasks.pushTask(AfterAssignTarget{.assign = assign, .op = op, .form = form});

  return this->assignmentRules.startAssignTarget(&assign->target, form);
}

void AssignTargets::startLogicalAssign(AssignExpr *assign) {
  auto *read = this->logicalRead(assign->target);
  if (read == nullptr) {
    assign->type = Type::error();
    auto spelling = std::string(Operators::opSpelling(*assign->op)) + kAssignSuffix;
    auto message = ErrorTextLogicalAssign::simple(spelling);
    this->state.walk.tasks.pushChecked(assign);
    this->state.walk.tasks.pushTask(ReportError{.loc = assign->target->loc(), .message = message});
    return this->state.walk.tasks.pushTask(
        CheckExprSlot{.slot = &assign->value, .expected = nullptr});
  }
  auto *set =
      this->state.ast.make<AssignExpr>(assign->loc(), std::nullopt, assign->target, assign->value);
  auto *binary = this->state.ast.make<BinaryExpr>(assign->loc(), *assign->op, read, set);
  bool isAnd = binary->op == BinaryOp::LogAnd;
  auto use = isAnd ? TruthUse::AndAssign : TruthUse::OrAssign;
  this->state.walk.tasks.pushTask(AfterLogicalAssign{.binary = binary});

  return this->state.walk.tasks.pushTask(StartLogical{
      .binary = binary,
      .expected = nullptr,
      .side = RightSide::VALUE,
      .use = use,
  });
}

void AssignTargets::startNullishAssign(AssignExpr *assign) {
  auto *member = dyn_cast<MemberExpr>(assign->target);
  bool isPlainMember = member != nullptr && !member->optional;
  if (!isPlainMember) {
    return this->assignmentRules.startNullishByTarget(assign);
  }
  auto *name = dyn_cast<NameExpr>(member->object);
  auto load = name != nullptr ? this->state.findEarlyLoad(name->name) : std::nullopt;
  if (!load.has_value()) {
    return this->state.walk.tasks.pushTask(AfterNullishHolder{.assign = assign});
  }
  this->state.walk.tasks.pushTask(AfterNullishHolder{.assign = assign});

  return this->state.walk.tasks.pushTask(*load);
}

void AssignTargets::finishVarTarget(const FinishVarTarget &task) {
  NameExpr *name = task.name;
  VarSymbol *variable = this->state.walk.scopes.findName(name->name).var();
  if (variable == nullptr) {
    this->state.walk.tasks.pushTask(DiscardChecked{});
    return this->state.walk.tasks.pushTask(CheckName{.name = name});
  }
  name->binding = NamedVar{.var = variable};
  FunctionDecl *functionDecl =
      variable->function() != nullptr ? variable->function() : variable->lambda();
  bool isKnown =
      variable->type != nullptr || functionDecl == nullptr || functionDecl->sym == nullptr;
  if (isKnown) {
    name->type = variable->type != nullptr ? variable->type : Type::error();
    return this->walkVarTargetUse(name, variable);
  }
  this->state.walk.tasks.pushTask(VarTargetTypeKnown{.name = name, .variable = variable});

  return this->state.walk.tasks.pushTask(FunctionTypeOf{.function = functionDecl->sym});
}

void AssignTargets::varTargetTypeKnown(const VarTargetTypeKnown &task) {
  VarSymbol *variable = task.variable;
  const Type *type = this->state.walk.tasks.takeAnswer();
  bool isNamedFunction =
      variable->function() != nullptr && variable->function()->sym->hasReturnType();
  if (isNamedFunction) {
    variable->type = type;
  }
  task.name->type = type;

  return this->walkVarTargetUse(task.name, variable);
}

void AssignTargets::walkVarTargetUse(NameExpr *name, VarSymbol *variable) {
  auto visible = this->checkVisible(name, variable);
  if (visible.isErr()) {
    this->state.diagnostics.error(name->loc(), visible.error().message);
  }
  this->walkVariableWritable(name, variable);
  variable->reassigned = true;

  return this->state.walk.tasks.pushTask(NoteUse{.variable = variable, .loc = name->loc()});
}

AssignTargets::ResultVisibleVariable AssignTargets::checkVisible(const NameExpr *name,
                                                                 const VarSymbol *variable) const {
  if (this->state.walk.block.isOtherCaseVar(variable)) {
    auto message = ErrorTextCheckVisible::otherCase(name->name);

    return err(DeclarationNamesRegistryCheckVisibleError{
        .code = DeclarationNamesRegistryCheckVisibleErrorCode::DECLARED_IN_OTHER_CASE,
        .message = message,
    });
  }

  return ok(variable);
}

void AssignTargets::discardChecked() {
  this->state.walk.tasks.takeChecked();
}

void AssignTargets::answerVarTarget(const AnswerVarTarget &task) {
  const VarSymbol *variable = task.name->var();
  const Type *type = variable != nullptr ? variable->type : Type::error();

  return this->state.walk.tasks.pushAnswer(type);
}

void AssignTargets::afterIndexTargetObject(const AfterIndexTargetObject &task) {
  IndexExpr *index = task.index;
  const Type *type = index->object->type;
  index->type = Type::error();
  auto &tasks = this->state.walk.tasks;
  if (this->isGenericIndex(index)) {
    tasks.pushTask(AfterGenericTarget{.index = index});
    tasks.pushTask(FinishGenericIndex{.index = index});
    return tasks.pushTask(CheckExprSlot{.slot = &index->index, .expected = nullptr});
  }
  if (type->nonOptional()->isJson()) {
    return tasks.pushTask(CheckJsonTarget{.target = index});
  }
  if (type->isDict()) {
    return tasks.pushTask(CheckDictTarget{.target = index, .form = task.form});
  }
  if (type->isTypedArray()) {
    tasks.pushTask(AfterTypedArrayPosition{.index = index});
    return tasks.pushTask(CheckPosition{.slot = &index->index});
  }
  auto notBytes = this->checkIndexNotBytes(type);
  if (notBytes.isErr()) {
    tasks.pushAnswer(Type::error());
    tasks.pushTask(ReportError{.loc = index->loc(), .message = notBytes.error().message});
    return tasks.pushTask(CheckExprSlot{.slot = &index->index, .expected = nullptr});
  }
  if (type->isObject()) {
    tasks.pushTask(AfterKeyedTarget{.target = task.target, .index = index, .form = task.form});
    return tasks.pushTask(CheckKeyedField{.index = index, .access = KeyedAccess::WRITE});
  }
  auto defined = this->checkObjectDefined(type);
  if (defined.isErr()) {
    tasks.pushAnswer(Type::error());
    tasks.pushTask(ReportError{.loc = index->object->loc(), .message = defined.error().message});
    return tasks.pushTask(CheckExprSlot{.slot = &index->index, .expected = nullptr});
  }
  tasks.pushTask(AfterArrayPosition{.index = index});

  return tasks.pushTask(CheckPosition{.slot = &index->index});
}

bool AssignTargets::isGenericIndex(const IndexExpr *index) const {
  const Type *object = index->object->type;
  bool isOpenObject = object->isParam() && !object->isListParam() && object->iterated() == nullptr;
  if (isOpenObject) {
    return true;
  }
  const auto *keyName = dyn_cast<NameExpr>(index->index);
  if (!object->isObject() || keyName == nullptr) {
    return false;
  }
  const VarSymbol *variable = this->state.walk.scopes.findName(keyName->name).var();
  if (variable == nullptr || variable->type == nullptr) {
    return false;
  }
  const Type *key = variable->type;

  return key->isParam() && !key->isListParam() &&
         this->state.types.relations.isExpectingLiteral(key);
}

void AssignTargets::afterGenericTarget(const AfterGenericTarget &task) {
  this->state.walk.tasks.takeChecked();

  return this->state.walk.tasks.pushAnswer(task.index->type);
}

void AssignTargets::afterTypedArrayPosition(const AfterTypedArrayPosition &task) {
  IndexExpr *index = task.index;
  const Type *type = index->object->type;
  bool isClamped = type->bytes() == Type::Bytes::Uint8Clamped;
  index->type = isClamped ? Type::intTy() : type->elementType();

  return this->state.walk.tasks.pushAnswer(index->type);
}

void AssignTargets::afterKeyedTarget(const AfterKeyedTarget &task) {
  Expr *checked = this->state.walk.tasks.takeChecked();
  IndexExpr *index = task.index;
  if (index->keyed()) {
    return this->state.walk.tasks.pushAnswer(index->type);
  }
  auto *member = dyn_cast<MemberExpr>(checked);
  if (member == nullptr) {
    return this->state.walk.tasks.pushAnswer(Type::error());
  }
  *task.target = member;

  return this->state.walk.tasks.pushTask(CheckFieldTarget{.member = member, .form = task.form});
}

void AssignTargets::afterArrayPosition(const AfterArrayPosition &task) {
  IndexExpr *index = task.index;
  const Type *type = index->object->type;
  if (type->isError()) {
    return this->state.walk.tasks.pushAnswer(type);
  }
  auto writable = this->checkIndexWritable(type);
  if (writable.isErr()) {
    bool isArrayUndefined = writable.error().code ==
                            CodeAssignmentsRegistryIndexWritableErrorCode::ARRAY_MAY_BE_UNDEFINED;
    llvm::SMLoc at = isArrayUndefined ? index->object->loc() : index->loc();
    this->state.diagnostics.error(at, writable.error().message);
    return this->state.walk.tasks.pushAnswer(Type::error());
  }
  index->type = type->inner();

  return this->state.walk.tasks.pushAnswer(index->type);
}

void AssignTargets::afterMemberTargetObject(const AfterMemberTargetObject &task) {
  MemberExpr *member = task.member;
  this->state.walk.self.forgetThisMember();
  const Type *type = member->object->type;
  member->type = Type::error();
  auto &tasks = this->state.walk.tasks;
  if (type->isError()) {
    return tasks.pushAnswer(type);
  }
  if (type->nonOptional()->isJson()) {
    return tasks.pushTask(CheckJsonTarget{.target = member});
  }
  auto *classDecl = const_cast<ClassDecl *>(type->classRef());
  if (classDecl != nullptr && classDecl->type != nullptr && isa<NameExpr>(member->object)) {
    return tasks.pushTask(CheckStaticTarget{.target = task.target, .classDecl = classDecl});
  }
  if (type->isDict()) {
    return tasks.pushTask(CheckDictTarget{.target = member, .form = task.form});
  }
  auto dict = this->checkDictDefined(type, member);
  if (dict.isErr()) {
    this->state.diagnostics.error(member->object->loc(), dict.error().message);
    return tasks.pushAnswer(Type::error());
  }
  if (type->isUnion()) {
    return this->startUnionTarget(member, type, task.form);
  }

  return this->startPlainMemberTarget(member, type, task.form);
}

void AssignTargets::startUnionTarget(MemberExpr *member, const Type *type, AssignForm form) {
  const auto *common = this->state.types.relations.commonBaseWith(type, member->name);
  if (common == nullptr) {
    return this->state.walk.tasks.pushTask(CheckUnionFieldTarget{.member = member});
  }
  this->state.walk.tasks.pushTask(CheckFieldTarget{.member = member, .form = form});

  return this->state.walk.tasks.pushTask(ConvertSlot{.slot = &member->object, .to = common});
}

void AssignTargets::startPlainMemberTarget(MemberExpr *member, const Type *type, AssignForm form) {
  auto notBytes = this->checkBytesMember(type, member);
  if (notBytes.isErr()) {
    this->state.diagnostics.error(member->loc(), notBytes.error().message);
    return this->state.walk.tasks.pushAnswer(Type::error());
  }
  if (type->isRuntimeObject()) {
    return this->state.walk.tasks.pushTask(CheckRuntimeTarget{.member = member});
  }
  if (type->isObject() || (type->isOptional() && type->inner()->isObject())) {
    return this->state.walk.tasks.pushTask(CheckFieldTarget{.member = member, .form = form});
  }
  auto writable = this->checkMemberWritable(type, member, form);
  if (writable.isErr()) {
    this->state.diagnostics.error(member->loc(), writable.error().message);
    return this->state.walk.tasks.pushAnswer(Type::error());
  }
  member->type = writable.value();

  return this->state.walk.tasks.pushAnswer(writable.value());
}

AssignTargets::ResultNameIsAssignable AssignTargets::checkNotImported(const NameExpr *name) {
  if (this->state.findNodeName(name->name) != nullptr) {
    auto message = ErrorTextAssign::imported(name->name);

    return err(CodeAssignmentsRegistryNotImportedError{
        .code = CodeAssignmentsRegistryNotImportedErrorCode::IMPORTED,
        .message = message,
    });
  }

  return ok(name);
}

bool AssignTargets::unfoldNarrowedCompound(AssignExpr *assign, BinaryOp op) {
  auto *target = dyn_cast<NameExpr>(assign->target);
  if (target == nullptr) {
    return false;
  }
  const VarSymbol *variable = this->state.walk.scopes.findName(target->name).var();
  bool isNarrowed = variable != nullptr && variable->type != nullptr &&
                    variable->type->nonOptional()->isUnion() &&
                    this->state.walk.flow.findNarrowed(variable) != nullptr;
  if (!isNarrowed) {
    return false;
  }
  auto *read = this->state.ast.make<NameExpr>(target->loc(), target->name);
  assign->value = this->state.ast.make<BinaryExpr>(assign->loc(), op, read, assign->value);
  assign->op.reset();

  return true;
}

Expr *AssignTargets::logicalRead(const Expr *target) {
  auto *index = dyn_cast<IndexExpr>(target);
  if (isa<NameExpr>(target) || isa<MemberExpr>(target)) {
    return this->assignmentRules.copyChain(target);
  }
  if (index != nullptr && !index->optional) {
    return this->indexRead(index);
  }

  return nullptr;
}

Expr *AssignTargets::indexRead(const IndexExpr *index) {
  auto *object = this->assignmentRules.copyChain(index->object);
  auto *position = this->assignmentRules.copyChain(index->index);
  if (object == nullptr || position == nullptr) {
    return nullptr;
  }

  return this->state.ast.make<IndexExpr>(index->loc(), object, position);
}

AssignTargets::ResultIndexIsNotBytes AssignTargets::checkIndexNotBytes(const Type *type) const {
  if (type->isBytes()) {
    bool isDataView = type->bytes() == Type::Bytes::DataView;
    auto message = ErrorTextIndexTarget::bytes(type->name(), isDataView);

    return err(CodeAssignmentsRegistryIndexNotBytesError{
        .code = CodeAssignmentsRegistryIndexNotBytesErrorCode::BYTES_INDEXED,
        .message = message,
    });
  }

  return ok(type);
}

AssignTargets::ResultObjectIsDefined AssignTargets::checkObjectDefined(const Type *type) const {
  bool isHolder = type->isOptional() && (type->inner()->isObject() || type->inner()->isDict());
  if (isHolder) {
    auto message = ErrorTextIndexTarget::undefined();

    return err(CodeAssignmentsRegistryObjectDefinedError{
        .code = CodeAssignmentsRegistryObjectDefinedErrorCode::MAY_BE_UNDEFINED,
        .message = message,
    });
  }

  return ok(type);
}

AssignTargets::ResultIndexIsWritable AssignTargets::checkIndexWritable(const Type *type) const {
  if (type->isString()) {
    auto message = ErrorTextIndexTarget::immutable();

    return err(CodeAssignmentsRegistryIndexWritableError{
        .code = CodeAssignmentsRegistryIndexWritableErrorCode::STRING_IMMUTABLE,
        .message = message,
    });
  }
  if (type->isOptional() && type->inner()->isArray()) {
    auto message = ErrorTextIndexTarget::arrayUndef();

    return err(CodeAssignmentsRegistryIndexWritableError{
        .code = CodeAssignmentsRegistryIndexWritableErrorCode::ARRAY_MAY_BE_UNDEFINED,
        .message = message,
    });
  }
  if (!type->isArray()) {
    auto message = ErrorTextIndexTarget::notIndexed(type->name());

    return err(CodeAssignmentsRegistryIndexWritableError{
        .code = CodeAssignmentsRegistryIndexWritableErrorCode::NOT_INDEXABLE,
        .message = message,
    });
  }

  return ok(type);
}

AssignTargets::ResultDefinedDictType
AssignTargets::checkDictDefined(const Type *type, const MemberExpr *member) const {
  if (type->isOptional() && type->inner()->isDict()) {
    auto message = ErrorTextMemberTarget::undefined(member->name);

    return err(CodeAssignmentsRegistryDictDefinedError{
        .code = CodeAssignmentsRegistryDictDefinedErrorCode::MAY_BE_UNDEFINED,
        .message = message,
    });
  }

  return ok(type);
}

AssignTargets::ResultNoBytesProperty
AssignTargets::checkBytesMember(const Type *type, const MemberExpr *member) const {
  const Type *bytes = type->nonOptional();
  bool isLength = BuiltinPropertyTable::isName(member->name, BuiltinProperty::LENGTH) ||
                  BuiltinPropertyTable::isName(member->name, BuiltinProperty::BYTE_LENGTH);
  if (bytes->isBytes() && isLength) {
    auto message = ErrorTextMemberTarget::fixedSize(bytes->name());

    return err(CodeAssignmentsRegistryBytesMemberError{
        .code = CodeAssignmentsRegistryBytesMemberErrorCode::LENGTH_FIXED,
        .message = message,
    });
  }
  if (bytes->isBytes()) {
    auto message = ErrorTextMemberTarget::noAssign(member->name, bytes->name());

    return err(CodeAssignmentsRegistryBytesMemberError{
        .code = CodeAssignmentsRegistryBytesMemberErrorCode::NOT_WRITABLE,
        .message = message,
    });
  }

  return ok(type);
}

AssignTargets::ResultFieldIsWritable AssignTargets::checkMemberWritable(const Type *type,
                                                                        const MemberExpr *member,
                                                                        AssignForm form) const {
  bool isLength = BuiltinPropertyTable::isName(member->name, BuiltinProperty::LENGTH);
  bool isArrayLength = type->isArray() && isLength;
  if (isArrayLength && form == AssignForm::COMPOUND) {
    auto message = ErrorTextMemberTarget::onlyAssign();

    return err(CodeAssignmentsRegistryMemberWritableError{
        .code = CodeAssignmentsRegistryMemberWritableErrorCode::COMPOUND_LENGTH,
        .message = message,
    });
  }
  if (type->isString() && isLength) {
    auto message = ErrorTextMemberTarget::textLength();

    return err(CodeAssignmentsRegistryMemberWritableError{
        .code = CodeAssignmentsRegistryMemberWritableErrorCode::STRING_LENGTH,
        .message = message,
    });
  }
  if (!isArrayLength) {
    auto message = ErrorTextMemberTarget::noAssign(member->name, type->name());

    return err(CodeAssignmentsRegistryMemberWritableError{
        .code = CodeAssignmentsRegistryMemberWritableErrorCode::NOT_WRITABLE,
        .message = message,
    });
  }

  return ok(Type::intTy());
}

AssignTargets::ResultVariableMutable
AssignTargets::checkVariableMutable(const NameExpr *name, const VarSymbol *variable) const {
  if (variable->isConst) {
    bool isFunction = variable->function() != nullptr;
    auto message = ErrorTextVarTarget::constant(name->name, isFunction);

    return err(CodeAssignmentsRegistryVariableMutableError{
        .code = CodeAssignmentsRegistryVariableMutableErrorCode::CONSTANT,
        .message = message,
    });
  }

  return ok(variable);
}

void AssignTargets::walkVariableWritable(const NameExpr *name, VarSymbol *variable) {
  auto mutableVar = this->checkVariableMutable(name, variable);
  if (mutableVar.isErr()) {
    this->state.diagnostics.error(name->loc(), mutableVar.error().message);
    return;
  }
  auto writable = this->checkImportAssign(name, variable);
  if (writable.isErr()) {
    this->state.diagnostics.error(name->loc(), writable.error().message);
  }
}

AssignTargets::ResultVarNotImported
AssignTargets::checkImportAssign(const NameExpr *name, const VarSymbol *variable) const {
  if (this->state.declaration.modules
          .getModuleState(this->state.declaration.modules.getCurrentModule())
          .importedVars.contains(variable)) {
    auto message = ErrorTextImportAssign::imported(name->name);

    return err(CodeAssignmentsRegistryCheckImportAssignError{
        .code = CodeAssignmentsRegistryCheckImportAssignErrorCode::IMPORTED_VARIABLE,
        .message = message,
    });
  }

  return ok(variable);
}

}
