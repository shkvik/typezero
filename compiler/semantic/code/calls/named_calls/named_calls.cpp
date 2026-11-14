#include "compiler/semantic/code/calls/named_calls/named_calls.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/program/library/builtin_names/library_class_table/library_class_table.h"
#include "compiler/program/library/intl/intl_type_name_table/intl_type_name_table.h"
#include "compiler/semantic/code/functions/code_functions_registry_errors.h"
#include "compiler/semantic/flow_v2/proofs/flow_proofs_errors.h"
#include "compiler/semantic/library/decimal/decimal_checker/decimal_checker_errors.h"
#include "llvm/ADT/STLExtras.h"

using llvm::dyn_cast;

namespace tz {

NamedCalls::NamedCalls(SemanticState &owner, CallRules &sharedCallRules)
    : callRules(sharedCallRules),
      state(owner) {}

void NamedCalls::scopedCall(const ScopedCall &task) {
  CallExpr *call = task.call;
  NameExpr *name = task.name;
  auto entry = this->state.walk.scopes.findName(name->name);
  FuncSymbol *function = entry.func();
  VarSymbol *variable = entry.var();
  if (function != nullptr && variable == nullptr) {
    return this->callRules.nextFunctionNameCall(FunctionNameDraft{
        .call = call,
        .name = name,
        .function = function,
        .expected = task.expected,
        .stage = FunctionNameStage::EVENTS,
    });
  }
  auto *known = this->variableFunction(variable);
  if (known != nullptr && known->sym != nullptr) {
    return this->startKnownFunctionCall(call, name, variable, known);
  }
  auto calledGlobal = GlobalNameTable::find(name->name);
  bool isArrayCall = calledGlobal == GlobalName::ARRAY && variable == nullptr &&
                     function == nullptr && entry.cls() == nullptr && !call->hasSpread();
  if (isArrayCall) {
    auto *made = this->state.ast.make<NewExpr>(call->loc(), name->name, name->typeArgs, call->args);
    return this->checkNew(CheckNew{.newExpr = made, .expected = task.expected});
  }
  if (variable == nullptr && function == nullptr) {
    return this->startNotCallable(call, name, entry);
  }

  return this->callRules.startCalleeValueCall(call);
}

FunctionDecl *NamedCalls::variableFunction(const VarSymbol *variable) const {
  if (variable == nullptr) {
    return nullptr;
  }
  if (variable->function() != nullptr) {
    return variable->function();
  }
  if (variable->isConst) {
    return variable->lambda();
  }

  return nullptr;
}

void NamedCalls::startKnownFunctionCall(CallExpr *call, NameExpr *name, VarSymbol *variable,
                                        const FunctionDecl *known) {
  name->binding = NamedVar{.var = variable};
  if (variable->type != nullptr) {
    name->type = variable->type;
    this->checkDirectCall(CheckDirectCall{
        .call = call,
        .function = known->sym,
        .loc = name->loc(),
        .expected = nullptr,
    });
  } else {
    this->state.walk.tasks.pushTask(
        AfterKnownFunctionType{.call = call, .name = name, .variable = variable, .known = known});
    this->state.walk.tasks.pushTask(FunctionTypeOf{.function = known->sym});
  }

  return this->state.walk.tasks.pushTask(NoteUse{.variable = variable, .loc = name->loc()});
}

void NamedCalls::afterKnownFunctionType(const AfterKnownFunctionType &task) {
  VarSymbol *variable = task.variable;
  const Type *type = this->state.walk.tasks.takeAnswer();
  bool isNamedFunction =
      variable->function() != nullptr && variable->function()->sym->hasReturnType();
  if (isNamedFunction) {
    variable->type = type;
  }
  task.name->type = type;

  return this->checkDirectCall(CheckDirectCall{
      .call = task.call,
      .function = task.known->sym,
      .loc = task.name->loc(),
      .expected = nullptr,
  });
}

void NamedCalls::startNotCallable(CallExpr *call, NameExpr *name, const ScopeEntry &entry) {
  std::string why = entry.cls() == nullptr ? ErrorTextUnsupported::why(name->name) : std::string();
  if (!why.empty()) {
    auto message = ErrorTextCallName::noSupport(name->name, why);
    this->state.diagnostics.error(name->loc(), message);
    this->state.walk.tasks.pushChecked(call);
    return this->state.walk.tasks.pushTask(WalkArgsLoosely{.call = call});
  }
  this->state.walk.tasks.pushTask(AfterNotCallableName{.call = call, .name = name});

  return this->state.walk.tasks.pushTask(CheckName{.name = name});
}

void NamedCalls::checkDirectCall(const CheckDirectCall &task) {
  CallExpr *call = task.call;
  FuncSymbol *function = task.function;
  if (function->isGeneric()) {
    return this->state.walk.tasks.pushTask(CheckGenericCall{
        .call = call,
        .function = function,
        .loc = task.loc,
        .expected = task.expected,
    });
  }
  auto *name = dyn_cast<NameExpr>(call->callee);
  llvm::ArrayRef<TypeRef> typeArgs =
      name != nullptr ? llvm::ArrayRef<TypeRef>(name->typeArgs) : llvm::ArrayRef<TypeRef>();
  auto usable = this->checkDirectTypeArgs(function, typeArgs);
  if (usable.isErr()) {
    this->state.diagnostics.error(task.loc, usable.error().message);
  }
  call->target = FunctionCall{.func = function};
  this->state.walk.tasks.pushTask(
      AfterDirectReturnType{.call = call, .function = function, .loc = task.loc});

  return this->state.walk.tasks.pushTask(ReturnTypeOf{.function = function});
}

void NamedCalls::checkNew(const CheckNew &task) {
  NewExpr *expr = task.newExpr;
  expr->type = Type::error();
  auto load = expr->name.empty() ? std::nullopt : this->state.findEarlyLoad(expr->name);
  if (!load.has_value()) {
    return this->afterNewLoad(AfterNewLoad{.newExpr = expr, .expected = task.expected});
  }
  this->state.walk.tasks.pushTask(AfterNewLoad{.newExpr = expr, .expected = task.expected});

  return this->state.walk.tasks.pushTask(*load);
}

void NamedCalls::afterNewLoad(const AfterNewLoad &task) {
  NewExpr *expr = task.newExpr;
  const Type *expected = task.expected;
  auto entry = this->state.walk.scopes.findName(expr->name);
  bool isVariable = expr->callee == nullptr && !expr->name.empty() && entry.var() != nullptr;
  if (isVariable) {
    expr->callee = this->state.ast.make<NameExpr>(expr->loc(), expr->name);
  }
  if (isVariable && !expr->member.empty()) {
    expr->callee = this->state.ast.make<MemberExpr>(expr->loc(), expr->callee, expr->member);
  }
  auto &tasks = this->state.walk.tasks;
  if (expr->callee != nullptr) {
    return tasks.pushTask(CheckValueNew{.newExpr = expr, .expected = expected});
  }
  auto global = GlobalNameTable::find(expr->name);
  if (!expr->member.empty()) {
    return this->startNamespaceNew(expr, expected, global);
  }
  ClassDecl *classDecl = entry.cls();
  if (classDecl != nullptr) {
    return this->startClassInstanceNew(expr, classDecl, expected);
  }
  const auto *node = this->state.findNodeName(expr->name);
  if (node != nullptr && node->module == NodeModule::Crypto) {
    return tasks.pushTask(CheckCryptoNew{.newExpr = expr, .name = node->name});
  }
  auto supported = this->checkSupportedNew(expr);
  if (supported.isErr()) {
    this->state.diagnostics.error(expr->loc(), supported.error().message);
    return this->pushNewArgsLoosely(expr);
  }
  if (this->startBuiltinNew(expr, expected, global)) {
    return;
  }
  auto found = this->checkClassFound(expr);
  if (found.isErr()) {
    this->state.diagnostics.error(expr->loc(), found.error().message);
    return this->pushNewArgsLoosely(expr);
  }

  return this->startArrayNew(expr, expected);
}

void NamedCalls::pushNewArgsLoosely(NewExpr *expr) {
  this->state.walk.tasks.pushChecked(expr);
  for (Expr *&arg : llvm::reverse(expr->args)) {
    this->state.walk.tasks.pushTask(CheckExprSlot{.slot = &arg, .expected = nullptr});
  }
}

void NamedCalls::startNamespaceNew(NewExpr *expr, const Type *expected,
                                   std::optional<GlobalName> global) {
  auto intl = this->state.walk.scopes.findName(expr->name);
  bool isGlobalIntl = global == GlobalName::INTL && intl.var() == nullptr &&
                      intl.func() == nullptr && intl.cls() == nullptr;
  bool isLocale = LibraryClassTable::isName(expr->member, LibraryClass::LOCALE);
  auto &tasks = this->state.walk.tasks;
  if (isGlobalIntl && isLocale) {
    return tasks.pushTask(CheckLocaleNew{.newExpr = expr, .expected = expected});
  }
  if (isGlobalIntl && IntlTypeNameTable::find(expr->member) == IntlTypeName::DATE_TIME_FORMAT) {
    return tasks.pushTask(CheckDateFormatNew{.newExpr = expr});
  }
  if (isGlobalIntl) {
    return tasks.pushTask(CheckIntlNew{.newExpr = expr});
  }
  auto plain = this->checkNotNamespaced(expr);
  if (plain.isErr()) {
    this->state.diagnostics.error(expr->loc(), plain.error().message);
  }

  return this->pushNewArgsLoosely(expr);
}

void NamedCalls::startClassInstanceNew(NewExpr *expr, ClassDecl *classDecl, const Type *expected) {
  auto crypto = this->checkCryptoClassNew(expr, classDecl);
  if (crypto.isErr()) {
    this->state.diagnostics.error(expr->loc(), crypto.error().message);
    return this->pushNewArgsLoosely(expr);
  }
  auto *finalClass = classDecl->findFinalClass();
  this->state.walk.tasks.pushTask(AfterClassNew{.classDecl = classDecl});

  return this->state.walk.tasks.pushTask(
      CheckClassNew{.newExpr = expr, .classDecl = finalClass, .expected = expected});
}

void NamedCalls::afterClassNew(const AfterClassNew &task) {
  Expr *checked = this->state.walk.tasks.takeChecked();
  this->state.walk.tasks.pushChecked(checked);
  auto *made = dyn_cast<NewExpr>(checked);
  if (made != nullptr && task.classDecl->library()) {
    return this->state.walk.tasks.pushTask(
        NoteLibraryInstance{.made = made, .classDecl = task.classDecl});
  }
}

bool NamedCalls::startBuiltinNew(NewExpr *expr, const Type *expected,
                                 std::optional<GlobalName> global) {
  auto named = this->state.walk.scopes.findName(expr->name);
  bool isUnshadowed = named.var() == nullptr && named.func() == nullptr;
  auto &tasks = this->state.walk.tasks;
  bool isCollection = global == GlobalName::MAP || global == GlobalName::SET ||
                      global == GlobalName::WEAK_MAP || global == GlobalName::WEAK_SET;
  const auto *bytes = this->state.library.bytes.bytesTypeNamed(expr->name);
  if (global == GlobalName::REG_EXP && isUnshadowed) {
    this->rejectNewTypeArgs(expr);
    tasks.pushTask(CheckRegExpConstructor{.newExpr = expr});
  } else if (global == GlobalName::DATE && isUnshadowed) {
    this->rejectNewTypeArgs(expr);
    tasks.pushTask(CheckDateNew{.newExpr = expr});
  } else if (isCollection && isUnshadowed) {
    tasks.pushTask(CheckDictNew{.newExpr = expr, .expected = expected});
  } else if (global == GlobalName::PROMISE && isUnshadowed) {
    tasks.pushTask(CheckPromiseNew{.newExpr = expr, .expected = expected});
  } else if (global == GlobalName::WEAK_REF && isUnshadowed) {
    tasks.pushTask(CheckWeakRefNew{.newExpr = expr, .expected = expected});
  } else if (bytes != nullptr && isUnshadowed) {
    tasks.pushTask(CheckBytesNew{.newExpr = expr, .bytes = bytes});
  } else if (global == GlobalName::BIG_INT64_ARRAY || global == GlobalName::BIG_UINT64_ARRAY) {
    auto what = ErrorTextFunctionName::quoted(expr->name);
    auto bigint = ErrorTextBigInt::none(what);
    this->state.diagnostics.error(expr->loc(), bigint);
    this->pushNewArgsLoosely(expr);
  } else {
    return false;
  }

  return true;
}

void NamedCalls::rejectNewTypeArgs(const NewExpr *expr) {
  auto plainNew = this->checkNoNewTypeArgs(expr);
  if (plainNew.isErr()) {
    this->state.diagnostics.error(expr->loc(), plainNew.error().message);
  }
}

void NamedCalls::startArrayNew(NewExpr *expr, const Type *expected) {
  bool isFromExpected =
      expr->typeArgs.empty() && expected != nullptr && expected->nonOptional()->isArray();
  auto counted = this->checkArrayTypeArgs(expr, isFromExpected);
  if (counted.isErr()) {
    this->state.diagnostics.error(expr->loc(), counted.error().message);
    return this->finishArrayNew(expr, Type::error());
  }
  if (isFromExpected) {
    return this->finishArrayNew(expr, expected->nonOptional()->inner());
  }
  this->state.walk.tasks.pushTask(AfterArrayElement{.newExpr = expr});

  return this->state.walk.tasks.pushTask(
      ResolveTypeAnswer{.ref = &expr->typeArgs[0], .use = TypeUse::STANDALONE});
}

void NamedCalls::afterArrayElement(const AfterArrayElement &task) {
  const Type *element = this->state.walk.tasks.takeAnswer();

  return this->finishArrayNew(task.newExpr, element);
}

NamedCalls::ResultArrayElement NamedCalls::checkArrayTypeArgs(const NewExpr *expr,
                                                              bool isFromExpected) const {
  if (expr->typeArgs.size() > 1) {
    auto message = ErrorTextNewArray::oneArg();

    return err(CodeCallsRegistryArrayElementError{
        .code = CodeCallsRegistryArrayElementErrorCode::TOO_MANY_TYPE_ARGUMENTS,
        .message = message,
    });
  }
  if (expr->typeArgs.empty() && !isFromExpected) {
    auto message = ErrorTextNewArray::needs();

    return err(CodeCallsRegistryArrayElementError{
        .code = CodeCallsRegistryArrayElementErrorCode::NEEDS_ELEMENT_TYPE,
        .message = message,
    });
  }

  return ok(Type::error());
}

void NamedCalls::finishArrayNew(NewExpr *expr, const Type *element) {
  auto fits = this->checkArrayArgs(expr);
  if (fits.isErr()) {
    this->state.diagnostics.error(expr->loc(), fits.error().message);
    return this->pushNewArgsLoosely(expr);
  }
  if (!element->isError()) {
    expr->type = Type::array(element);
  }
  this->state.walk.tasks.pushChecked(expr);
  if (expr->args.size() != 1) {
    return;
  }
  this->state.walk.tasks.pushTask(AfterArrayLength{.newExpr = expr, .element = element});

  return this->state.walk.tasks.pushTask(CheckPosition{.slot = &expr->args[0]});
}

void NamedCalls::afterArrayLength(const AfterArrayLength &task) {
  NewExpr *expr = task.newExpr;
  auto zero = this->checkZeroValue(task.element);
  if (zero.isErr()) {
    this->state.diagnostics.error(expr->loc(), zero.error().message);
  }
  auto what = ErrorTextNewArray::length();
  auto between = this->state.flow.proofs.requireBetween(expr->args[0], 0, std::nullopt, what);
  bool isReported =
      between.isErr() && between.error().code != FlowProofsRequireBetweenErrorCode::EXEMPT;
  if (isReported) {
    return this->state.diagnostics.error(expr->args[0]->loc(), between.error().message);
  }
}

NamedCalls::ResultTypeArgsUsable
NamedCalls::checkDirectTypeArgs(const FuncSymbol *function,
                                llvm::ArrayRef<TypeRef> typeArgs) const {
  if (!typeArgs.empty()) {
    auto message = ErrorTextCallTypeArgs::notGeneric(function->name);

    return err(CodeCallsRegistryDirectTypeArgsError{
        .code = CodeCallsRegistryDirectTypeArgsErrorCode::NOT_GENERIC,
        .message = message,
    });
  }

  return ok(function);
}

NamedCalls::ResultNamespaceNew NamedCalls::checkNotNamespaced(const NewExpr *expr) const {
  if (!expr->member.empty()) {
    auto message = ErrorTextNamespaceNew::namespaced(expr->name, expr->member);

    return err(CodeCallsRegistryNamespaceNewError{
        .code = CodeCallsRegistryNamespaceNewErrorCode::NAMESPACE_CLASS,
        .message = message,
    });
  }

  return ok(expr);
}

NamedCalls::ResultCryptoMadeOk NamedCalls::checkCryptoClassNew(const NewExpr *expr,
                                                               const ClassDecl *classDecl) const {
  bool isCryptoKey = LibraryClassTable::isName(classDecl->name, LibraryClass::CRYPTO_KEY);
  bool isHidden = classDecl->library() &&
                  !this->state.library.declarations.isInsideLibrary(expr->loc()) &&
                  this->isCryptoClass(classDecl);
  if (isHidden && isCryptoKey) {
    auto message = ErrorTextCryptoNew::cryptoKey();

    return err(CodeCallsRegistryCryptoNewError{
        .code = CodeCallsRegistryCryptoNewErrorCode::CRYPTO_KEY,
        .message = message,
    });
  }
  if (isHidden) {
    auto message = ErrorTextCryptoNew::subtle();

    return err(CodeCallsRegistryCryptoNewError{
        .code = CodeCallsRegistryCryptoNewErrorCode::SUBTLE_CRYPTO,
        .message = message,
    });
  }

  return ok(classDecl);
}

bool NamedCalls::isCryptoClass(const ClassDecl *classDecl) const {
  auto &declarations = this->state.library.declarations;
  auto keyName = LibraryClassTable::getName(LibraryClass::CRYPTO_KEY);
  auto subtleName = LibraryClassTable::getName(LibraryClass::SUBTLE_CRYPTO);

  return classDecl == declarations.findLibraryClass(keyName) ||
         classDecl == declarations.findLibraryClass(subtleName);
}

NamedCalls::ResultSupportedNew NamedCalls::checkSupportedNew(const NewExpr *expr) const {
  auto why = ErrorTextUnsupported::why(expr->name);
  if (!why.empty()) {
    auto message = ErrorTextSupportedNew::unsupported(expr->name, why);

    return err(CodeCallsRegistrySupportedNewError{
        .code = CodeCallsRegistrySupportedNewErrorCode::UNSUPPORTED,
        .message = message,
    });
  }

  return ok(expr);
}

NamedCalls::ResultNoTypeArgsOk NamedCalls::checkNoNewTypeArgs(const NewExpr *expr) const {
  if (!expr->typeArgs.empty()) {
    auto message = ErrorTextNoTypeArgs::given(expr->name);

    return err(CodeCallsRegistryNoTypeArgsError{
        .code = CodeCallsRegistryNoTypeArgsErrorCode::TYPE_ARGUMENTS_GIVEN,
        .message = message,
    });
  }

  return ok(expr);
}

NamedCalls::ResultClassIsFound NamedCalls::checkClassFound(const NewExpr *expr) const {
  auto global = GlobalNameTable::find(expr->name);
  if (global != GlobalName::ARRAY) {
    auto message = ErrorTextClassFound::notFound(expr->name);

    return err(CodeCallsRegistryClassFoundError{
        .code = CodeCallsRegistryClassFoundErrorCode::CLASS_NOT_FOUND,
        .message = message,
    });
  }

  return ok(expr);
}

NamedCalls::ResultArrayArgsFit NamedCalls::checkArrayArgs(const NewExpr *expr) const {
  if (expr->args.size() > 1) {
    auto message = ErrorTextArrayArgs::literal();

    return err(CodeCallsRegistryArrayArgsError{
        .code = CodeCallsRegistryArrayArgsErrorCode::VALUES_NEED_LITERAL,
        .message = message,
    });
  }

  return ok(expr);
}

NamedCalls::ResultHasZeroValue NamedCalls::checkZeroValue(const Type *element) const {
  if (!element->isError() && !element->hasZeroValue()) {
    auto message = ErrorTextZeroValue::none(element->name());

    return err(CodeCallsRegistryZeroValueError{
        .code = CodeCallsRegistryZeroValueErrorCode::NO_ZERO_VALUE,
        .message = message,
    });
  }

  return ok(element);
}

}
