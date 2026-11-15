#include "compiler/semantic/library/dispatch/library_dispatch_registry.h"
#include "compiler/program/ast_walker/ast_walker.h"
#include "compiler/program/library/async/timers_promises_function_table/timers_promises_function_table.h"
#include "compiler/program/library/builtin_modules/error_export_table/error_export_table.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/program/library/bytes/buffer_export_table/buffer_export_table.h"
#include "compiler/program/library/crypto/subtle_intrinsic_table/subtle_intrinsic_table.h"
#include "compiler/program/library/fs/fs_function_table/fs_function_table.h"
#include "compiler/program/library/fs/readline_method_table/readline_method_table.h"
#include "compiler/program/library/iterator/iterator_method_table/iterator_method_table.h"
#include "compiler/program/library/library_module_names/library_module_names.h"
#include "compiler/program/library/string/string_method_table/string_method_table.h"
#include "compiler/program/types/argument_lists/argument_lists.h"
#include "llvm/ADT/STLExtras.h"
#include <cassert>

using llvm::dyn_cast;

namespace tz {

LibraryDispatchRegistry::LibraryDispatchRegistry(SemanticState &owner)
    : state(owner) {}

void LibraryDispatchRegistry::importBuiltinModule(const ImportBuiltinModule &task) {
  Module *module = task.module;
  const ImportDecl &importDecl = *task.importDecl;
  auto &here = this->state.declaration.modules.getModuleState(module);
  auto which = KnownModules::moduleOf(importDecl.path);
  if (!which.has_value()) {
    for (const ModuleName &name : importDecl.names) {
      here.badImports.insert(name.as);
    }
    if (!importDecl.whole.empty()) {
      here.badImports.insert(importDecl.whole);
    }
    return;
  }
  if (!importDecl.whole.empty()) {
    this->reportWholeImport(module, importDecl, *which, here);
  }
  for (const ModuleName &name : importDecl.names) {
    this->bindNamedImport(importDecl, name, *which, here);
  }
}

void LibraryDispatchRegistry::processAliasSlot(const ProcessAliasSlot &task) {
  Expr *aliased = this->processAlias(*task.slot);
  *task.slot = aliased;
}

Expr *LibraryDispatchRegistry::processAlias(Expr *expr) {
  auto *name = dyn_cast<NameExpr>(expr);
  const NodeName *nodeName = name != nullptr ? this->state.findNodeName(name->name) : nullptr;
  bool isBuffer = nodeName != nullptr && nodeName->module == NodeModule::Buffer &&
                  BufferExportTable::find(nodeName->name) == BufferExport::BUFFER;
  if (isBuffer) {
    auto bufferName = GlobalNameTable::getName(GlobalName::BUFFER);
    return this->state.ast.make<NameExpr>(name->loc(), bufferName);
  }
  if (nodeName == nullptr || nodeName->module != NodeModule::Process) {
    return expr;
  }
  if (nodeName->name.empty()) {
    auto processName = GlobalNameTable::getName(GlobalName::PROCESS);
    return this->state.ast.make<NameExpr>(name->loc(), processName);
  }

  return this->processMember(this->state.ast, name->loc(), nodeName->name);
}

void LibraryDispatchRegistry::checkNodeName(const CheckNodeName &task) {
  NameExpr *name = task.name;
  auto &tasks = this->state.walk.tasks;
  const NodeName *nodeName = this->state.findNodeName(name->name);
  if (nodeName == nullptr) {
    return tasks.pushChecked(nullptr);
  }
  if (this->isProcessAlias(*nodeName)) {
    Expr *aliased = this->processAlias(name);

    return tasks.pushTask(CheckExpr{.expr = aliased, .expected = nullptr});
  }

  return this->startNodeValue(*nodeName, name);
}

void LibraryDispatchRegistry::startNodeValue(const NodeName &nodeName, Expr *expr) {
  auto &tasks = this->state.walk.tasks;
  expr->type = Type::error();
  auto label = KnownModules::getPath(nodeName.module);
  if (nodeName.name.empty()) {
    bool isPath = this->isPathModule(nodeName.module);
    auto typical = KnownModules::getTypicalName(nodeName.module);
    auto message = ErrorTextNodeValue::notValue(label, isPath, typical);
    this->state.diagnostics.error(expr->loc(), message);

    return tasks.pushChecked(expr);
  }
  NodeModule module = nodeName.module;
  llvm::StringRef name = nodeName.name;
  tasks.pushTask(AfterModuleValue{.expr = expr, .name = name, .module = module});
  if (module == NodeModule::Crypto) {
    return tasks.pushTask(CheckCryptoValue{.name = name, .expr = expr});
  }
  if (this->isPathModule(module)) {
    return tasks.pushTask(CheckPathValue{.module = module, .name = name, .expr = expr});
  }
  if (module == NodeModule::Os) {
    return tasks.pushTask(CheckOsValue{.name = name, .expr = expr});
  }
  if (module == NodeModule::Fs || module == NodeModule::FsPromises) {
    return tasks.pushTask(CheckFsValue{.name = name, .expr = expr});
  }

  return tasks.pushChecked(nullptr);
}

void LibraryDispatchRegistry::afterModuleValue(const AfterModuleValue &task) {
  auto &tasks = this->state.walk.tasks;
  Expr *value = tasks.takeChecked();
  if (value != nullptr) {
    return tasks.pushChecked(value);
  }
  auto label = KnownModules::getPath(task.module);
  auto message = ErrorTextNodeValue::onlyCalled(task.name, label);
  this->state.diagnostics.error(task.expr->loc(), message);

  return tasks.pushChecked(task.expr);
}

void LibraryDispatchRegistry::checkNodeMember(const CheckNodeMember &task) {
  MemberExpr *member = task.member;
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(AfterCryptoModuleMember{.member = member});

  return tasks.pushTask(CheckCryptoModuleMember{.member = member});
}

void LibraryDispatchRegistry::afterCryptoModuleMember(const AfterCryptoModuleMember &task) {
  MemberExpr *member = task.member;
  auto &tasks = this->state.walk.tasks;
  Expr *cryptoMember = tasks.takeChecked();
  if (cryptoMember != nullptr) {
    return tasks.pushChecked(cryptoMember);
  }
  tasks.pushTask(AfterFsConstant{.member = member});

  return tasks.pushTask(CheckFsConstant{.member = member});
}

void LibraryDispatchRegistry::afterFsConstant(const AfterFsConstant &task) {
  MemberExpr *member = task.member;
  auto &tasks = this->state.walk.tasks;
  Expr *fsConstant = tasks.takeChecked();
  if (fsConstant != nullptr) {
    return tasks.pushChecked(fsConstant);
  }
  auto *object = dyn_cast<NameExpr>(member->object);
  const NodeName *nodeName = object != nullptr ? this->state.findNodeName(object->name) : nullptr;
  if (nodeName == nullptr) {
    return tasks.pushChecked(nullptr);
  }
  if (this->isProcessAlias(*nodeName)) {
    member->object = this->processAlias(object);

    return tasks.pushChecked(nullptr);
  }
  if (!nodeName->name.empty()) {
    return tasks.pushChecked(nullptr);
  }
  auto path = KnownModules::getPath(nodeName->module);
  auto found = this->state.library.modules.checkExportFound(nodeName->module, member->name, path);
  if (found.isErr()) {
    this->state.diagnostics.error(member->loc(), found.error().message);
    member->type = Type::error();

    return tasks.pushChecked(member);
  }
  NodeName inner = this->importedNode(nodeName->module, member->name);

  return this->startNodeValue(inner, member);
}

void LibraryDispatchRegistry::checkNodeCall(const CheckNodeCall &task) {
  CallExpr *call = task.call;
  auto &tasks = this->state.walk.tasks;
  if (call->optional) {
    return tasks.pushChecked(nullptr);
  }
  auto *name = dyn_cast<NameExpr>(call->callee);
  if (name != nullptr) {
    return this->startNodeNameCall(call, name, task.expected);
  }
  auto *member = dyn_cast<MemberExpr>(call->callee);
  if (member == nullptr || member->optional) {
    return tasks.pushChecked(nullptr);
  }

  return this->startNodeMemberCall(call, member, task.expected);
}

void LibraryDispatchRegistry::failNodeCall(CallExpr *call) {
  auto &tasks = this->state.walk.tasks;
  call->type = Type::error();
  tasks.pushChecked(call);
  for (Expr *&arg : llvm::reverse(call->args)) {
    tasks.pushTask(CheckExprSlot{.slot = &arg, .expected = nullptr});
  }
}

void LibraryDispatchRegistry::startNodeNameCall(CallExpr *call, NameExpr *name,
                                                const Type *expected) {
  auto &tasks = this->state.walk.tasks;
  const NodeName *nodeName = this->state.findNodeName(name->name);
  if (nodeName == nullptr) {
    return tasks.pushChecked(nullptr);
  }
  if (this->isProcessAlias(*nodeName)) {
    call->callee = this->processAlias(name);

    return tasks.pushChecked(nullptr);
  }
  if (nodeName->name.empty()) {
    auto path = KnownModules::getPath(nodeName->module);
    auto message = ErrorTextNodeValue::notFunc(name->name, path);
    this->state.diagnostics.error(name->loc(), message);

    return this->failNodeCall(call);
  }

  return this->dispatchNodeCall(call, *nodeName, name->loc(), expected);
}

void LibraryDispatchRegistry::startNodeMemberCall(CallExpr *call, MemberExpr *member,
                                                  const Type *expected) {
  auto &tasks = this->state.walk.tasks;
  auto *object = dyn_cast<NameExpr>(member->object);
  const NodeName *nodeName = object != nullptr ? this->state.findNodeName(object->name) : nullptr;
  if (nodeName == nullptr) {
    return tasks.pushChecked(nullptr);
  }
  if (this->isProcessAlias(*nodeName)) {
    member->object = this->processAlias(object);

    return tasks.pushChecked(nullptr);
  }
  bool isScheduler =
      TimersPromisesFunctionTable::find(nodeName->name) == TimersPromisesFunction::SCHEDULER;
  if (nodeName->module == NodeModule::TimersPromises && isScheduler) {
    return tasks.pushTask(CheckSchedulerCall{.call = call, .member = member});
  }
  if (!nodeName->name.empty()) {
    return tasks.pushChecked(nullptr);
  }
  auto path = KnownModules::getPath(nodeName->module);
  auto found = this->state.library.modules.checkExportFound(nodeName->module, member->name, path);
  if (found.isErr()) {
    this->state.diagnostics.error(member->loc(), found.error().message);

    return this->failNodeCall(call);
  }
  NodeName target{nodeName->module, member->name};

  return this->dispatchNodeCall(call, target, member->loc(), expected);
}

void LibraryDispatchRegistry::dispatchNodeCall(CallExpr *call, const NodeName &target,
                                               llvm::SMLoc loc, const Type *expected) {
  auto &tasks = this->state.walk.tasks;
  NodeModule module = target.module;
  llvm::StringRef functionName = target.name;
  if (module == NodeModule::TimersPromises) {
    return tasks.pushTask(CheckTimersPromisesCall{
        .call = call, .functionName = functionName, .loc = loc, .expected = expected});
  }
  if (module == NodeModule::Crypto || module == NodeModule::TzCrypto) {
    return tasks.pushTask(CheckCryptoCall{.call = call, .functionName = functionName, .loc = loc});
  }
  if (this->isPathModule(module)) {
    return tasks.pushTask(
        CheckPathCall{.call = call, .module = module, .functionName = functionName, .loc = loc});
  }
  if (module == NodeModule::Os) {
    return tasks.pushTask(CheckOsCall{.call = call, .functionName = functionName, .loc = loc});
  }
  bool isTzFs = module == NodeModule::TzFs;
  auto syncName = FsFunctionTable::getSyncName(functionName);
  bool isReplaceSync = FsFunctionTable::find(functionName) == FsFunction::REPLACE_SYNC;
  bool isReplace = FsFunctionTable::find(syncName) == FsFunction::REPLACE_SYNC;
  if (module == NodeModule::Fs || (isTzFs && isReplaceSync)) {
    return tasks.pushTask(
        CheckFsModuleCall{.call = call, .functionName = functionName, .loc = loc});
  }
  if (module == NodeModule::FsPromises || (isTzFs && isReplace)) {
    return tasks.pushTask(
        CheckFsPromisesCall{.call = call, .functionName = functionName, .loc = loc});
  }
  if (module == NodeModule::Readline || module == NodeModule::ReadlinePromises) {
    return tasks.pushTask(CheckReadlineCall{
        .call = call, .module = module, .functionName = functionName, .loc = loc});
  }
  if (module == NodeModule::TzIo) {
    return tasks.pushTask(CheckTzIoCall{.call = call, .loc = loc});
  }
  if (module == NodeModule::ChildProcess) {
    return tasks.pushTask(CheckChildCall{.call = call, .functionName = functionName, .loc = loc});
  }

  return tasks.pushChecked(nullptr);
}

void LibraryDispatchRegistry::checkLibCall(const CheckLibCall &task) {
  CallExpr *call = task.call;
  NameExpr *name = task.name;
  auto &tasks = this->state.walk.tasks;
  auto own = this->state.walk.scopes.findName(name->name);
  const FunctionDecl *functionDecl = own.func() != nullptr ? own.func()->decl : nullptr;
  if (functionDecl == nullptr || !functionDecl->intrinsic()) {
    return tasks.pushChecked(nullptr);
  }
  llvm::StringRef intrinsicName = functionDecl->name;
  call->type = Type::error();
  if (SubtleIntrinsicTable::isSubtleName(intrinsicName)) {
    return tasks.pushTask(
        CheckSubtleIntrinsic{.call = call, .name = name, .intrinsicName = intrinsicName});
  }
  const auto *intrinsic = LibraryIntrinsicTable::find(intrinsicName);
  bool isKnown = intrinsic != nullptr;
  unsigned args = isKnown ? intrinsic->args : 0;
  unsigned typeArgs = isKnown ? intrinsic->typeArgs : 0;
  auto shape = this->checkIntrinsicShape(call, name, intrinsicName, isKnown, args, typeArgs);
  if (shape.isErr()) {
    this->state.diagnostics.error(name->loc(), shape.error().message);

    return this->failNodeCall(call);
  }
  call->target = BuiltinCall{.builtin = intrinsic->builtin};
  auto draft = IntrinsicDraft{
      .call = call,
      .name = name,
      .intrinsicName = intrinsicName,
      .intrinsic = intrinsic->intrinsic,
  };
  if (intrinsic->typeArgs == 0) {
    return this->startIntrinsic(draft, nullptr);
  }
  tasks.pushTask(AfterIntrinsicType{.draft = draft});

  return tasks.pushTask(ResolveTypeAnswer{.ref = &name->typeArgs[0], .use = TypeUse::STANDALONE});
}

void LibraryDispatchRegistry::afterIntrinsicType(const AfterIntrinsicType &task) {
  const Type *typeArg = this->state.walk.tasks.takeAnswer();

  return this->startIntrinsic(task.draft, typeArg);
}

void LibraryDispatchRegistry::startIntrinsic(const IntrinsicDraft &draft, const Type *typeArg) {
  CallExpr *call = draft.call;
  auto &tasks = this->state.walk.tasks;
  switch (draft.intrinsic) {
    case LibraryIntrinsic::AS_LISTENER:
    case LibraryIntrinsic::AS_BOUND_LISTENER:
      return this->startListenerIntrinsic(draft, typeArg);
    case LibraryIntrinsic::AS_CLASS:
      tasks.pushChecked(call);
      tasks.pushTask(FinishAsClass{.draft = draft, .typeArg = typeArg});
      return tasks.pushTask(CheckExprSlot{.slot = &call->args[0], .expected = nullptr});
    case LibraryIntrinsic::AHEAD:
    case LibraryIntrinsic::AHEAD_MOVABLE:
      tasks.pushChecked(call);
      tasks.pushTask(FinishAhead{.draft = draft});
      return tasks.pushTask(CheckExprSlot{.slot = &call->args[0], .expected = nullptr});
    case LibraryIntrinsic::ORDER:
      call->type = Type::voidTy();
      return tasks.pushChecked(call);
    case LibraryIntrinsic::PANIC:
      call->type = Type::voidTy();
      tasks.pushChecked(call);
      return tasks.pushTask(CheckArgSlot{.slot = &call->args[0], .to = Type::string()});
    case LibraryIntrinsic::DEFER: {
      call->type = Type::anyObject();
      tasks.pushChecked(call);
      const Type *deferred = Type::function({}, 0, false, Type::voidTy(), false);
      return tasks.pushTask(CheckArgSlot{.slot = &call->args[0], .to = deferred});
    }
    case LibraryIntrinsic::WITH_CONTEXT:
      tasks.pushChecked(call);
      tasks.pushTask(FinishWithContext{.draft = draft});
      tasks.pushTask(CheckExprSlot{.slot = &call->args[1], .expected = nullptr});
      return tasks.pushTask(CheckArgSlot{.slot = &call->args[0], .to = Type::anyObject()});
    case LibraryIntrinsic::CHUNK_OF:
    case LibraryIntrinsic::IS_CHUNK:
    case LibraryIntrinsic::CHUNK_KIND:
    case LibraryIntrinsic::TYPE_NAME:
    case LibraryIntrinsic::OVERRIDES:
    case LibraryIntrinsic::FS_OPEN:
    case LibraryIntrinsic::FS_READ:
    case LibraryIntrinsic::FS_WRITE:
    case LibraryIntrinsic::FS_CLOSE:
    case LibraryIntrinsic::STDIN_CHUNK:
    case LibraryIntrinsic::STDIN_FAILED:
    case LibraryIntrinsic::STD_WRITE:
      return tasks.pushTask(
          CheckStreamIntrinsic{.call = call, .name = draft.name, .typeArg = typeArg});
  }
}

void LibraryDispatchRegistry::startListenerIntrinsic(const IntrinsicDraft &draft,
                                                     const Type *typeArg) {
  CallExpr *call = draft.call;
  auto &tasks = this->state.walk.tasks;
  const Type *functionType = this->flatListenerType(typeArg);
  tasks.pushChecked(call);
  tasks.pushTask(FinishListenerIntrinsic{.draft = draft, .functionType = functionType});
  if (draft.intrinsic == LibraryIntrinsic::AS_BOUND_LISTENER) {
    tasks.pushTask(CheckArgSlot{.slot = &call->args[1], .to = Type::anyObject()});
  }

  return tasks.pushTask(CheckArgSlot{.slot = &call->args[0], .to = Type::unknown()});
}

void LibraryDispatchRegistry::finishListenerIntrinsic(const FinishListenerIntrinsic &task) {
  const Type *functionType = task.functionType;
  task.draft.call->type = functionType;
  bool isFunction =
      functionType->isFunction() || functionType->isParam() || functionType->isError();
  if (isFunction) {
    return;
  }
  auto message = ErrorTextLibraryCall::listener();

  return this->state.diagnostics.error(task.draft.name->loc(), message);
}

void LibraryDispatchRegistry::finishAsClass(const FinishAsClass &task) {
  const Type *typeArg = task.typeArg;
  bool isClass = typeArg->isClass() || typeArg->isParam() || typeArg->isError();
  if (!isClass) {
    auto message = ErrorTextLibraryCall::asClass();
    this->state.diagnostics.error(task.draft.name->loc(), message);
  }
  task.draft.call->type = Type::optional(typeArg);
}

void LibraryDispatchRegistry::finishAhead(const FinishAhead &task) {
  CallExpr *call = task.draft.call;
  call->type = call->args[0]->type;
  if (!this->isAwaitable(call->type)) {
    auto message = ErrorTextLibraryCall::ahead(task.draft.intrinsicName);
    this->state.diagnostics.error(task.draft.name->loc(), message);
  }
  auto *current = this->state.walk.functions.getCurrentFunction();
  if (current != nullptr && current->decl != nullptr) {
    current->decl->awaitsAhead = true;
  }
}

void LibraryDispatchRegistry::finishWithContext(const FinishWithContext &task) {
  CallExpr *call = task.draft.call;
  const Type *body = call->args[1]->type;
  bool isPlainFunction = body->isFunction() && body->params().empty();
  call->type = isPlainFunction ? body->ret() : Type::error();
  if (call->type->isError() && !body->isError()) {
    auto message = ErrorTextLibraryCall::context();
    this->state.diagnostics.error(task.draft.name->loc(), message);
  }
}

void LibraryDispatchRegistry::namedModuleType(const NamedModuleType &task) {
  auto &tasks = this->state.walk.tasks;
  TypeAliasDecl *alias = this->state.library.declarations.findModuleAlias(task.name);
  assert(alias != nullptr && "a built-in module declares the type");
  tasks.pushTask(AnswerAliasType{.alias = alias});

  return tasks.pushTask(ResolveAlias{.alias = alias});
}

void LibraryDispatchRegistry::checkRuntimeMember(const CheckRuntimeMember &task) {
  MemberExpr *member = task.member;
  const Type *type = member->object->type;
  auto message = ErrorTextRuntimeObject::noMember(member->name, type->name(), type->runtimeKind());
  this->state.diagnostics.error(member->loc(), message);

  return this->state.walk.tasks.pushChecked(member);
}

void LibraryDispatchRegistry::checkRuntimeTarget(const CheckRuntimeTarget &task) {
  MemberExpr *member = task.member;
  const Type *type = member->object->type;
  auto message = ErrorTextRuntimeObject::assigned(member->name, type->name(), type->runtimeKind());
  this->state.diagnostics.error(member->loc(), message);

  return this->state.walk.tasks.pushAnswer(Type::error());
}

void LibraryDispatchRegistry::checkRuntimeMethod(const CheckRuntimeMethod &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  auto &tasks = this->state.walk.tasks;
  call->type = Type::error();
  tasks.pushChecked(call);
  if (this->state.library.common.hasSpread(call)) {
    return tasks.pushTask(RejectSpreadCall{.call = call});
  }
  switch (member->object->type->runtimeKind()) {
    case Type::Runtime::Readline:
    case Type::Runtime::ReadlinePromises:
      return this->startReadlineMethod(call, member);
    case Type::Runtime::AsyncIterator:
      return this->startAsyncIteratorMethod(call, member);
    case Type::Runtime::CString:
      return this->startCStringMethod(call, member);
    case Type::Runtime::None:
    case Type::Runtime::FileHandle:
      return this->reportNoMember(call, member);
  }
}

void LibraryDispatchRegistry::pushLooseArgs(CallExpr *call) {
  for (Expr *&arg : llvm::reverse(call->args)) {
    this->state.walk.tasks.pushTask(CheckExprSlot{.slot = &arg, .expected = nullptr});
  }
}

void LibraryDispatchRegistry::reportNoMember(CallExpr *call, MemberExpr *member) {
  const Type *type = member->object->type;
  auto message = ErrorTextRuntimeObject::noMember(member->name, type->name(), type->runtimeKind());
  this->state.diagnostics.error(member->loc(), message);

  return this->pushLooseArgs(call);
}

bool LibraryDispatchRegistry::hasWrongArity(CallExpr *call, MemberExpr *member, unsigned min,
                                            unsigned max) {
  auto arity = this->state.library.common.checkArity(call, min, max);
  if (arity.isErr()) {
    this->state.diagnostics.error(member->loc(), arity.error().message);
  }

  return arity.isErr();
}

void LibraryDispatchRegistry::startReadlineMethod(CallExpr *call, MemberExpr *member) {
  auto method = ReadlineMethodTable::find(member->name);
  if (!method.has_value()) {
    return this->reportNoMember(call, member);
  }
  switch (*method) {
    case ReadlineMethod::CLOSE:
      return this->checkReadlineClose(call, member);
    case ReadlineMethod::QUESTION:
      return this->startQuestion(call, member);
    case ReadlineMethod::ON:
    case ReadlineMethod::ONCE:
    case ReadlineMethod::ADD_LISTENER:
    case ReadlineMethod::OFF:
    case ReadlineMethod::REMOVE_LISTENER:
    case ReadlineMethod::REMOVE_ALL_LISTENERS:
    case ReadlineMethod::EMIT:
    case ReadlineMethod::PREPEND_LISTENER:
    case ReadlineMethod::PREPEND_ONCE_LISTENER:
    case ReadlineMethod::LISTENERS:
    case ReadlineMethod::LISTENER_COUNT:
    case ReadlineMethod::ADD_EVENT_LISTENER:
    case ReadlineMethod::REMOVE_EVENT_LISTENER:
    case ReadlineMethod::DISPATCH_EVENT:
      return this->reportReadlineEvents(call, member);
  }
}

void LibraryDispatchRegistry::startCStringMethod(CallExpr *call, MemberExpr *member) {
  const auto *method = StringMethodTable::find(member->name);
  bool isToString = method != nullptr && method->method == StringMethod::TO_STRING;
  if (!isToString) {
    return this->reportNoMember(call, member);
  }
  if (this->hasWrongArity(call, member, 0, 0)) {
    return;
  }
  call->target = BuiltinCall{.builtin = Builtin::CStrToString};
  call->type = Type::string();
}

void LibraryDispatchRegistry::reportReadlineEvents(CallExpr *call, MemberExpr *member) {
  auto message = ErrorTextReadlineMethod::events(member->name);
  this->state.diagnostics.error(member->loc(), message);

  return this->pushLooseArgs(call);
}

void LibraryDispatchRegistry::checkReadlineClose(CallExpr *call, MemberExpr *member) {
  if (this->hasWrongArity(call, member, 0, 0)) {
    return;
  }
  call->target = BuiltinCall{.builtin = Builtin::RlClose};
  call->type = Type::voidTy();
}

void LibraryDispatchRegistry::startQuestion(CallExpr *call, MemberExpr *member) {
  auto &tasks = this->state.walk.tasks;
  bool hasPromises = member->object->type->runtimeKind() == Type::Runtime::ReadlinePromises;
  if (this->hasWrongArity(call, member, 1, hasPromises ? 1 : 3)) {
    return;
  }
  tasks.pushTask(AfterQuestionQuery{.call = call, .member = member, .hasPromises = hasPromises});
  tasks.pushTask(Coerce{.to = Type::string(), .what = ""});

  return tasks.pushTask(CheckExpr{.expr = call->args[0], .expected = Type::string()});
}

void LibraryDispatchRegistry::afterQuestionQuery(const AfterQuestionQuery &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  auto &tasks = this->state.walk.tasks;
  Expr *query = tasks.takeChecked();
  if (!task.hasPromises) {
    return this->startQuestionCallback(call, member, query);
  }
  call->args = {query};
  call->target = BuiltinCall{.builtin = Builtin::RlQuestion};
  tasks.pushTask(FinishQuestionPromise{.call = call});

  return tasks.pushTask(PreludeTypeOf{.type = PreludeType::QUESTION_ERROR});
}

void LibraryDispatchRegistry::finishQuestionPromise(const FinishQuestionPromise &task) {
  const Type *failure = this->state.walk.tasks.takeAnswer();
  const Type *result = Type::result(Type::string(), failure);
  task.call->type = Type::promise(result);
}

void LibraryDispatchRegistry::startQuestionCallback(CallExpr *call, MemberExpr *member,
                                                    Expr *query) {
  auto &tasks = this->state.walk.tasks;
  if (call->args.size() < 2) {
    auto message = ErrorTextReadlineMethod::callback();

    return this->state.diagnostics.error(member->loc(), message);
  }
  if (call->args.size() == 3) {
    auto message = ErrorTextReadlineMethod::options();
    this->state.diagnostics.error(call->args[1]->loc(), message);

    return this->pushLooseArgs(call);
  }
  Signature signature;
  signature.params = {Type::string()};
  signature.ret = Type::voidTy();
  signature.voidResult = true;
  signature.asyncVoid = true;
  tasks.pushTask(FinishQuestionCallback{.call = call, .query = query});

  return tasks.pushTask(CheckFunctionArgSlot{
      .slot = &call->args[1], .expected = signature, .primary = 1, .method = member->name});
}

void LibraryDispatchRegistry::finishQuestionCallback(const FinishQuestionCallback &task) {
  CallExpr *call = task.call;
  Expr *callback = call->args[1];
  if (!callback->type->isFunction()) {
    return;
  }
  auto *functionExpr = dyn_cast<FunctionExpr>(callback);
  if (functionExpr != nullptr) {
    functionExpr->fn->nonEscaping = false;
  }
  call->args = {task.query, callback};
  call->target = BuiltinCall{.builtin = Builtin::RlQuestionCallback};
  call->type = Type::voidTy();
}

void LibraryDispatchRegistry::startAsyncIteratorMethod(CallExpr *call, MemberExpr *member) {
  const auto *method = IteratorMethodTable::find(member->name);
  auto which = method != nullptr ? std::optional<IteratorMethod>(method->method) : std::nullopt;
  if (which == IteratorMethod::THROW) {
    auto message = ErrorTextAsyncIterator::noThrow();
    this->state.diagnostics.error(member->loc(), message);

    return this->pushLooseArgs(call);
  }
  if (which == IteratorMethod::NEXT || which == IteratorMethod::RETURN) {
    auto message = ErrorTextAsyncIterator::notYet(member->name);
    this->state.diagnostics.error(member->loc(), message);

    return this->pushLooseArgs(call);
  }

  return this->reportNoMember(call, member);
}

bool LibraryDispatchRegistry::isPathModule(NodeModule module) const {
  return module == NodeModule::Path || module == NodeModule::PathPosix ||
         module == NodeModule::PathWin32;
}

MemberExpr *LibraryDispatchRegistry::processMember(AstContext &context, llvm::SMLoc loc,
                                                   llvm::StringRef name) const {
  auto processName = GlobalNameTable::getName(GlobalName::PROCESS);
  auto *process = context.make<NameExpr>(loc, processName);

  return context.make<MemberExpr>(loc, process, name);
}

void LibraryDispatchRegistry::bindNodeName(ModuleState &here, const ImportDecl &importDecl,
                                           const NodeBinding &binding) {
  auto own = here.declared.find(binding.alias);
  if (own != here.declared.end()) {
    auto message = ErrorTextNodeImport::conflict(binding.alias, binding.name, importDecl.path);
    this->state.diagnostics.error(binding.loc, message);
    auto declared = ErrorTextNodeImport::declared(binding.alias);
    this->state.diagnostics.note(own->second, declared);
    return;
  }
  bool isTaken = here.imports.count(binding.alias) != 0;
  if (!isTaken) {
    auto [_, isEmplaced] = here.nodeNames.try_emplace(binding.alias, binding.node);
    isTaken = !isEmplaced;
  }
  if (isTaken) {
    auto message = ErrorTextNodeImport::duplicate(binding.alias);
    this->state.diagnostics.error(binding.loc, message);
  }
}

void LibraryDispatchRegistry::reportWholeImport(Module *module, const ImportDecl &importDecl,
                                                NodeModule which, ModuleState &here) {
  auto used = this->wholeImportUses(module, importDecl.whole);
  auto names = ErrorTextNodeImport::list(used);
  auto form = ErrorTextNodeImport::form(importDecl.wholeIsDefault);
  auto message = this->wholeImportMessage(importDecl, which, names, form);
  this->state.diagnostics.error(importDecl.wholeLoc, message);
  NodeBinding binding{
      .alias = importDecl.whole,
      .loc = importDecl.wholeLoc,
      .node = NodeName{which, {}},
      .name = KnownModules::getTypicalName(which),
  };
  this->bindNodeName(here, importDecl, binding);
}

std::vector<llvm::StringRef> LibraryDispatchRegistry::wholeImportUses(Module *module,
                                                                      llvm::StringRef whole) const {
  auto used = std::vector<llvm::StringRef>();
  for (Stmt *item : module->file.items) {
    for (const Expr *expr : AstWalker::allExprsIn(item)) {
      auto *member = dyn_cast<MemberExpr>(expr);
      auto *object = member != nullptr ? dyn_cast<NameExpr>(member->object) : nullptr;
      if (object != nullptr && object->name == whole && !llvm::is_contained(used, member->name)) {
        used.push_back(member->name);
      }
    }
  }

  return used;
}

std::string LibraryDispatchRegistry::wholeImportMessage(const ImportDecl &importDecl,
                                                        NodeModule which, const std::string &names,
                                                        const std::string &form) const {
  bool isProcessName = GlobalNameTable::find(importDecl.whole) == GlobalName::PROCESS;
  if (which == NodeModule::Process && isProcessName) {
    return ErrorTextNodeImport::process(form);
  }
  if (which == NodeModule::Buffer) {
    return ErrorTextNodeImport::buffer(form, importDecl.path);
  }
  if (!names.empty()) {
    return ErrorTextNodeImport::uses(form, names, importDecl.path);
  }
  auto typical = KnownModules::getTypicalName(which);

  return ErrorTextNodeImport::typical(form, typical, importDecl.path);
}

void LibraryDispatchRegistry::bindNamedImport(const ImportDecl &importDecl, const ModuleName &name,
                                              NodeModule which, ModuleState &here) {
  auto found = this->state.library.modules.checkExportFound(which, name.name, importDecl.path);
  if (found.isErr()) {
    this->state.diagnostics.error(name.loc, found.error().message);
    here.badImports.insert(name.as);
    return;
  }
  NodeBinding binding{
      .alias = name.as,
      .loc = name.asLoc,
      .node = this->importedNode(which, name.name),
      .name = name.name,
  };
  this->bindNodeName(here, importDecl, binding);
}

NodeName LibraryDispatchRegistry::importedNode(NodeModule which, llvm::StringRef name) const {
  auto submodule = KnownModules::findSubmodule(which, name);
  if (submodule.has_value()) {
    return NodeName{*submodule, {}};
  }

  return NodeName{which, name};
}

bool LibraryDispatchRegistry::isProcessAlias(const NodeName &nodeName) const {
  return nodeName.module == NodeModule::Process ||
         (nodeName.module == NodeModule::Buffer && !nodeName.name.empty());
}

void LibraryDispatchRegistry::prepareLibraries(SourceFile &program) {
  for (Module *module : program.modules) {
    if (module->builtin) {
      this->prepareLibrary(module);
    }
  }
}

void LibraryDispatchRegistry::prepareLibrary(Module *module) {
  this->state.inventory.comparesFunctions = true;
  llvm::SourceMgr &sources = this->state.diagnostics.sourceMgr();
  for (unsigned id : module->buffers) {
    const llvm::MemoryBuffer *buffer = sources.getMemoryBuffer(id);
    this->state.library.declarations.addLibraryRange(buffer->getBufferStart(),
                                                     buffer->getBufferEnd());
    this->state.diagnostics.quietRange(buffer->getBufferStart(), buffer->getBufferEnd());
  }
  bool hasIntrinsics = module->name == LibraryModuleNames::kIntrinsicsPath;
  for (Stmt *item : module->file.items) {
    this->rememberLibraryItem(item, hasIntrinsics);
  }
}

void LibraryDispatchRegistry::rememberLibraryItem(Stmt *item, bool hasIntrinsics) {
  auto *functionDecl = dyn_cast<FunctionDecl>(item);
  if (functionDecl != nullptr) {
    return this->rememberLibraryFunction(functionDecl, hasIntrinsics);
  }
  auto *alias = dyn_cast<TypeAliasDecl>(item);
  if (alias != nullptr) {
    return this->state.library.declarations.rememberLibraryType(alias);
  }
  auto *classDecl = dyn_cast<ClassDecl>(item);
  if (classDecl != nullptr) {
    classDecl->origin = ClassOrigin::LIBRARY;
    this->state.library.declarations.rememberLibraryClass(classDecl);
  }
}

void LibraryDispatchRegistry::rememberLibraryFunction(FunctionDecl *functionDecl,
                                                      bool hasIntrinsics) {
  if (!hasIntrinsics) {
    return this->state.library.declarations.rememberLibraryFunction(functionDecl);
  }
  if (functionDecl->external()) {
    functionDecl->bodySource = FunctionBodySource::INTRINSIC;
  }
}

LibraryDispatchRegistry::ResultIntrinsicShape
LibraryDispatchRegistry::checkIntrinsicShape(const CallExpr *call, const NameExpr *name,
                                             llvm::StringRef intrinsicName, bool known,
                                             unsigned args, unsigned typeArgs) const {
  bool isWrong =
      !known || call->args.size() != args || name->typeArgs.size() != typeArgs || call->hasSpread();
  if (isWrong) {
    auto message = ErrorTextIntrinsicCall::wrong(intrinsicName);

    return err(LibraryDispatchRegistryIntrinsicShapeError{
        .code = LibraryDispatchRegistryIntrinsicShapeErrorCode::NOT_AN_INTRINSIC,
        .message = message,
    });
  }

  return ok(call);
}

const Type *LibraryDispatchRegistry::flatListenerType(const Type *typeArg) const {
  auto flat = std::vector<const Type *>();
  size_t minArgs = 0;
  bool hasRest = false;
  bool isFlattened = typeArg->isFunction() &&
                     ArgumentLists::flattenArguments(typeArg->params(), minArgs, hasRest, flat);
  if (!isFlattened) {
    return typeArg;
  }

  return Type::function(flat, minArgs, hasRest, typeArg->ret(), typeArg->hasThis());
}

bool LibraryDispatchRegistry::isAwaitable(const Type *type) const {
  if (type->isError() || type->isPromise()) {
    return true;
  }
  if (!type->isUnion()) {
    return false;
  }
  for (const Type *member : type->members()) {
    if (member->isPromise()) {
      return true;
    }
  }

  return false;
}

void LibraryDispatchRegistry::declareErrorTypes() {
  for (BuiltinText text : ErrorExportTable::getTexts()) {
    this->declare(text);
  }
  for (const ErrorExportInfo &exports : ErrorExportTable::getExports()) {
    std::vector<llvm::StringRef> names = exports.names();
    for (llvm::StringRef name : names) {
      this->exportFrom(exports.module, name);
    }
  }
}

void LibraryDispatchRegistry::declare(BuiltinText builtin) {
  auto items = this->state.library.declarations.getBuiltinItems(builtin);
  for (Stmt *item : items) {
    auto *alias = dyn_cast<TypeAliasDecl>(item);
    if (alias != nullptr) {
      this->state.library.declarations.rememberModuleAlias(alias);
    }
  }
}

void LibraryDispatchRegistry::exportFrom(NodeModule module, llvm::StringRef name) {
  this->state.library.declarations.addModuleExport(module, name);
}

}
