#include "compiler/semantic/library/reflect/library_reflect_registry.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/program/to_utf16/to_utf16.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/declaration/class_tree/class_tree.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
#include "compiler/semantic/flow/placed_nodes/placed_nodes.h"
#include "compiler/semantic/types/type_parts/type_routes.h"
#include "llvm/ADT/STLExtras.h"
#include <functional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

LibraryReflectRegistry::LibraryReflectRegistry(SemanticState &owner)
    : state(owner) {}

void LibraryReflectRegistry::checkReflectCall(const CheckReflectCall &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  call->type = Type::error();
  auto function = ReflectFunctionTable::find(member->name);
  if (!function.has_value()) {
    auto message = ErrorTextReflectCall::unknown(member->name);

    return this->failReflectName(call, member, message);
  }
  switch (*function) {
    case ReflectFunction::GET:
    case ReflectFunction::APPLY:
    case ReflectFunction::CONSTRUCT:
    case ReflectFunction::FILL:
      return this->startReflection(call, member, task.expected);
    case ReflectFunction::METADATA:
    case ReflectFunction::GET_METADATA_KEYS:
    case ReflectFunction::GET_OWN_METADATA_KEYS:
      return this->reportReflectName(call, member, *function);
    case ReflectFunction::DEFINE_METADATA:
      return this->startMetadataCall(call, member, task.expected, {Builtin::MetaDefine, 3, 4});
    case ReflectFunction::GET_METADATA:
      return this->startMetadataCall(call, member, task.expected, {Builtin::MetaGet, 2, 3});
    case ReflectFunction::GET_OWN_METADATA:
      return this->startMetadataCall(call, member, task.expected, {Builtin::MetaGetOwn, 2, 3});
    case ReflectFunction::HAS_METADATA:
      return this->startMetadataCall(call, member, task.expected, {Builtin::MetaHas, 2, 3});
    case ReflectFunction::HAS_OWN_METADATA:
      return this->startMetadataCall(call, member, task.expected, {Builtin::MetaHasOwn, 2, 3});
    case ReflectFunction::DELETE_METADATA:
      return this->startMetadataCall(call, member, task.expected, {Builtin::MetaDelete, 2, 3});
  }
}

void LibraryReflectRegistry::reportReflectName(CallExpr *call, MemberExpr *member,
                                               ReflectFunction function) {
  bool isDecorator = function == ReflectFunction::METADATA;
  auto message =
      isDecorator ? ErrorTextReflectCall::decorator() : ErrorTextReflectCall::keys(member->name);

  return this->failReflectName(call, member, message);
}

void LibraryReflectRegistry::failReflectName(CallExpr *call, MemberExpr *member,
                                             const std::string &message) {
  auto &tasks = this->state.walk.tasks;
  this->state.diagnostics.error(member->loc(), message);
  tasks.pushChecked(call);

  return tasks.pushTask(WalkArgsLoosely{.call = call});
}

void LibraryReflectRegistry::startReflection(CallExpr *call, MemberExpr *member,
                                             const Type *expected) {
  auto &tasks = this->state.walk.tasks;
  call->type = Type::error();
  if (this->state.library.common.hasSpread(call)) {
    tasks.pushChecked(call);

    return tasks.pushTask(RejectSpreadCall{.call = call});
  }
  auto function = ReflectFunctionTable::find(member->name);
  if (function == ReflectFunction::GET) {
    return this->startReflectGet(call, member);
  }
  if (function == ReflectFunction::FILL) {
    return this->startReflectFill(call, member);
  }
  if (function == ReflectFunction::APPLY) {
    return this->startReflectApply(call, member);
  }

  return this->startReflectConstruct(call, member, expected);
}

bool LibraryReflectRegistry::hasWrongArity(CallExpr *call, MemberExpr *member, unsigned min,
                                           unsigned max) {
  auto arity = this->state.library.common.checkArity(call, min, max);
  if (arity.isErr()) {
    this->state.diagnostics.error(member->loc(), arity.error().message);
  }

  return arity.isErr();
}

void LibraryReflectRegistry::startReflectGet(CallExpr *call, MemberExpr *member) {
  auto &tasks = this->state.walk.tasks;
  if (this->hasWrongArity(call, member, 2, 2)) {
    return tasks.pushChecked(call);
  }
  auto *object = dyn_cast<NameExpr>(call->args[0]);
  auto *key = dyn_cast<StringLitExpr>(call->args[1]);
  tasks.pushTask(AfterReflectGetObject{.call = call, .object = object, .key = key});

  return tasks.pushTask(CheckExprSlot{.slot = &call->args[0], .expected = nullptr});
}

void LibraryReflectRegistry::afterReflectGetObject(const AfterReflectGetObject &task) {
  CallExpr *call = task.call;
  auto &tasks = this->state.walk.tasks;
  const Expr *target = call->args[0];
  const Type *type = target->type;
  if (type->isError()) {
    tasks.pushChecked(call);

    return tasks.pushTask(CheckExprSlot{.slot = &call->args[1], .expected = nullptr});
  }
  const Type *plain = type->nonOptional();
  const auto *base = this->state.generics.apparent.apparentType(plain);
  if (base->isJson()) {
    auto message = ErrorTextReflectGet::json();
    this->state.diagnostics.error(target->loc(), message);
    tasks.pushChecked(call);

    return tasks.pushTask(WalkArgsLoosely{.call = call});
  }
  bool isObject = type->isUnknown() || this->state.types.relations.isObjectReference(base) ||
                  base->isAnyObject();
  if (!isObject) {
    auto message = ErrorTextReflectGet::notObject(type->name());
    this->state.diagnostics.error(target->loc(), message);
    tasks.pushChecked(call);

    return tasks.pushTask(CheckExprSlot{.slot = &call->args[1], .expected = nullptr});
  }
  auto get = ReflectGetDraft{
      .call = call,
      .object = task.object,
      .key = task.key,
      .type = type,
      .base = base,
  };
  if (task.key == nullptr) {
    return this->startDynamicGet(get);
  }

  return this->startKnownGet(get);
}

void LibraryReflectRegistry::startKnownGet(const ReflectGetDraft &get) {
  auto &tasks = this->state.walk.tasks;
  auto *seen = this->stripImplicit(get.call->args[0]);
  const Type *seenType = seen->type;
  ClassDecl *classDecl = this->seenClass(seenType);
  auto ascii = this->asciiName(get.key->value);
  if (seenType->isOptional() || !ascii.has_value()) {
    return this->startDynamicGet(get);
  }
  auto known = ContinueKnownGet{.get = get, .name = *ascii};
  if (classDecl == nullptr) {
    return this->continueKnownGet(known);
  }
  tasks.pushTask(std::move(known));

  return tasks.pushTask(CompleteClass{.classDecl = classDecl});
}

ClassDecl *LibraryReflectRegistry::seenClass(const Type *seenType) const {
  bool isClassObject = seenType->isObject() && !seenType->isAnyObject();

  return isClassObject ? const_cast<ClassDecl *>(seenType->classDecl()) : nullptr;
}

void LibraryReflectRegistry::continueKnownGet(const ContinueKnownGet &task) {
  const ReflectGetDraft &get = task.get;
  const std::string &name = task.name;
  auto *seen = this->stripImplicit(get.call->args[0]);
  const Type *seenType = seen->type;
  ClassDecl *classDecl = this->seenClass(seenType);
  const ClassField *declared = classDecl != nullptr ? classDecl->findField(name) : nullptr;
  bool isOpen = classDecl == nullptr || (declared != nullptr && declared->access == Access::Public);
  const Type *base = get.base;
  bool isNamed = get.object != nullptr && !get.type->isOptional() && base->isObject() &&
                 !base->isAnyObject() && base->field(name) != nullptr && isOpen;
  if (isNamed) {
    return this->readNamedField(get.object, get.key, name);
  }
  bool isClassField = classDecl != nullptr && classDecl->literal() == nullptr &&
                      seenType->field(name) != nullptr && isOpen;
  if (isClassField) {
    return this->readClassField(seen, get.key, name);
  }
  bool isPlainRead =
      classDecl == nullptr || classDecl->literal() != nullptr || seenType->field(name) != nullptr;
  if (isPlainRead) {
    return this->startDynamicGet(get);
  }

  return this->readReflectMethod(get, seen, classDecl, name);
}

void LibraryReflectRegistry::readNamedField(const NameExpr *object, const StringLitExpr *key,
                                            const std::string &name) {
  auto &ast = this->state.ast;
  auto &tasks = this->state.walk.tasks;
  auto *root = ast.make<NameExpr>(object->loc(), object->name);
  llvm::StringRef kept = ast.keepName(name);
  auto *read = ast.make<MemberExpr>(key->loc(), root, kept);
  tasks.pushTask(Convert{.to = Type::unknown()});

  return tasks.pushTask(CheckExpr{.expr = read, .expected = nullptr});
}

void LibraryReflectRegistry::readClassField(Expr *seen, const StringLitExpr *key,
                                            const std::string &name) {
  auto &ast = this->state.ast;
  auto &tasks = this->state.walk.tasks;
  llvm::StringRef kept = ast.keepName(name);
  auto *read = ast.make<MemberExpr>(key->loc(), seen, kept);
  tasks.pushTask(Convert{.to = Type::unknown()});

  return tasks.pushTask(CheckClassMember{.member = read, .expected = nullptr});
}

void LibraryReflectRegistry::readReflectMethod(const ReflectGetDraft &get, Expr *seen,
                                               ClassDecl *classDecl, const std::string &name) {
  CallExpr *call = get.call;
  auto known = this->knownReflectMethod(classDecl, name);
  if (!known.has_value()) {
    return this->startDynamicGet(get);
  }
  call->args.resize(1);
  call->args[0] = seen;
  call->target = BuiltinCall{.builtin = Builtin::ReflectMethod, .method = known->method->sym};
  call->type = Type::unknown();
  if (known->hasSeveral) {
    this->state.library.reflection.markSeveralVariants(call);
  }

  return this->state.walk.tasks.pushChecked(call);
}

void LibraryReflectRegistry::startDynamicGet(const ReflectGetDraft &get) {
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(FinishDynamicGet{.get = get});

  return tasks.pushTask(CheckArgSlot{.slot = &get.call->args[1], .to = Type::string()});
}

void LibraryReflectRegistry::finishDynamicGet(const FinishDynamicGet &task) {
  CallExpr *call = task.get.call;
  auto &tasks = this->state.walk.tasks;
  this->noteReflectedRead(task.get.type, task.get.base);
  if (!this->state.library.reflect.isOnlyChecked()) {
    this->state.library.reflection.markReflectionUsed();
  }
  call->target = BuiltinCall{.builtin = Builtin::ReflectGet};
  call->type = Type::unknown();
  tasks.pushChecked(call);

  return tasks.pushTask(ConvertSlot{.slot = &call->args[0], .to = Type::unknown()});
}

void LibraryReflectRegistry::startReflectFill(CallExpr *call, MemberExpr *member) {
  auto &tasks = this->state.walk.tasks;
  tasks.pushChecked(call);
  if (this->hasWrongArity(call, member, 2, 2)) {
    return;
  }
  const Type *values = Type::dict(Type::DictKind::Map, Type::string(), Type::unknown());
  tasks.pushTask(AfterFillArgs{.call = call});
  tasks.pushTask(CheckArgSlot{.slot = &call->args[1], .to = values});

  return tasks.pushTask(CheckExprSlot{.slot = &call->args[0], .expected = nullptr});
}

void LibraryReflectRegistry::afterFillArgs(const AfterFillArgs &task) {
  CallExpr *call = task.call;
  auto &tasks = this->state.walk.tasks;
  const Expr *cls = call->args[0];
  const Type *type = cls->type;
  if (type->isError() || call->args[1]->type->isError()) {
    return;
  }
  if (!type->isClassRef()) {
    auto message = ErrorTextReflectFill::notClass(type->name());

    return this->state.diagnostics.error(cls->loc(), message);
  }
  auto fails = this->fillFailure(cls, type);
  if (!fails.has_value()) {
    return;
  }
  tasks.pushTask(FinishFill{.call = call, .type = type});
  tasks.pushTask(CalledFunction{});

  return tasks.pushTask(PreludeTypeOf{.type = *fails});
}

std::optional<PreludeType> LibraryReflectRegistry::fillFailure(const Expr *cls, const Type *type) {
  auto &reflection = this->state.library.reflection;
  auto *made = const_cast<ClassDecl *>(type->classRef());
  if (made == nullptr) {
    if (!this->state.library.reflect.isOnlyChecked()) {
      reflection.markAnyObjectFilled();
      reflection.markReflectionUsed();
    }

    return PreludeType::REFLECT_FILL_ERROR;
  }
  auto fillable = this->checkFillableClass(made);
  if (fillable.isErr()) {
    this->state.diagnostics.error(cls->loc(), fillable.error().message);

    return std::nullopt;
  }
  made->reflectFill = true;

  return PreludeType::REFLECT_FILL_VALUE_ERROR;
}

void LibraryReflectRegistry::finishFill(const FinishFill &task) {
  CallExpr *call = task.call;
  const Type *fails = this->state.walk.tasks.takeAnswer();
  const Type *made = this->state.types.relations.constructedBy(task.type);
  call->target = BuiltinCall{.builtin = Builtin::ReflectFill};
  call->type = Type::result(made, fails);
}

void LibraryReflectRegistry::startReflectApply(CallExpr *call, MemberExpr *member) {
  auto &tasks = this->state.walk.tasks;
  if (this->hasWrongArity(call, member, 3, 3)) {
    return tasks.pushChecked(call);
  }
  tasks.pushTask(AfterApplyFunction{.call = call});

  return tasks.pushTask(CheckExprSlot{.slot = &call->args[0], .expected = nullptr});
}

void LibraryReflectRegistry::afterApplyFunction(const AfterApplyFunction &task) {
  CallExpr *call = task.call;
  auto &tasks = this->state.walk.tasks;
  const auto *held = this->heldReflectMethod(call->args[0]);
  const Type *gives = this->appliedResult(call->args[0]->type);
  tasks.pushTask(FinishApply{.call = call, .held = held, .gives = gives});
  tasks.pushTask(CalledFunction{});
  tasks.pushTask(ReflectArgsSlot{.slot = &call->args[2]});
  tasks.pushTask(AfterApplyThis{.call = call});
  tasks.pushTask(CheckExprSlot{.slot = &call->args[1], .expected = nullptr});

  return this->walkAppliedFunction(call);
}

void LibraryReflectRegistry::walkAppliedFunction(CallExpr *call) {
  const Type *type = call->args[0]->type;
  if (!type->isError() && !type->isUnknown() && !type->isFunction()) {
    auto message = ErrorTextReflectApply::notFunc(type->name());

    return this->state.diagnostics.error(call->args[0]->loc(), message);
  }

  return this->state.walk.tasks.pushTask(
      ConvertSlot{.slot = &call->args[0], .to = Type::unknown()});
}

void LibraryReflectRegistry::afterApplyThis(const AfterApplyThis &task) {
  CallExpr *call = task.call;
  if (call->args[1]->type->isVoid()) {
    auto message = ErrorTextReflectApply::voidThis();

    return this->state.diagnostics.error(call->args[1]->loc(), message);
  }

  return this->state.walk.tasks.pushTask(
      ConvertSlot{.slot = &call->args[1], .to = Type::unknown()});
}

void LibraryReflectRegistry::finishApply(const FinishApply &task) {
  CallExpr *call = task.call;
  bool isClean = task.held != nullptr && task.gives->isUnknown() &&
                 !call->args[0]->type->isError() && !call->args[1]->type->isError() &&
                 !call->args[2]->type->isError();
  if (isClean && this->canApplyDirectly(call, task.held)) {
    return this->startDirectApply(call, task.held);
  }
  auto &reflection = this->state.library.reflection;
  auto &tasks = this->state.walk.tasks;
  if (!this->state.library.reflect.isOnlyChecked()) {
    reflection.markReflectionUsed();
    reflection.markAnyObjectRead();
  }
  call->target = BuiltinCall{.builtin = Builtin::ReflectApply};
  tasks.pushTask(FinishApplyResult{.call = call, .gives = task.gives});

  return tasks.pushTask(PreludeTypeOf{.type = PreludeType::REFLECT_ERROR});
}

void LibraryReflectRegistry::finishApplyResult(const FinishApplyResult &task) {
  CallExpr *call = task.call;
  auto &tasks = this->state.walk.tasks;
  const Type *errors = tasks.takeAnswer();
  call->type = Type::result(task.gives, errors);

  return tasks.pushChecked(call);
}

bool LibraryReflectRegistry::canApplyDirectly(const CallExpr *call,
                                              const FunctionDecl *functionDecl) const {
  const auto *self = ExprParts::withoutImplicitCasts(call->args[1]);
  const ClassDecl *classDecl = self->type->isOptional() ? nullptr : self->type->classDecl();
  bool isOwnClass = classDecl != nullptr && classDecl->literal() == nullptr &&
                    classDecl->derivesFrom(functionDecl->cls());
  const auto *list = dyn_cast<ArrayLitExpr>(call->args[2]);
  if (!isOwnClass || list == nullptr) {
    return false;
  }
  for (const ArrayLitExpr::Element &element : list->elements) {
    if (element.spread || element.hole) {
      return false;
    }
  }
  const FuncSymbol *function = functionDecl->sym;
  size_t count = list->elements.size();
  size_t fixed = function->hasRest ? function->params.size() - 1 : function->params.size();
  if (count < function->minArgs || (!function->hasRest && count > fixed)) {
    return false;
  }
  for (size_t index = 0; index < count; ++index) {
    const auto *arg = ExprParts::withoutImplicitCasts(list->elements[index].expr);
    const Type *param = index < fixed ? function->params[index] : function->params.back()->inner();
    if (param == nullptr || !this->isReflectAccepting(arg->type, param)) {
      return false;
    }
  }

  return true;
}

void LibraryReflectRegistry::startDirectApply(CallExpr *call, const FunctionDecl *functionDecl) {
  auto &ast = this->state.ast;
  auto &tasks = this->state.walk.tasks;
  auto *self = this->stripImplicit(call->args[1]);
  auto *list = cast<ArrayLitExpr>(call->args[2]);
  FuncSymbol *function = functionDecl->sym;
  auto args = std::vector<Expr *>();
  for (const ArrayLitExpr::Element &element : list->elements) {
    auto *arg = this->stripImplicit(element.expr);
    args.push_back(arg);
  }
  auto *member = ast.make<MemberExpr>(call->loc(), self, functionDecl->name);
  auto *direct = ast.make<CallExpr>(call->loc(), member, std::move(args));
  direct->target = MethodCall{.method = function};
  auto given = std::vector<bool>(direct->args.size(), true);
  tasks.pushTask(AfterDirectArguments{.loc = call->loc()});
  tasks.pushTask(CheckArguments{
      .call = direct,
      .params = function->params,
      .minArgs = function->minArgs,
      .hasRest = function->hasRest,
      .loc = call->loc(),
      .checked = std::move(given),
  });
  tasks.pushTask(AfterDirectMember{.member = member, .direct = direct, .function = function});

  return tasks.pushTask(FunctionTypeOf{.function = function});
}

void LibraryReflectRegistry::afterDirectMember(const AfterDirectMember &task) {
  task.member->type = this->state.walk.tasks.takeAnswer();
  task.direct->type = task.function->getReturnType();
}

void LibraryReflectRegistry::afterDirectArguments(const AfterDirectArguments &task) {
  auto &tasks = this->state.walk.tasks;
  Expr *result = tasks.takeChecked();
  tasks.pushTask(ReflectOk{});
  if (!result->type->isVoid()) {
    tasks.pushChecked(result);

    return tasks.pushTask(Convert{.to = Type::unknown()});
  }
  auto *undefined = this->state.ast.make<UndefinedExpr>(task.loc);
  tasks.pushTask(MakeVoidSequence{.result = result, .loc = task.loc});
  tasks.pushTask(Convert{.to = Type::unknown()});

  return tasks.pushTask(CheckExpr{.expr = undefined, .expected = nullptr});
}

void LibraryReflectRegistry::makeVoidSequence(const MakeVoidSequence &task) {
  auto &tasks = this->state.walk.tasks;
  Expr *nothing = tasks.takeChecked();
  auto parts = std::vector<Expr *>{task.result, nothing};
  auto *sequence = this->state.ast.make<SequenceExpr>(task.loc, std::move(parts));
  sequence->type = Type::unknown();

  return tasks.pushChecked(sequence);
}

void LibraryReflectRegistry::reflectOk() {
  auto &tasks = this->state.walk.tasks;
  Expr *value = tasks.takeChecked();
  if (value->type->isError()) {
    return tasks.pushChecked(value);
  }
  tasks.pushTask(FinishReflectOk{.value = value});

  return tasks.pushTask(PreludeTypeOf{.type = PreludeType::REFLECT_ERROR});
}

void LibraryReflectRegistry::finishReflectOk(const FinishReflectOk &task) {
  Expr *value = task.value;
  auto &ast = this->state.ast;
  auto &tasks = this->state.walk.tasks;
  const Type *errors = tasks.takeAnswer();
  llvm::StringRef okName = GlobalNameTable::getName(GlobalName::OK);
  auto *callee = ast.make<NameExpr>(value->loc(), okName);
  auto *call = ast.make<CallExpr>(value->loc(), callee, std::vector<Expr *>{value});
  call->target = BuiltinCall{.builtin = Builtin::ResultOk};
  call->type = Type::result(value->type, errors);

  return tasks.pushChecked(call);
}

void LibraryReflectRegistry::startReflectConstruct(CallExpr *call, MemberExpr *member,
                                                   const Type *expected) {
  auto &tasks = this->state.walk.tasks;
  if (call->args.size() == 3) {
    auto message = ErrorTextReflectNew::third();
    this->state.diagnostics.error(call->args[2]->loc(), message);
    tasks.pushChecked(call);

    return tasks.pushTask(WalkArgsLoosely{.call = call});
  }
  if (this->hasWrongArity(call, member, 2, 2)) {
    return tasks.pushChecked(call);
  }
  auto *name = dyn_cast<NameExpr>(call->args[0]);
  auto *list = dyn_cast<ArrayLitExpr>(call->args[1]);
  tasks.pushTask(
      AfterConstructClass{.call = call, .name = name, .list = list, .expected = expected});

  return tasks.pushTask(CheckExprSlot{.slot = &call->args[0], .expected = nullptr});
}

void LibraryReflectRegistry::afterConstructClass(const AfterConstructClass &task) {
  CallExpr *call = task.call;
  auto &tasks = this->state.walk.tasks;
  const Type *type = call->args[0]->type;
  if (type->isError()) {
    tasks.pushChecked(call);

    return tasks.pushTask(ReflectArgsSlot{.slot = &call->args[1]});
  }
  if (!type->isClassRef()) {
    auto message = ErrorTextReflectNew::notClass(type->name());
    this->state.diagnostics.error(call->args[0]->loc(), message);
    tasks.pushChecked(call);

    return tasks.pushTask(ReflectArgsSlot{.slot = &call->args[1]});
  }
  bool isLiteral = task.name != nullptr && task.list != nullptr && type->classRef() != nullptr &&
                   !this->hasSpread(task.list);
  if (isLiteral) {
    return this->startLiteralConstruct(call, task.name, task.list, task.expected);
  }

  return this->startDynamicConstruct(call, type);
}

void LibraryReflectRegistry::startLiteralConstruct(CallExpr *call, const NameExpr *name,
                                                   const ArrayLitExpr *list, const Type *expected) {
  auto &ast = this->state.ast;
  auto &tasks = this->state.walk.tasks;
  auto args = std::vector<Expr *>();
  for (const ArrayLitExpr::Element &element : list->elements) {
    args.push_back(element.expr);
  }
  auto *create = ast.make<NewExpr>(call->loc(), "", std::vector<TypeRef>(), std::move(args));
  create->callee = ast.make<NameExpr>(name->loc(), name->name);
  const Type *plain = expected != nullptr ? expected->nonOptional() : nullptr;
  const Type *wanted = plain != nullptr && plain->isResult() ? plain->okType() : nullptr;
  tasks.pushTask(ReflectOk{});

  return tasks.pushTask(CheckValueNew{.newExpr = create, .expected = wanted});
}

void LibraryReflectRegistry::startDynamicConstruct(CallExpr *call, const Type *type) {
  auto &tasks = this->state.walk.tasks;
  const ClassDecl *made = type->classRef();
  if (made != nullptr && made->type == nullptr) {
    auto message = ErrorTextReflectNew::javascript(made->name);
    this->state.diagnostics.error(call->args[0]->loc(), message);
    tasks.pushChecked(call);

    return tasks.pushTask(ReflectArgsSlot{.slot = &call->args[1]});
  }
  bool isKnown = made != nullptr && !made->isAbstract;
  tasks.pushTask(FinishDynamicConstruct{.call = call, .isKnown = isKnown});
  tasks.pushTask(PreludeTypeOf{.type = PreludeType::REFLECT_ERROR});
  tasks.pushTask(CalledFunction{});
  tasks.pushTask(ReflectArgsSlot{.slot = &call->args[1]});
  if (made == nullptr || isKnown) {
    return;
  }
  const Type *constructor = Type::constructorOf(made->type);

  return tasks.pushTask(ConvertSlot{.slot = &call->args[0], .to = constructor});
}

void LibraryReflectRegistry::finishDynamicConstruct(const FinishDynamicConstruct &task) {
  CallExpr *call = task.call;
  auto &tasks = this->state.walk.tasks;
  const Type *errors = tasks.takeAnswer();
  if (!task.isKnown && !this->state.library.reflect.isOnlyChecked()) {
    this->state.library.reflection.markReflectionUsed();
  }
  call->target = BuiltinCall{.builtin = Builtin::ReflectConstruct};
  const Type *constructed = this->state.types.relations.constructedBy(call->args[0]->type);
  call->type = Type::result(constructed, errors);

  return tasks.pushChecked(call);
}

void LibraryReflectRegistry::reflectArgsSlot(const ReflectArgsSlot &task) {
  Expr **slot = task.slot;
  auto &tasks = this->state.walk.tasks;
  if (isa<ArrayLitExpr>(*slot)) {
    const Type *values = Type::array(Type::unknown());

    return tasks.pushTask(CheckExprSlot{.slot = slot, .expected = values});
  }
  tasks.pushTask(AfterReflectArgs{.slot = slot});

  return tasks.pushTask(CheckExprSlot{.slot = slot, .expected = nullptr});
}

void LibraryReflectRegistry::afterReflectArgs(const AfterReflectArgs &task) {
  Expr *args = *task.slot;
  auto list = this->checkArgumentList(args->type);
  if (list.isOk()) {
    return;
  }
  this->state.diagnostics.error(args->loc(), list.error().message);
  args->type = Type::error();
}

void LibraryReflectRegistry::startMetadataCall(CallExpr *call, MemberExpr *member,
                                               const Type *expected, MetadataFunction function) {
  auto &tasks = this->state.walk.tasks;
  tasks.pushChecked(call);
  if (this->state.library.common.hasSpread(call)) {
    return tasks.pushTask(RejectSpreadCall{.call = call});
  }
  auto arity = this->state.library.common.checkArity(call, function.min, function.max);
  if (arity.isErr()) {
    return this->state.diagnostics.error(member->loc(), arity.error().message);
  }
  auto key = this->metadataKeyOf(call);
  bool isNamed = key.key != nullptr;
  auto draft = MetadataCallDraft{
      .call = call,
      .key = std::move(key),
      .builtin = function.builtin,
      .expected = expected,
  };
  if (isNamed) {
    return this->continueMetadataCall(draft);
  }
  tasks.pushTask(AfterMetadataKeyArg{.draft = std::move(draft)});

  return tasks.pushTask(CheckExprSlot{.slot = &call->args[0], .expected = nullptr});
}

MetadataKeyDraft LibraryReflectRegistry::metadataKeyOf(CallExpr *call) {
  Expr *&keyArg = call->args[0];
  auto named = this->metadataKey(keyArg);
  const Type *design = this->designType(keyArg);
  auto key = MetadataKeyDraft{
      .key = named.key,
      .label = named.label,
      .loc = keyArg->loc(),
      .design = design,
  };
  if (key.key == nullptr) {
    return key;
  }
  call->metaKey = key.key;
  keyArg = this->state.ast.make<StringLitExpr>(key.loc, u"");
  keyArg->type = Type::string();

  return key;
}

void LibraryReflectRegistry::afterMetadataKeyArg(const AfterMetadataKeyArg &task) {
  const MetadataCallDraft &draft = task.draft;
  CallExpr *call = draft.call;
  const Expr *keyArg = call->args[0];
  auto records = this->checkKeyOfRecords(keyArg->type);
  if (records.isErr()) {
    this->state.diagnostics.error(draft.key.loc, records.error().message);

    return this->state.walk.tasks.pushTask(WalkArgsLoosely{.call = call});
  }
  bool isDefine = draft.builtin == Builtin::MetaDefine;
  if (isDefine && this->isDesignKey(keyArg)) {
    auto message = ErrorTextMetadataKey::design();
    this->state.diagnostics.error(draft.key.loc, message);
  }

  return this->continueMetadataCall(draft);
}

void LibraryReflectRegistry::continueMetadataCall(const MetadataCallDraft &draft) {
  CallExpr *call = draft.call;
  auto &tasks = this->state.walk.tasks;
  bool isDefine = draft.builtin == Builtin::MetaDefine;
  size_t at = isDefine ? 2 : 1;
  tasks.pushTask(FinishMetadataCall{.draft = draft});
  tasks.pushTask(AfterMetadataTarget{.call = call, .at = at});
  tasks.pushTask(CheckExprSlot{.slot = &call->args[at], .expected = nullptr});
  if (!isDefine) {
    return;
  }

  return this->startMetadataValue(draft);
}

void LibraryReflectRegistry::startMetadataValue(const MetadataCallDraft &draft) {
  auto &tasks = this->state.walk.tasks;
  const void *key = draft.key.key;
  const MetadataType *known =
      key != nullptr ? this->state.library.reflection.findMetadataType(key) : nullptr;
  const Type *hint = known != nullptr ? known->type : nullptr;
  tasks.pushTask(AfterMetadataValue{.draft = draft, .isKnown = known != nullptr});

  return tasks.pushTask(CheckExprSlot{.slot = &draft.call->args[1], .expected = hint});
}

void LibraryReflectRegistry::afterMetadataValue(const AfterMetadataValue &task) {
  const MetadataCallDraft &draft = task.draft;
  Expr **slot = &draft.call->args[1];
  const Expr *value = *slot;
  const Type *type = value->type;
  this->state.walk.tasks.pushTask(MetadataValueUnknown{.slot = slot});
  if (type->isVoid()) {
    auto message = ErrorTextMetadataValue::voidValue();

    return this->state.diagnostics.error(value->loc(), message);
  }
  const MetadataType *known =
      task.isKnown ? this->state.library.reflection.findMetadataType(draft.key.key) : nullptr;
  if (known != nullptr && !type->isError()) {
    return this->pushKnownMetadataCoerce(slot, known, draft.key.label);
  }
  bool isRemembered =
      draft.key.key != nullptr && !type->isError() && !TypeQueries::mentionsParam(type);
  if (isRemembered) {
    return this->rememberMetadataType(value, draft.key);
  }
}

void LibraryReflectRegistry::pushKnownMetadataCoerce(Expr **slot, const MetadataType *known,
                                                     const std::string &label) {
  auto &tasks = this->state.walk.tasks;
  auto records = ErrorTextMetadataValue::label(label);
  this->state.diagnostics.pushList(DiagnosticRoute::PARENT);
  tasks.pushTask(AfterKnownMetadata{
      .label = label,
      .knownType = known->type,
      .knownLoc = known->loc,
  });

  return tasks.pushTask(CoerceSlot{.slot = slot, .to = known->type, .what = records});
}

void LibraryReflectRegistry::afterKnownMetadata(const AfterKnownMetadata &task) {
  auto &diagnostics = this->state.diagnostics;
  auto coerceDiagnostics = diagnostics.popList();
  bool isReachingProgram = diagnostics.getCurrentDestination() == DiagnosticRoute::PROGRAM;
  if (Diagnostics::hasErrors(coerceDiagnostics) && isReachingProgram) {
    auto message = ErrorTextMetadataValue::since(task.label, task.knownType->name());
    diagnostics.note(task.knownLoc, message);
  }
}

void LibraryReflectRegistry::metadataValueUnknown(const MetadataValueUnknown &task) {
  if ((*task.slot)->type->isVoid()) {
    return;
  }

  return this->state.walk.tasks.pushTask(ConvertSlot{.slot = task.slot, .to = Type::unknown()});
}

void LibraryReflectRegistry::rememberMetadataType(const Expr *value, const MetadataKeyDraft &key) {
  if (value->type->isUndefined()) {
    auto message = ErrorTextMetadataValue::undefined(key.label);

    return this->state.diagnostics.error(value->loc(), message);
  }
  this->state.library.reflection.rememberMetadataType(key.key, value->type, value->loc());
}

void LibraryReflectRegistry::afterMetadataTarget(const AfterMetadataTarget &task) {
  CallExpr *call = task.call;
  size_t at = task.at;
  auto &tasks = this->state.walk.tasks;
  const Expr *target = call->args[at];
  auto object = this->checkTargetObject(target->type);
  if (object.isErr()) {
    this->state.diagnostics.error(target->loc(), object.error().message);
  }
  if (call->args.size() == at + 1) {
    auto *nothing = this->state.ast.make<UndefinedExpr>(call->loc());
    call->args.push_back(nothing);
  }
  const Type *name = Type::optional(Type::string());
  tasks.pushTask(CheckArgSlot{.slot = &call->args[at + 1], .to = name});
  if (object.isErr()) {
    return;
  }

  return tasks.pushTask(ConvertSlot{.slot = &call->args[at], .to = Type::anyObject()});
}

void LibraryReflectRegistry::finishMetadataCall(const FinishMetadataCall &task) {
  const MetadataCallDraft &draft = task.draft;
  CallExpr *call = draft.call;
  Builtin builtin = draft.builtin;
  call->target = BuiltinCall{.builtin = builtin};
  if (builtin == Builtin::MetaDefine) {
    call->type = Type::voidTy();

    return;
  }
  bool isReading = builtin == Builtin::MetaGet || builtin == Builtin::MetaGetOwn;
  if (!isReading) {
    call->type = Type::boolTy();

    return;
  }
  if (draft.key.key == nullptr) {
    call->type = draft.key.design != nullptr ? draft.key.design : Type::unknown();

    return;
  }

  return this->nextMetadataWriter(NextMetadataWriter{.draft = draft, .index = 0});
}

void LibraryReflectRegistry::nextMetadataWriter(const NextMetadataWriter &task) {
  const MetadataCallDraft &draft = task.draft;
  auto &reflection = this->state.library.reflection;
  const MetadataType *known = reflection.findMetadataType(draft.key.key);
  if (known != nullptr) {
    draft.call->type = Type::optional(known->type);

    return;
  }
  auto writers = reflection.getMetadataWriters(draft.key.label);
  if (task.index >= writers.size()) {
    return this->finishUnrecorded(draft);
  }
  this->state.walk.tasks.pushTask(NextMetadataWriter{.draft = draft, .index = task.index + 1});

  return this->walkBodyEarly(writers[task.index]);
}

void LibraryReflectRegistry::finishUnrecorded(const MetadataCallDraft &draft) {
  const MetadataKeyDraft &key = draft.key;
  auto written = this->checkWrittenMetadata(draft.expected, key.label);
  if (written.isErr()) {
    this->state.diagnostics.error(key.loc, written.error().message);
    draft.call->type = Type::optional(Type::error());

    return;
  }
  this->state.library.reflection.rememberMetadataType(key.key, written.value(), key.loc);
  draft.call->type = Type::optional(written.value());
}

LibraryReflectRegistry::ResultMetadataTyped
LibraryReflectRegistry::checkWrittenMetadata(const Type *expected, llvm::StringRef label) const {
  bool isWritten = expected != nullptr && !expected->nonOptional()->isUnknown() &&
                   !TypeQueries::mentionsParam(expected);
  if (!isWritten) {
    auto message = ErrorTextMetadataType::unknown(label);

    return err(LibraryReflectRegistryMetadataTypeError{
        .code = LibraryReflectRegistryMetadataTypeErrorCode::NO_RECORD_TYPE,
        .message = message,
    });
  }

  return ok(expected->nonOptional());
}

void LibraryReflectRegistry::walkBodyEarly(FunctionDecl *functionDecl) {
  FuncSymbol *function = functionDecl->sym;
  bool isSettled = function == nullptr || functionDecl->checked || function->isGeneric();
  if (isSettled || this->state.walk.functions.isCheckingFunction(functionDecl)) {
    return;
  }
  auto &tasks = this->state.walk.tasks;
  if (!function->hasReturnType()) {
    return tasks.pushTask(InferReturnType{.function = function});
  }
  if (functionDecl->cls() != nullptr) {
    return;
  }

  return tasks.pushTask(WalkBodyNow{.functionDecl = functionDecl});
}

void LibraryReflectRegistry::noteReflectedConversion(const NoteReflectedConversion &task) {
  const Type *from = task.from;
  const Type *to = task.to;
  auto &reflection = this->state.library.reflection;
  if (this->state.library.reflect.isOnlyChecked()) {
    return;
  }
  const Type *target = to->nonOptional();
  if (target->isConstructor()) {
    auto *classDecl = const_cast<ClassDecl *>(from->nonOptional()->classRef());
    if (classDecl != nullptr) {
      reflection.rememberConstructorCast(task.castExpr, classDecl);
    }

    return;
  }
  bool isUnknownList = target->isArray() && target->inner()->isUnknown();
  if (!target->isAnyObject() && !to->isUnknown() && !isUnknownList) {
    return;
  }
  auto escaped = llvm::SmallPtrSet<ClassDecl *, 8>();
  this->classesIn(from, escaped);
  for (ClassDecl *classDecl : escaped) {
    reflection.addEscapedClass(classDecl);
  }
}

void LibraryReflectRegistry::noteConstructorFlow(const NoteConstructorFlow &task) {
  auto targets = llvm::SmallVector<const Type *, 4>();
  this->constructorsIn(task.to, targets);
  if (targets.empty()) {
    return;
  }
  auto sources = llvm::SmallVector<const Type *, 4>();
  this->constructorsIn(task.from, sources);
  for (const Type *source : sources) {
    for (const Type *target : targets) {
      this->noteWidening(source, target);
    }
  }
}

void LibraryReflectRegistry::writeDesignRecords(const WriteDesignRecords &task) {
  ClassDecl *classDecl = task.classDecl;
  llvm::SMLoc loc = task.loc;
  auto &tasks = this->state.walk.tasks;
  if (task.field != nullptr) {
    tasks.pushTask(AfterFieldComplete{.classDecl = classDecl, .field = task.field, .loc = loc});

    return tasks.pushTask(CompleteClass{.classDecl = classDecl});
  }
  if (task.method != nullptr) {
    return this->writeMethodRecords(classDecl, task.method, loc);
  }
  if (task.constructor == nullptr) {
    return;
  }
  auto conversions = std::vector<SlotConversion>();
  auto *params = this->paramClasses(task.constructor, loc, conversions);
  this->writeDesignRecord(classDecl, {DesignKey::PARAM_TYPES, params, {}, loc}, conversions);

  return this->pushConversions(conversions);
}

void LibraryReflectRegistry::afterFieldComplete(const AfterFieldComplete &task) {
  const ClassField *field = task.field;
  if (field->resolved == nullptr || field->resolved->isError()) {
    return;
  }
  auto conversions = std::vector<SlotConversion>();
  auto *fieldClass = this->designClass(field->resolved, task.loc);
  this->writeDesignRecord(task.classDecl, {DesignKey::TYPE, fieldClass, field->name, task.loc},
                          conversions);

  return this->pushConversions(conversions);
}

void LibraryReflectRegistry::writeMethodRecords(ClassDecl *classDecl, const FunctionDecl *method,
                                                llvm::SMLoc loc) {
  auto conversions = std::vector<SlotConversion>();
  const auto *returnType = method->sym->getReturnType();
  if (returnType != nullptr && !returnType->isVoid() && !returnType->isError()) {
    auto *returnClass = this->designClass(returnType, loc);
    this->writeDesignRecord(classDecl, {DesignKey::RETURN_TYPE, returnClass, method->name, loc},
                            conversions);
  }
  auto *params = this->paramClasses(method, loc, conversions);
  this->writeDesignRecord(classDecl, {DesignKey::PARAM_TYPES, params, method->name, loc},
                          conversions);
  auto *functionConstructor =
      this->state.library.reflect.builtinConstructor(BuiltinConstructor::FUNCTION);
  auto *functionClass = this->classValueAt(functionConstructor, loc);
  this->writeDesignRecord(classDecl, {DesignKey::TYPE, functionClass, method->name, loc},
                          conversions);

  return this->pushConversions(conversions);
}

void LibraryReflectRegistry::writeDesignRecord(ClassDecl *classDecl, const DesignRecord &record,
                                               std::vector<SlotConversion> &conversions) {
  auto &ast = this->state.ast;
  llvm::SMLoc loc = record.loc;
  llvm::StringRef reflect = GlobalNameTable::getName(GlobalName::REFLECT);
  auto *callee = ast.make<NameExpr>(loc, reflect);
  callee->type = Type::error();
  llvm::StringRef keyName = DesignKeyTable::getName(record.key);
  auto keyText = ToUtf16::toUtf16(keyName);
  auto *keyLiteral = ast.make<StringLitExpr>(loc, keyText);
  keyLiteral->type = Type::string();
  bool isClassRef = record.value->type->isClassRef();
  Expr *property = this->memberProperty(record.member, loc);
  auto *owner = this->classValueAt(classDecl, loc);
  auto args = std::vector<Expr *>{keyLiteral, record.value, owner, property};
  auto *call = ast.make<CallExpr>(loc, callee, std::move(args));
  call->target = BuiltinCall{.builtin = Builtin::MetaDefine};
  call->designRecord = true;
  call->type = Type::voidTy();
  auto *statement = ast.make<ExprStmt>(loc, call);
  classDecl->decorations.push_back(statement);
  if (isClassRef) {
    const Type *anyClass = Type::constructorOf(Type::anyObject());
    conversions.push_back({.slot = &call->args[1], .to = anyClass});
  }
  const Type *name = Type::optional(Type::string());
  conversions.push_back({.slot = &call->args[1], .to = Type::unknown()});
  conversions.push_back({.slot = &call->args[2], .to = Type::anyObject()});
  conversions.push_back({.slot = &call->args[3], .to = name});
}

ArrayLitExpr *LibraryReflectRegistry::paramClasses(const FunctionDecl *functionDecl,
                                                   llvm::SMLoc loc,
                                                   std::vector<SlotConversion> &conversions) {
  auto *list = this->state.ast.make<ArrayLitExpr>(loc);
  const auto *any = Type::constructorOf(Type::anyObject());
  for (const Type *param : functionDecl->sym->params) {
    if (param != nullptr) {
      auto *paramClass = this->designClass(param, loc);
      auto element = ArrayLitExpr::Element{.expr = paramClass, .spread = false};
      list->elements.push_back(element);
    }
  }
  for (ArrayLitExpr::Element &element : list->elements) {
    conversions.push_back({.slot = &element.expr, .to = any});
  }
  list->type = Type::array(any);

  return list;
}

void LibraryReflectRegistry::pushConversions(const std::vector<SlotConversion> &conversions) {
  for (const SlotConversion &conversion : llvm::reverse(conversions)) {
    this->state.walk.tasks.pushTask(ConvertSlot{.slot = conversion.slot, .to = conversion.to});
  }
}

Expr *LibraryReflectRegistry::designClass(const Type *type, llvm::SMLoc loc) {
  auto *constructor = this->designConstructor(type);

  return this->classValueAt(constructor, loc);
}

Expr *LibraryReflectRegistry::classValueAt(ClassDecl *classDecl, llvm::SMLoc loc) {
  bool isAnonymous = classDecl->name.empty() && classDecl->type != nullptr;
  llvm::StringRef shown = isAnonymous ? classDecl->type->name() : classDecl->name;
  auto *name = this->state.ast.make<NameExpr>(loc, classDecl->name);
  name->binding = NamedClass{.cls = classDecl};
  name->type = Type::classRef(classDecl, shown);
  classDecl->usedAsValue = true;

  return name;
}

std::vector<FunctionDecl *>
LibraryReflectRegistry::constructorsOf(const ClassDecl *classDecl) const {
  for (const ClassDecl *current = classDecl; current != nullptr; current = current->parent) {
    if (!current->constructors.empty()) {
      return current->constructors;
    }
  }

  return {};
}

void LibraryReflectRegistry::classesIn(const Type *type,
                                       llvm::SmallPtrSetImpl<ClassDecl *> &out) const {
  auto parts = TypeParts::collect(type, kReflectClassesRoute);
  for (const Type *part : parts.getTypes()) {
    auto *classDecl = const_cast<ClassDecl *>(part->classDecl());
    if (classDecl != nullptr && classDecl->literal() == nullptr) {
      out.insert(classDecl);
    }
  }
}

void LibraryReflectRegistry::constructorsIn(const Type *type,
                                            llvm::SmallVectorImpl<const Type *> &out) const {
  auto parts = TypeParts::collect(type, kReflectConstructorsRoute);
  for (const Type *part : parts.getTypes()) {
    if (part->isConstructor()) {
      out.push_back(part->constructed());
    }
  }
}

bool LibraryReflectRegistry::isInstanceOf(const Type *made, const Type *of) const {
  return made == of || of->isAnyObject() || made->isSubtypeOf(of);
}

Expr *LibraryReflectRegistry::stripImplicit(Expr *expr) const {
  return ExprParts::withoutImplicitCasts(expr);
}

void LibraryReflectRegistry::noteWidening(const Type *source, const Type *target) {
  if (source != target && this->isInstanceOf(source, target)) {
    this->state.library.reflection.addConstructorWidening(source, target);
  } else if (source != target && this->isInstanceOf(target, source)) {
    this->state.library.reflection.addConstructorWidening(target, source);
  }
}

void LibraryReflectRegistry::finishReflection() {
  auto &reflect = this->state.library.reflection;
  for (const ConstructorCast &entry : reflect.getConstructorCasts()) {
    entry.cls->reflectNew = true;
  }
  if (reflect.isAnyObjectFilled()) {
    for (ClassDecl *classDecl : this->state.declaration.classes.getClasses()) {
      classDecl->reflectFill = !classDecl->builtin();
    }
  }
  if (!reflect.isReflectionUsed()) {
    for (ClassDecl *classDecl : this->state.declaration.classes.getClasses()) {
      classDecl->reflectNew = classDecl->reflectMethods = false;
    }
    return;
  }
  if (reflect.isAnyObjectRead()) {
    for (ClassDecl *classDecl : reflect.getEscapedClasses()) {
      this->markReflectMethods(classDecl);
    }
    for (ClassDecl *classDecl : this->state.declaration.classes.getClasses()) {
      if (classDecl->reflectNew) {
        this->markReflectMethods(classDecl);
      }
    }
  }
  for (ClassDecl *classDecl : reflect.getReflectedObjects()) {
    this->markReflectMethods(classDecl);
  }
  this->walkReflectNews();
}

void LibraryReflectRegistry::markReflectMethods(ClassDecl *classDecl) const {
  std::vector<ClassDecl *> stack = {classDecl};
  while (!stack.empty()) {
    ClassDecl *next = stack.back();
    stack.pop_back();
    if (!next->reflectMethods) {
      next->reflectMethods = true;
      stack.insert(stack.end(), next->children.rbegin(), next->children.rend());
    }
  }
}

LibraryReflectRegistry::ResultConstructible
LibraryReflectRegistry::checkConstructible(ClassDecl *classDecl,
                                           llvm::ArrayRef<const Type *> args) {
  if (classDecl->builtin()) {
    auto message = ErrorTextConstructible::builtin();

    return err(LibraryReflectRegistryConstructibleError{
        .code = LibraryReflectRegistryConstructibleErrorCode::BUILTIN_CLASS,
        .message = message,
    });
  }
  if (classDecl->isAbstract) {
    auto message = ErrorTextConstructible::abstract();

    return err(LibraryReflectRegistryConstructibleError{
        .code = LibraryReflectRegistryConstructibleErrorCode::ABSTRACT,
        .message = message,
    });
  }
  auto constructors = this->constructorsOf(classDecl);
  if (constructors.empty() && !args.empty()) {
    auto message = ErrorTextConstructible::noArgs();

    return err(LibraryReflectRegistryConstructibleError{
        .code = LibraryReflectRegistryConstructibleErrorCode::NO_ARGUMENTS,
        .message = message,
    });
  }
  if (constructors.empty()) {
    return ok(classDecl);
  }
  size_t fitting = 0;
  auto symbols = std::vector<const FuncSymbol *>();
  for (FunctionDecl *constructor : constructors) {
    FuncSymbol *function = constructor->sym;
    if (function == nullptr) {
      auto message = ErrorTextConstructible::unknown();

      return err(LibraryReflectRegistryConstructibleError{
          .code = LibraryReflectRegistryConstructibleErrorCode::UNKNOWN_CONSTRUCTOR,
          .message = message,
      });
    }
    symbols.push_back(function);
    size_t fixed = function->hasRest ? function->params.size() - 1 : function->params.size();
    bool isFitting =
        args.size() >= function->minArgs && (function->hasRest || args.size() <= fixed);
    for (size_t index = 0; index < args.size() && isFitting; ++index) {
      const Type *param =
          index < fixed ? function->params[index] : function->params.back()->inner();
      isFitting = param != nullptr && this->isReflectAccepting(args[index], param);
    }
    fitting += isFitting;
  }
  if (fitting == 0) {
    auto message = ErrorTextConstructible::takes(symbols);

    return err(LibraryReflectRegistryConstructibleError{
        .code = LibraryReflectRegistryConstructibleErrorCode::NO_FIT,
        .message = message,
    });
  }
  if (fitting > 1) {
    auto message = ErrorTextConstructible::several();

    return err(LibraryReflectRegistryConstructibleError{
        .code = LibraryReflectRegistryConstructibleErrorCode::SEVERAL_FIT,
        .message = message,
    });
  }

  return ok(classDecl);
}

LibraryReflectRegistry::ResultFillableClass
LibraryReflectRegistry::checkFillableClass(ClassDecl *made) const {
  if (made->type == nullptr || made->builtin()) {
    auto why = ErrorTextReflectFill::javascript();
    auto message = ErrorTextReflectFill::cannot(made->name, why);

    return err(LibraryReflectRegistryFillableClassError{
        .code = LibraryReflectRegistryFillableClassErrorCode::JAVASCRIPT_CLASS,
        .message = message,
    });
  }
  if (made->isAbstract) {
    auto why = ErrorTextReflectFill::abstract();
    auto message = ErrorTextReflectFill::cannot(made->name, why);

    return err(LibraryReflectRegistryFillableClassError{
        .code = LibraryReflectRegistryFillableClassErrorCode::ABSTRACT,
        .message = message,
    });
  }
  if (!this->constructorsOf(made).empty()) {
    auto why = ErrorTextReflectFill::ownCtor(made->name);
    auto message = ErrorTextReflectFill::cannot(made->name, why);

    return err(LibraryReflectRegistryFillableClassError{
        .code = LibraryReflectRegistryFillableClassErrorCode::HAS_CONSTRUCTOR,
        .message = message,
    });
  }

  return ok(made);
}

void LibraryReflectRegistry::walkReflectNews() {
  auto &reflect = this->state.library.reflection;
  if (!reflect.hasReflectedNews()) {
    return;
  }
  auto reach = ReachMap();
  auto entries = this->constructorEntries();
  for (const ReflectedNew &reflected : reflect.getReflectedNews()) {
    this->walkReflectedNew(reflected, entries, reach);
  }
}

std::vector<LibraryReflectRegistry::ConstructorEntry>
LibraryReflectRegistry::constructorEntries() const {
  auto entries = std::vector<ConstructorEntry>();
  for (const ConstructorCast &entry : this->state.library.reflection.getConstructorCasts()) {
    const Type *target = entry.cast->type != nullptr ? entry.cast->type->nonOptional() : nullptr;
    if (target != nullptr && target->isConstructor()) {
      entries.push_back({.classDecl = entry.cls, .constructed = target->constructed()});
    }
  }

  return entries;
}

void LibraryReflectRegistry::walkReflectedNew(const ReflectedNew &reflected,
                                              llvm::ArrayRef<ConstructorEntry> entries,
                                              ReachMap &reach) {
  const Type *made = reflected.expr->type;
  if (made == nullptr || made->isError()) {
    return;
  }
  auto classes = this->reachingClasses(made, entries, reach);
  for (ClassDecl *classDecl : classes) {
    auto constructed = this->checkConstructible(classDecl, reflected.args);
    if (constructed.isErr()) {
      auto message =
          ErrorTextReflectNew::mayFail(classDecl->type->name(), constructed.error().message);
      this->state.diagnostics.error(reflected.expr->loc(), message);
      return;
    }
  }
}

std::vector<ClassDecl *>
LibraryReflectRegistry::reachingClasses(const Type *made, llvm::ArrayRef<ConstructorEntry> entries,
                                        ReachMap &reach) const {
  auto classes = std::vector<ClassDecl *>();
  for (const ConstructorEntry &entry : entries) {
    bool isReaching = entry.classDecl->type != nullptr &&
                      this->reachableFrom(entry.constructed, reach).contains(made);
    if (isReaching && !llvm::is_contained(classes, entry.classDecl)) {
      classes.push_back(entry.classDecl);
    }
  }

  return classes;
}

const LibraryReflectRegistry::TypeSet &
LibraryReflectRegistry::reachableFrom(const Type *start, ReachMap &reach) const {
  auto [found, isFresh] = reach.try_emplace(start);
  if (isFresh) {
    this->fillReachable(start, found->second);
  }

  return found->second;
}

void LibraryReflectRegistry::fillReachable(const Type *start, TypeSet &reached) const {
  auto &reflect = this->state.library.reflection;
  auto work = std::vector<const Type *>{start};
  reached.insert(start);
  while (!work.empty()) {
    const Type *current = work.back();
    work.pop_back();
    for (const Type *next : reflect.getConstructorWidenings(current)) {
      bool isInserted = reached.insert(next).second;
      if (isInserted) {
        work.push_back(next);
      }
    }
  }
}

std::optional<std::string> LibraryReflectRegistry::asciiName(const std::u16string &text) const {
  auto name = std::string();
  for (char16_t unit : text) {
    if (unit >= 128) {
      return std::nullopt;
    }
    name.push_back(char(unit));
  }

  return name;
}

void LibraryReflectRegistry::noteReflectedRead(const Type *type, const Type *base) {
  if (this->state.library.reflect.isOnlyChecked()) {
    return;
  }
  if (type->isUnknown() || base->isAnyObject() || base->classDecl() == nullptr) {
    this->state.library.reflection.markAnyObjectRead();
    return;
  }
  auto reflected = llvm::SmallPtrSet<ClassDecl *, 8>();
  this->classesIn(base, reflected);
  for (ClassDecl *reflectedClass : reflected) {
    this->state.library.reflection.addReflectedObject(reflectedClass);
  }
}

const Type *LibraryReflectRegistry::appliedResult(const Type *type) const {
  bool isReturning = type->isFunction() && type->ret() != nullptr && !type->ret()->isError();

  return isReturning ? type->ret() : Type::unknown();
}

bool LibraryReflectRegistry::hasSpread(const ArrayLitExpr *list) const {
  for (const ArrayLitExpr::Element &element : list->elements) {
    if (element.spread) {
      return true;
    }
  }

  return false;
}

LibraryReflectRegistry::ResultKeyOfRecords
LibraryReflectRegistry::checkKeyOfRecords(const Type *type) const {
  if (!type->isString() && !type->isError()) {
    auto message = ErrorTextMetadataKey::type(type->name());

    return err(LibraryReflectRegistryKeyOfRecordsError{
        .code = LibraryReflectRegistryKeyOfRecordsErrorCode::NOT_A_KEY,
        .message = message,
    });
  }

  return ok(type);
}

LibraryReflectRegistry::ResultTargetObject
LibraryReflectRegistry::checkTargetObject(const Type *type) const {
  if (!type->isError() && !this->state.types.relations.isObjectReference(type) &&
      !type->isAnyObject()) {
    auto message = ErrorTextMetadataTarget::notObject(type->name());

    return err(LibraryReflectRegistryTargetObjectError{
        .code = LibraryReflectRegistryTargetObjectErrorCode::NOT_AN_OBJECT,
        .message = message,
    });
  }

  return ok(type);
}

LibraryReflectRegistry::ResultArgumentList
LibraryReflectRegistry::checkArgumentList(const Type *type) const {
  if (type->isError() || type->isArray()) {
    return ok(type);
  }
  if (type->isTuple()) {
    auto message = ErrorTextReflectArgs::tuple();

    return err(LibraryReflectRegistryArgumentListError{
        .code = LibraryReflectRegistryArgumentListErrorCode::TUPLE,
        .message = message,
    });
  }
  auto message = ErrorTextReflectArgs::notArray(type->name());

  return err(LibraryReflectRegistryArgumentListError{
      .code = LibraryReflectRegistryArgumentListErrorCode::NOT_AN_ARRAY,
      .message = message,
  });
}

const Type *LibraryReflectRegistry::designType(const Expr *key) const {
  const auto *literal = dyn_cast<StringLitExpr>(key);
  if (literal == nullptr) {
    return nullptr;
  }
  auto written = ToUtf8::toUtf8(literal->value);
  auto designKey = DesignKeyTable::find(written);
  if (!designKey.has_value()) {
    return nullptr;
  }
  const auto *any = Type::constructorOf(Type::anyObject());
  switch (designKey.value()) {
    case DesignKey::TYPE:
    case DesignKey::RETURN_TYPE:
      return Type::optional(any);
    case DesignKey::PARAM_TYPES:
      return Type::optional(Type::array(any));
  }

  return nullptr;
}

bool LibraryReflectRegistry::isDesignKey(const Expr *key) const {
  const auto *literal = dyn_cast<StringLitExpr>(key);
  if (literal == nullptr) {
    return false;
  }
  auto written = ToUtf8::toUtf8(literal->value);

  return llvm::StringRef(written).starts_with(DesignKeyTable::getPrefix());
}

bool LibraryReflectRegistry::isReflectAccepting(const Type *arg, const Type *param) const {
  const Type *given = arg;
  const Type *wanted = param;
  while (wanted->isOptional()) {
    if (given->isUndefined()) {
      return true;
    }
    given = given->isOptional() ? given->inner() : given;
    wanted = wanted->inner();
  }

  return wanted->isUnknown() || this->isPlainAccepting(given, wanted);
}

bool LibraryReflectRegistry::isPlainAccepting(const Type *arg, const Type *param) const {
  if (arg->isOptional() || arg->isUnion() || param->isUnion()) {
    return false;
  }
  const Type *value = arg->isLiteral() ? arg->literalBase() : arg;
  switch (param->kind()) {
    case Type::Kind::Int:
      return value->isInteger() && param->holds(value);
    case Type::Kind::Double:
      return value->isInteger() || value->isFloat();
    case Type::Kind::Bool:
      return value->isBool();
    case Type::Kind::String:
      return value->isString();
    case Type::Kind::Array:
    case Type::Kind::Function:
    case Type::Kind::Decimal:
    case Type::Kind::Result:
    case Type::Kind::Json:
      return arg == param;
    case Type::Kind::ClassRef:
      return this->isClassRefAccepting(arg, param);
    case Type::Kind::Object:
      return this->isObjectAccepting(arg, param);
    case Type::Kind::Error:
    case Type::Kind::Void:
    case Type::Kind::Undefined:
    case Type::Kind::Optional:
    case Type::Kind::Param:
    case Type::Kind::Never:
    case Type::Kind::Iterator:
    case Type::Kind::Dict:
    case Type::Kind::Literal:
    case Type::Kind::Union:
    case Type::Kind::Promise:
    case Type::Kind::Unknown:
      return false;
  }

  return false;
}

bool LibraryReflectRegistry::isClassRefAccepting(const Type *arg, const Type *param) const {
  const ClassDecl *classDecl = arg->classRef();
  if (classDecl == nullptr || classDecl->type == nullptr) {
    return false;
  }
  if (param->classRef() != nullptr) {
    return classDecl == param->classRef();
  }
  const Type *made = param->constructed();

  return made->isAnyObject() ||
         (made->classDecl() != nullptr && classDecl->type->isSubtypeOf(made));
}

bool LibraryReflectRegistry::isObjectAccepting(const Type *arg, const Type *param) const {
  const ClassDecl *classDecl = arg->classDecl();
  if (classDecl == nullptr || classDecl->literal() != nullptr || classDecl->builtin() ||
      !arg->isClass()) {
    return param->isDate() && arg->isDate();
  }
  if (param->isAnyObject()) {
    return true;
  }

  return (param->isClass() || param->isBase() || !param->bases().empty()) &&
         arg->isSubtypeOf(param);
}

std::optional<LibraryReflectRegistry::KnownMethod>
LibraryReflectRegistry::knownReflectMethod(ClassDecl *classDecl, llvm::StringRef name) {
  auto methods = classDecl->findMethodsNamed(name);
  auto isGeneric = std::bind_front(&LibraryReflectRegistry::isGenericMethod, this);
  llvm::erase_if(methods, isGeneric);
  if (methods.empty()) {
    return std::nullopt;
  }
  auto isCallable = std::bind_front(&LibraryReflectRegistry::isReflectCallable, this, classDecl);
  if (!llvm::all_of(methods, isCallable)) {
    return std::nullopt;
  }
  if (this->isDeclaredBelow(classDecl, name)) {
    return std::nullopt;
  }
  bool hasSeveral = methods.size() > 1;

  return KnownMethod{.method = methods.front(), .hasSeveral = hasSeveral};
}

bool LibraryReflectRegistry::isGenericMethod(const FunctionDecl *functionDecl) const {
  return !functionDecl->typeParams.empty();
}

bool LibraryReflectRegistry::isReflectCallable(ClassDecl *classDecl,
                                               FunctionDecl *functionDecl) const {
  bool isOpen =
      functionDecl->sym != nullptr && !functionDecl->isAbstract() &&
      functionDecl->access() == Access::Public &&
      this->state.library.common.soleImplementation(classDecl, functionDecl) == functionDecl;
  if (!isOpen) {
    return false;
  }
  const Type *returnType = functionDecl->sym->getReturnType();

  return returnType != nullptr && !returnType->isError();
}

bool LibraryReflectRegistry::isDeclaredBelow(const ClassDecl *classDecl,
                                             llvm::StringRef name) const {
  for (const ClassDecl *child : ClassTree::descendants(classDecl)) {
    if (this->hasOwnMethodNamed(child, name)) {
      return true;
    }
  }

  return false;
}

bool LibraryReflectRegistry::hasOwnMethodNamed(const ClassDecl *owner, llvm::StringRef name) const {
  for (const FunctionDecl *method : this->state.library.common.ownMethods(owner)) {
    if (method->name == name) {
      return true;
    }
  }

  return false;
}

const FunctionDecl *LibraryReflectRegistry::heldReflectMethod(const Expr *expr) {
  const auto *bare = ExprParts::withoutImplicitCasts(expr);
  const auto *name = dyn_cast<NameExpr>(bare);
  if (name != nullptr) {
    return name->var() != nullptr ? name->var()->reflectMethod() : nullptr;
  }
  const auto *got = dyn_cast<CallExpr>(bare);
  if (got == nullptr || got->builtin() != Builtin::ReflectMethod ||
      this->state.library.reflection.hasSeveralVariants(got)) {
    return nullptr;
  }
  const auto *object = ExprParts::withoutImplicitCasts(got->args[0]);

  return isa<NameExpr>(object) ? got->method()->decl : nullptr;
}

LibraryReflectRegistry::NamedKey LibraryReflectRegistry::metadataKey(const Expr *key) const {
  auto *name = dyn_cast<NameExpr>(key);
  if (name == nullptr) {
    return NamedKey{.key = nullptr, .label = ""};
  }
  auto entry = this->state.walk.scopes.findName(name->name);

  return NamedKey{.key = this->entryKey(entry), .label = name->name.str()};
}

const void *LibraryReflectRegistry::entryKey(const ScopeEntry &entry) const {
  const VarSymbol *variable = entry.var();
  if (variable != nullptr) {
    bool isKeyConst = variable->isConst && variable->type != nullptr && !variable->type->isString();

    return isKeyConst ? variable : nullptr;
  }
  if (entry.func() != nullptr) {
    return entry.func();
  }

  return entry.cls();
}

ClassDecl *LibraryReflectRegistry::designConstructor(const Type *type) {
  const auto *plain = this->designPlain(type);
  if (!plain->isUnion()) {
    return this->plainConstructor(plain);
  }
  const auto *first = this->designPlain(plain->members().front());
  auto *common = this->plainConstructor(first);
  for (const Type *member : plain->members()) {
    const auto *memberPlain = this->designPlain(member);
    auto *own = this->plainConstructor(memberPlain);
    if (own != common) {
      return this->state.library.reflect.builtinConstructor(BuiltinConstructor::OBJECT);
    }
  }

  return common;
}

const Type *LibraryReflectRegistry::designPlain(const Type *type) const {
  const Type *plain = type->nonOptional();

  return plain->isLiteral() ? plain->literalBase() : plain;
}

ClassDecl *LibraryReflectRegistry::plainConstructor(const Type *type) {
  if (type->isBytes()) {
    return this->state.library.reflect.builtinConstructorNamed(type->name());
  }
  auto builtin = this->builtinDesign(type);
  if (builtin.has_value()) {
    return this->state.library.reflect.builtinConstructor(*builtin);
  }
  auto *classDecl = const_cast<ClassDecl *>(type->classDecl());
  if (classDecl != nullptr && classDecl->literal() == nullptr) {
    return classDecl;
  }

  return this->state.library.reflect.builtinConstructor(BuiltinConstructor::OBJECT);
}

std::optional<BuiltinConstructor> LibraryReflectRegistry::builtinDesign(const Type *type) const {
  if (type->isPromise()) {
    return BuiltinConstructor::PROMISE;
  }
  if (type->isNumeric()) {
    return BuiltinConstructor::NUMBER;
  }
  if (type->isString()) {
    return BuiltinConstructor::STRING;
  }
  if (type->isBool()) {
    return BuiltinConstructor::BOOLEAN;
  }
  if (type->isArray() || type->isTuple() || type->isMatch()) {
    return BuiltinConstructor::ARRAY;
  }
  if (type->isFunction() || type->isClassRef()) {
    return BuiltinConstructor::FUNCTION;
  }
  if (type->isDate()) {
    return BuiltinConstructor::DATE;
  }
  if (type->isRegExp()) {
    return BuiltinConstructor::REG_EXP;
  }
  if (type->isMap()) {
    return BuiltinConstructor::MAP;
  }
  if (type->isSet()) {
    return BuiltinConstructor::SET;
  }

  return std::nullopt;
}

Expr *LibraryReflectRegistry::memberProperty(llvm::StringRef member, llvm::SMLoc loc) {
  auto &ast = this->state.ast;
  if (member.empty()) {
    Expr *nothing = ast.make<UndefinedExpr>(loc);
    nothing->type = Type::undefinedTy();

    return nothing;
  }
  auto text = std::u16string(member.begin(), member.end());
  Expr *property = ast.make<StringLitExpr>(loc, text);
  property->type = Type::string();

  return property;
}

void LibraryReflectRegistry::rememberWriters(SourceFile &program) {
  for (Module *module : program.modules) {
    for (Stmt *item : module->file.items) {
      for (FunctionDecl *functionDecl : this->functionsOf(item)) {
        this->rememberWritersIn(functionDecl);
      }
    }
  }
}

std::vector<FunctionDecl *> LibraryReflectRegistry::functionsOf(Stmt *item) const {
  auto *functionDecl = dyn_cast<FunctionDecl>(item);
  auto *classDecl = dyn_cast<ClassDecl>(item);
  if (functionDecl != nullptr) {
    return {functionDecl};
  }
  if (classDecl != nullptr) {
    return classDecl->methods;
  }

  return {};
}

void LibraryReflectRegistry::rememberWritersIn(FunctionDecl *functionDecl) {
  auto defaults = std::vector<const Expr *>();
  for (const Param &param : functionDecl->params) {
    defaults.push_back(param.defaultValue());
  }
  auto nodes = PlacedNodes::collect({functionDecl->body}, defaults);
  for (const PlacedExpr &placed : nodes.getExprs()) {
    this->rememberWriter(functionDecl, placed.expr);
  }
}

void LibraryReflectRegistry::rememberWriter(FunctionDecl *functionDecl, const Expr *expr) {
  auto *call = dyn_cast<CallExpr>(expr);
  if (call == nullptr || call->args.empty() || !this->isDefiningMetadata(call)) {
    return;
  }
  auto *key = dyn_cast<NameExpr>(call->args[0]);
  if (key != nullptr) {
    this->state.library.reflection.addMetadataWriter(key->name, functionDecl);
  }
}

bool LibraryReflectRegistry::isDefiningMetadata(const CallExpr *call) const {
  auto *member = dyn_cast<MemberExpr>(call->callee);
  if (member == nullptr) {
    return false;
  }
  auto *object = dyn_cast<NameExpr>(member->object);
  if (object == nullptr || object->name != GlobalNameTable::getName(GlobalName::REFLECT)) {
    return false;
  }

  return ReflectFunctionTable::find(member->name) == ReflectFunction::DEFINE_METADATA;
}

}
