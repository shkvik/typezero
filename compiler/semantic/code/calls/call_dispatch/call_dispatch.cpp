#include "compiler/semantic/code/calls/call_dispatch/call_dispatch.h"
#include "compiler/program/library/array/array_method_table/array_method_table.h"
#include "compiler/program/library/builtin_names/global_member_table/global_member_table.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/program/library/builtin_names/keyword_table/keyword_table.h"
#include "compiler/program/library/builtin_names/library_class_table/library_class_table.h"
#include "compiler/program/library/function/function_method_table/function_method_table.h"
#include "compiler/program/library/iterator/iterator_method_table/iterator_method_table.h"
#include "compiler/program/library/object/object_member_table/object_member_table.h"
#include "compiler/program/library/string/string_method_table/string_method_table.h"

using llvm::dyn_cast;

namespace tz {

namespace {

constexpr const char *kIteratorMethod = "[Symbol.iterator]";
constexpr const char *kAsyncIteratorMethod = "[Symbol.asyncIterator]";
constexpr GlobalName kTypeArgFreeGlobals[] =
    {

}

CallDispatch::CallDispatch(SemanticState & owner, CallRules &sharedCallRules)
    : callRules(sharedCallRules),
                     state(owner) {}

void CallDispatch::checkCall(const CheckCall &task) {
  CallExpr *call = task.call;
  call->type = Type::error();
  this->callRules.expandListSpreads(call->args, call->spread);
  this->rewriteSymbolCallee(call);
  auto *superName = dyn_cast<NameExpr>(call->callee);
  if (superName != nullptr && superName->name == KeywordTable::getName(Keyword::SUPER)) {
    return this->state.walk.tasks.pushTask(CheckSuperCall{.call = call});
  }
  this->state.walk.tasks.pushTask(AfterNodeCallTry{.call = call, .expected = task.expected});

  return this->state.walk.tasks.pushTask(CheckNodeCall{.call = call, .expected = task.expected});
}

void CallDispatch::afterNodeCallTry(const AfterNodeCallTry &task) {
  Expr *nodeCall = this->state.walk.tasks.takeChecked();
  if (nodeCall != nullptr) {
    return this->state.walk.tasks.pushChecked(nodeCall);
  }
  CallExpr *call = task.call;
  auto *member = dyn_cast<MemberExpr>(call->callee);
  if (member != nullptr && !call->optional) {
    return this->startMemberCall(call, member, task.expected);
  }
  auto *name = dyn_cast<NameExpr>(call->callee);
  if (name != nullptr && !call->optional) {
    return this->nextNamedCall(NamedCallDraft{
        .call = call,
        .name = name,
        .expected = task.expected,
        .stage = NamedCallStage::LIBRARY,
    });
  }

  return this->callRules.startCalleeValueCall(call);
}

void CallDispatch::startMemberCall(CallExpr *call, MemberExpr *member, const Type *expected) {
  if (this->state.isGlobalName(member->object, GlobalName::PROMISE)) {
    return this->state.walk.tasks.pushTask(
        CheckPromiseStatic{.call = call, .member = member, .expected = expected});
  }
  this->rejectGlobalTypeArgs(member);
  auto *superObject = dyn_cast<NameExpr>(member->object);
  if (superObject != nullptr && superObject->name == KeywordTable::getName(Keyword::SUPER)) {
    return this->state.walk.tasks.pushTask(CheckSuperMethodCall{.call = call, .member = member});
  }
  this->state.walk.tasks.pushTask(
      AfterResultStaticTry{.call = call, .member = member, .expected = expected});

  return this->state.walk.tasks.pushTask(CheckResultStatic{.call = call, .member = member});
}

void CallDispatch::afterResultStaticTry(const AfterResultStaticTry &task) {
  Expr *resultStatic = this->state.walk.tasks.takeChecked();
  if (resultStatic != nullptr) {
    return this->state.walk.tasks.pushChecked(resultStatic);
  }
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  auto &tasks = this->state.walk.tasks;
  if (this->state.isGlobalName(member->object, GlobalName::JSON)) {
    return tasks.pushTask(CheckJsonStatic{.call = call, .member = member});
  }
  if (this->state.isGlobalMember(member, GlobalMember::ARRAY_IS_ARRAY)) {
    return tasks.pushTask(CheckArrayIsArray{.call = call, .member = member});
  }
  if (this->state.isGlobalMember(member, GlobalMember::JSON_IS_JSON)) {
    return tasks.pushTask(CheckJsonIsJson{.call = call, .member = member});
  }
  if (this->state.isGlobalName(member->object, GlobalName::REFLECT)) {
    return tasks.pushTask(
        CheckReflectCall{.call = call, .member = member, .expected = task.expected});
  }
  tasks.pushTask(AfterConsoleTry{.call = call, .member = member, .expected = task.expected});

  return tasks.pushTask(CheckConsoleCall{.call = call, .member = member});
}

void CallDispatch::afterConsoleTry(const AfterConsoleTry &task) {
  Expr *consoleCall = this->state.walk.tasks.takeChecked();
  if (consoleCall != nullptr) {
    return this->state.walk.tasks.pushChecked(consoleCall);
  }
  this->state.walk.tasks.pushTask(
      AfterProcessTry{.call = task.call, .member = task.member, .expected = task.expected});

  return this->state.walk.tasks.pushTask(
      CheckProcessCall{.call = task.call, .member = task.member});
}

void CallDispatch::afterProcessTry(const AfterProcessTry &task) {
  Expr *processCall = this->state.walk.tasks.takeChecked();
  if (processCall != nullptr) {
    return this->state.walk.tasks.pushChecked(processCall);
  }
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  const Type *expected = task.expected;
  auto &tasks = this->state.walk.tasks;
  auto *nested = dyn_cast<MemberExpr>(member->object);
  bool isBitCall = this->state.isGlobalName(member->object, GlobalName::BIT) ||
                   (nested != nullptr && this->state.isGlobalName(nested->object, GlobalName::BIT));
  if (isBitCall) {
    return tasks.pushTask(CheckBitCall{.call = call, .member = member});
  }
  if (this->state.isGlobalName(member->object, GlobalName::MATH)) {
    return tasks.pushTask(CheckMathCall{.call = call, .member = member, .expected = expected});
  }
  if (this->state.isGlobalName(member->object, GlobalName::REG_EXP)) {
    return tasks.pushTask(CheckRegExpStatic{.call = call, .member = member});
  }
  if (this->state.isGlobalName(member->object, GlobalName::INTL)) {
    return tasks.pushTask(CheckIntlCall{.call = call, .member = member});
  }
  bool isIntlMember =
      nested != nullptr && this->state.isGlobalName(nested->object, GlobalName::INTL);
  bool isLocale = isIntlMember && LibraryClassTable::isName(nested->name, LibraryClass::LOCALE);
  if (isLocale) {
    return tasks.pushTask(CheckLocaleStatic{.call = call, .member = member, .expected = expected});
  }
  if (isIntlMember) {
    return tasks.pushTask(CheckIntlStatic{.call = call, .member = member});
  }
  if (this->state.isGlobalName(member->object, GlobalName::DECIMAL)) {
    return tasks.pushTask(CheckDecimalStatic{.call = call, .member = member});
  }
  if (this->state.library.crypto.isX509Class(member->object)) {
    return tasks.pushTask(CheckX509Parse{.call = call, .member = member});
  }
  if (this->state.isGlobalName(member->object, GlobalName::CRYPTO)) {
    return tasks.pushTask(CheckWebCryptoCall{.call = call, .member = member});
  }
  tasks.pushTask(AfterBytesStaticTry{.call = call, .member = member, .expected = expected});

  return tasks.pushTask(CheckBytesStatic{.call = call, .member = member});
}

void CallDispatch::afterBytesStaticTry(const AfterBytesStaticTry &task) {
  Expr *bytesStatic = this->state.walk.tasks.takeChecked();
  if (bytesStatic != nullptr) {
    return this->state.walk.tasks.pushChecked(bytesStatic);
  }
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  auto &tasks = this->state.walk.tasks;
  bool isNumberStatic = this->state.isGlobalName(member->object, GlobalName::STRING) ||
                        this->state.isGlobalName(member->object, GlobalName::NUMBER);
  if (isNumberStatic) {
    if (this->callRules.rejectSpread(call)) {
      return;
    }
    return tasks.pushTask(CheckNumberStatic{.call = call, .member = member});
  }
  if (this->state.isGlobalName(member->object, GlobalName::OBJECT)) {
    return tasks.pushTask(
        CheckObjectCall{.call = call, .member = member, .expected = task.expected});
  }
  auto *object = dyn_cast<NameExpr>(member->object);
  ClassDecl *classDecl =
      object != nullptr ? this->state.walk.scopes.findName(object->name).cls() : nullptr;
  if (classDecl != nullptr) {
    return tasks.pushTask(CheckClassStaticCall{
        .call = call,
        .member = member,
        .classDecl = classDecl,
        .expected = task.expected,
    });
  }
  if (object != nullptr && object->name == KeywordTable::getName(Keyword::THIS)) {
    this->state.walk.self.rememberThisMember(member->name);
  }
  if (this->state.isGlobalName(member->object, GlobalName::ITERATOR)) {
    return tasks.pushTask(CheckIteratorFrom{.call = call, .member = member});
  }
  if (this->state.isGlobalName(member->object, GlobalName::ARRAY)) {
    return tasks.pushTask(CheckArrayFrom{.call = call, .member = member});
  }
  if (this->state.isGlobalName(member->object, GlobalName::MAP)) {
    return tasks.pushTask(CheckMapStatic{.call = call, .member = member});
  }

  return this->startReceiverObject(call, member, task.expected);
}

void CallDispatch::startReceiverObject(CallExpr *call, MemberExpr *member, const Type *expected) {
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(AfterReceiverObject{.call = call, .member = member, .expected = expected});
  tasks.pushTask(ApparentSlot{.slot = &member->object});
  tasks.pushTask(CheckExprSlot{.slot = &member->object, .expected = nullptr});
  const auto *iteratorMethod = IteratorMethodTable::find(member->name);
  if (iteratorMethod != nullptr && iteratorMethod->form.is<IteratorConsumerMethod>()) {
    return tasks.pushTask(MarkFused{.expr = member->object});
  }
}

void CallDispatch::afterReceiverObject(const AfterReceiverObject &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  const Type *expected = task.expected;
  this->state.walk.self.forgetThisMember();
  if (member->optional) {
    member->object = this->callRules.chainLink(member->object);
  }
  const Type *receiver = member->object->type;
  if (!receiver->isClass() && !receiver->isUnion()) {
    this->rejectTypeArgs(member);
  }
  auto &tasks = this->state.walk.tasks;
  if (receiver->isRuntimeObject()) {
    return tasks.pushTask(CheckRuntimeMethod{.call = call, .member = member, .expected = expected});
  }
  if (receiver->isResult()) {
    tasks.pushTask(AfterResultMethodTry{.call = call, .member = member, .expected = expected});
    return tasks.pushTask(CheckResultMethod{.call = call, .member = member, .expected = expected});
  }

  return this->startLibraryReceiver(call, member, expected);
}

void CallDispatch::afterResultMethodTry(const AfterResultMethodTry &task) {
  Expr *resultMethod = this->state.walk.tasks.takeChecked();
  if (resultMethod != nullptr) {
    return this->state.walk.tasks.pushChecked(resultMethod);
  }

  return this->startLibraryReceiver(task.call, task.member, task.expected);
}

void CallDispatch::startLibraryReceiver(CallExpr *call, MemberExpr *member, const Type *expected) {
  const Type *receiver = member->object->type;
  auto &tasks = this->state.walk.tasks;
  if (receiver->nonOptional()->isJson()) {
    return tasks.pushTask(CheckJsonMethod{.call = call, .member = member});
  }
  if (receiver->isRegExp()) {
    return tasks.pushTask(CheckRegexMethod{.call = call, .member = member});
  }
  if (receiver->isDate()) {
    tasks.pushTask(AfterDateMethodTry{.call = call, .member = member, .expected = expected});
    return tasks.pushTask(CheckDateMethod{.call = call, .member = member});
  }

  return this->startOtherReceiver(call, member, expected);
}

void CallDispatch::afterDateMethodTry(const AfterDateMethodTry &task) {
  Expr *dateMethod = this->state.walk.tasks.takeChecked();
  if (dateMethod != nullptr) {
    return this->state.walk.tasks.pushChecked(dateMethod);
  }

  return this->startOtherReceiver(task.call, task.member, task.expected);
}

void CallDispatch::startOtherReceiver(CallExpr *call, MemberExpr *member, const Type *expected) {
  const Type *receiver = member->object->type;
  auto &tasks = this->state.walk.tasks;
  if (receiver->isDateFormat()) {
    return tasks.pushTask(CheckDateFormatMethod{.call = call, .member = member});
  }
  if (receiver->isIntl()) {
    return tasks.pushTask(CheckIntlMethod{.call = call, .member = member});
  }
  if (receiver->isCrypto()) {
    return tasks.pushTask(CheckCryptoMethod{.call = call, .member = member});
  }
  if (receiver->isWeakRef()) {
    return tasks.pushTask(CheckWeakRefMethod{.call = call, .member = member});
  }
  if (receiver->isBytes()) {
    return tasks.pushTask(CheckBytesMethod{.call = call, .member = member, .expected = expected});
  }
  if (receiver->isUnknown() || receiver->isAnyObject() || receiver->isClassRef()) {
    return tasks.pushTask(CheckUnknownMethod{.call = call, .member = member});
  }
  if (receiver->isFunction() && receiver->hasThis()) {
    return tasks.pushTask(CheckThisCall{.call = call, .member = member});
  }
  if (receiver->isListParam()) {
    return tasks.pushTask(CheckListMethod{.call = call, .member = member});
  }
  if (receiver->isPromise()) {
    return tasks.pushTask(CheckPromiseMethod{.call = call, .member = member, .expected = expected});
  }
  if (receiver->isTimer()) {
    return tasks.pushTask(CheckTimerMethod{.call = call, .member = member});
  }
  if (!receiver->isMatch()) {
    return this->startShapedReceiver(call, member, expected);
  }
  tasks.pushTask(AfterMatchReceiver{.call = call, .member = member, .expected = expected});
  tasks.pushChecked(member->object);

  return tasks.pushTask(MatchItems{});
}

void CallDispatch::afterMatchReceiver(const AfterMatchReceiver &task) {
  Expr *values = this->state.walk.tasks.takeChecked();
  if (values != nullptr) {
    task.member->object = values;
  }

  return this->startShapedReceiver(task.call, task.member, task.expected);
}

void CallDispatch::startShapedReceiver(CallExpr *call, MemberExpr *member, const Type *expected) {
  const Type *receiver = member->object->type;
  if (!receiver->isUnion()) {
    return this->startSettledReceiver(call, member, receiver, expected);
  }
  const auto *common = this->state.types.relations.commonBaseWith(receiver, member->name);
  if (common == nullptr) {
    this->rejectTypeArgs(member);
    return this->state.walk.tasks.pushTask(CheckUnionMethodCall{.call = call, .member = member});
  }
  this->state.walk.tasks.pushTask(AfterCommonReceiver{
      .call = call,
      .member = member,
      .receiver = common,
      .expected = expected,
  });

  return this->state.walk.tasks.pushTask(ConvertSlot{.slot = &member->object, .to = common});
}

void CallDispatch::afterCommonReceiver(const AfterCommonReceiver &task) {
  return this->startSettledReceiver(task.call, task.member, task.receiver, task.expected);
}

void CallDispatch::startSettledReceiver(CallExpr *call, MemberExpr *member, const Type *receiver,
                                        const Type *expected) {
  const Type *classReceiver = member->object->type;
  bool isSubtleCall = classReceiver->isClass() &&
                      this->state.library.crypto.isSubtleClass(classReceiver) &&
                      !this->state.library.declarations.isInsideLibrary(call->loc());
  if (!isSubtleCall) {
    return this->settledAfterSubtle(call, member, receiver, expected);
  }
  this->state.walk.tasks.pushTask(AfterSubtleTry{
      .call = call,
      .member = member,
      .receiver = receiver,
      .expected = expected,
  });

  return this->state.walk.tasks.pushTask(
      CheckSubtleCall{.call = call, .member = member, .expected = expected});
}

void CallDispatch::afterSubtleTry(const AfterSubtleTry &task) {
  Expr *subtle = this->state.walk.tasks.takeChecked();
  if (subtle != nullptr) {
    return this->state.walk.tasks.pushChecked(subtle);
  }

  return this->settledAfterSubtle(task.call, task.member, task.receiver, task.expected);
}

void CallDispatch::settledAfterSubtle(CallExpr *call, MemberExpr *member, const Type *receiver,
                                      const Type *expected) {
  if (receiver->isClass()) {
    bool isProven = this->state.library.tzModules.proveUrlSetter(member, call);
    this->state.walk.tasks.pushTask(
        AfterClassMethodCall{.call = call, .member = member, .isProven = isProven});
    return this->state.walk.tasks.pushTask(
        CheckMethodCall{.call = call, .member = member, .expected = expected});
  }
  this->rejectTypeArgs(member);
  if (!receiver->isTuple()) {
    return this->startNonTupleReceiver(call, member, receiver, expected);
  }
  const auto *arrayMethod = ArrayMethodTable::find(member->name);
  auto *copy = this->tupleArrayCopy(member, arrayMethod);
  if (copy == nullptr) {
    return this->state.walk.tasks.pushTask(CheckTupleMethod{.call = call, .member = member});
  }
  member->object = copy;

  return this->startNonTupleReceiver(call, member, copy->type, expected);
}

void CallDispatch::afterClassMethodCall(const AfterClassMethodCall &task) {
  Expr *checked = this->state.walk.tasks.takeChecked();
  CallExpr *call = task.call;
  if (task.isProven && checked->type->isResult()) {
    auto *dropped = this->state.ast.make<UnaryExpr>(call->loc(), UnaryOp::Void, checked);
    dropped->type = Type::undefinedTy();
    this->state.walk.tasks.pushChecked(dropped);
  } else {
    this->state.walk.tasks.pushChecked(checked);
  }

  return this->state.walk.tasks.pushTask(WalkHeadersCall{.call = call, .member = task.member});
}

Expr *CallDispatch::tupleArrayCopy(MemberExpr *member, const ArrayMethodInfo *arrayMethod) {
  bool isJoining = arrayMethod != nullptr && arrayMethod->method == ArrayMethod::JOIN;
  bool isChanging = arrayMethod != nullptr && (arrayMethod->form.is<ResizingArrayMethod>() ||
                                               arrayMethod->form.is<RewritingArrayMethod>() ||
                                               arrayMethod->form.is<SortingArrayMethod>());
  if (isChanging || isJoining) {
    return nullptr;
  }

  return this->arrayCopy(member->object);
}

Expr *CallDispatch::arrayCopy(Expr *tuple) {
  const Type *type = tuple->type;
  if (type == nullptr || !type->isTuple() || type->tupleRest() != nullptr ||
      type->fields().empty() || type->tupleMin() != type->fields().size()) {
    return nullptr;
  }
  const Type *element = type->fields().front().type;
  for (const Field &field : type->fields()) {
    if (field.type != element) {
      return nullptr;
    }
  }
  auto *copy = this->state.ast.make<ArrayLitExpr>(tuple->loc());
  copy->elements.push_back(ArrayLitExpr::Element{.expr = tuple, .spread = true});
  copy->type = Type::array(element);

  return copy;
}

void CallDispatch::startNonTupleReceiver(CallExpr *call, MemberExpr *member, const Type *receiver,
                                         const Type *expected) {
  auto &tasks = this->state.walk.tasks;
  if (receiver->isObject()) {
    return tasks.pushTask(CheckFieldCall{.call = call, .member = member});
  }
  const auto *arrayMethod = ArrayMethodTable::find(member->name);
  bool isArrayPush =
      receiver->isArray() && arrayMethod != nullptr && arrayMethod->method == ArrayMethod::PUSH;
  if (!isArrayPush && this->callRules.rejectSpread(call)) {
    return;
  }
  const Type *valueReceiver = member->object->type;
  const auto *stringMethod = StringMethodTable::find(member->name);
  bool isStringLocale = valueReceiver->isString() && stringMethod != nullptr &&
                        stringMethod->form.is<LocaleStringMethod>();
  if (isStringLocale) {
    return tasks.pushTask(CheckStringLocale{.call = call, .member = member});
  }
  auto localeMember = ObjectMemberTable::find(member->name);
  bool isNumberLocale = (valueReceiver->isNumeric() || valueReceiver->isDecimal()) &&
                        localeMember == ObjectMember::TO_LOCALE_STRING;
  if (isNumberLocale) {
    return tasks.pushTask(CheckNumberLocale{.call = call, .member = member});
  }
  if (valueReceiver->isDecimal()) {
    return tasks.pushTask(CheckDecimalMethod{.call = call, .member = member});
  }

  return this->startPlainValueMethod(call, member, receiver, expected);
}

void CallDispatch::startPlainValueMethod(CallExpr *call, MemberExpr *member, const Type *receiver,
                                         const Type *expected) {
  auto &tasks = this->state.walk.tasks;
  if (receiver->isIterator()) {
    return tasks.pushTask(CheckIteratorMethod{.call = call, .member = member});
  }
  if (receiver->isDict()) {
    return tasks.pushTask(CheckDictMethod{.call = call, .member = member});
  }
  if (receiver->isString()) {
    return tasks.pushTask(CheckStringMethod{.call = call, .member = member});
  }
  if (receiver->isArray()) {
    return tasks.pushTask(CheckArrayMethod{.call = call, .member = member, .expected = expected});
  }
  if (receiver->isNumeric()) {
    return tasks.pushTask(CheckNumberMethod{.call = call, .member = member});
  }
  auto objectMember = ObjectMemberTable::find(member->name);
  bool isBoolToString =
      receiver->isBool() && objectMember == ObjectMember::TO_STRING && call->args.empty();
  if (isBoolToString) {
    call->args.push_back(member->object);
    call->target = BuiltinCall{.builtin = Builtin::StringOf};
    call->type = Type::string();
    tasks.pushChecked(call);
    return tasks.pushTask(ToStringSlot{.slot = &call->args.back()});
  }
  this->reportMissingMethod(member, receiver);
  tasks.pushChecked(call);

  return tasks.pushTask(WalkArgsLoosely{.call = call});
}

void CallDispatch::nextNamedCall(const NamedCallDraft &draft) {
  auto &tasks = this->state.walk.tasks;
  auto next = draft;
  CallExpr *call = draft.call;
  NameExpr *name = draft.name;
  switch (draft.stage) {
    case NamedCallStage::LIBRARY:
      next.stage = NamedCallStage::RESULT;
      tasks.pushTask(AfterNamedCallTry{.draft = next});
      return tasks.pushTask(CheckLibCall{.call = call, .name = name});
    case NamedCallStage::RESULT:
      next.stage = NamedCallStage::JSON;
      tasks.pushTask(AfterNamedCallTry{.draft = next});
      return tasks.pushTask(
          CheckResultCall{.call = call, .name = name, .expected = draft.expected});
    case NamedCallStage::JSON:
      next.stage = NamedCallStage::TIMER;
      tasks.pushTask(AfterNamedCallTry{.draft = next});
      return tasks.pushTask(CheckJsonGlobal{.call = call, .name = name});
    case NamedCallStage::TIMER:
      next.stage = NamedCallStage::GLOBAL;
      tasks.pushTask(AfterNamedCallTry{.draft = next});
      return tasks.pushTask(CheckAsyncGlobal{.call = call, .name = name});
    case NamedCallStage::GLOBAL:
      next.stage = NamedCallStage::SCOPED;
      tasks.pushTask(AfterNamedCallTry{.draft = next});
      return tasks.pushTask(CheckGlobalCall{.call = call, .name = name});
    case NamedCallStage::SCOPED:
      return this->startScopedCall(call, name, draft.expected);
  }
}

void CallDispatch::afterNamedCallTry(const AfterNamedCallTry &task) {
  Expr *claimed = this->state.walk.tasks.takeChecked();
  if (claimed != nullptr) {
    return this->state.walk.tasks.pushChecked(claimed);
  }

  return this->nextNamedCall(task.draft);
}

void CallDispatch::startScopedCall(CallExpr *call, NameExpr *name, const Type *expected) {
  auto load = this->state.findEarlyLoad(name->name);
  if (!load.has_value()) {
    return this->state.walk.tasks.pushTask(
        ScopedCall{.call = call, .name = name, .expected = expected});
  }
  this->state.walk.tasks.pushTask(ScopedCall{.call = call, .name = name, .expected = expected});

  return this->state.walk.tasks.pushTask(*load);
}

void CallDispatch::afterFunctionNameTry(const AfterFunctionNameTry &task) {
  Expr *claimed = this->state.walk.tasks.takeChecked();
  if (claimed != nullptr) {
    return this->state.walk.tasks.pushChecked(claimed);
  }

  return this->callRules.nextFunctionNameCall(task.draft);
}

void CallDispatch::afterNotCallableName(const AfterNotCallableName &task) {
  const Expr *checked = this->state.walk.tasks.takeChecked();
  if (!checked->type->isError()) {
    auto message = ErrorTextCallName::notFunc(task.name->name);
    this->state.diagnostics.error(task.name->loc(), message);
  }
  this->state.walk.tasks.pushChecked(task.call);

  return this->state.walk.tasks.pushTask(WalkArgsLoosely{.call = task.call});
}

CallDispatch::ResultTypeArgsAbsent CallDispatch::checkNoTypeArgs(const MemberExpr *member) const {
  if (!member->typeArgs.empty()) {
    auto message = ErrorTextCallTypeArgs::forGeneric();

    return err(CodeCallsRegistryNoTypeArgsError{
        .code = CodeCallsRegistryNoTypeArgsErrorCode::TYPE_ARGUMENTS_GIVEN,
        .message = message,
    });
  }

  return ok(member);
}

void CallDispatch::rewriteSymbolCallee(CallExpr *call) {
  auto *index = dyn_cast<IndexExpr>(call->callee);
  if (index == nullptr || index->optional) {
    return;
  }
  auto *key = dyn_cast<MemberExpr>(index->index);
  bool isIterator =
      key != nullptr && this->state.isGlobalMember(key, GlobalMember::SYMBOL_ITERATOR);
  bool isAsyncIterator =
      key != nullptr && this->state.isGlobalMember(key, GlobalMember::SYMBOL_ASYNC_ITERATOR);
  if (!isIterator && !isAsyncIterator) {
    return;
  }
  llvm::StringRef name = isIterator ? kIteratorMethod : kAsyncIteratorMethod;
  call->callee = this->state.ast.make<MemberExpr>(index->loc(), index->object, name);
}

void CallDispatch::rejectTypeArgs(MemberExpr *member) {
  auto absent = this->checkNoTypeArgs(member);
  if (absent.isErr()) {
    this->state.diagnostics.error(member->loc(), absent.error().message);
    member->typeArgs.clear();
  }
}

void CallDispatch::rejectGlobalTypeArgs(MemberExpr *member) {
  auto *object = dyn_cast<NameExpr>(member->object);
  if (object == nullptr) {
    return;
  }
  auto entry = this->state.walk.scopes.findName(object->name);
  if (entry.cls() != nullptr) {
    return;
  }
  for (GlobalName global : kTypeArgFreeGlobals) {
    if (this->state.isGlobalName(object, global)) {
      return this->rejectTypeArgs(member);
    }
  }
}

void CallDispatch::reportMissingMethod(const MemberExpr *member, const Type *receiver) {
  if (receiver->isOptional()) {
    auto message = ErrorTextCallMethod::undefined(member->name);
    this->state.diagnostics.error(member->object->loc(), message);
    return;
  }
  auto functionMethod = FunctionMethodTable::find(member->name);
  bool isSettingThis = functionMethod.has_value();
  if (receiver->isFunction() && isSettingThis) {
    auto message = ErrorTextCallMethod::setsThis(member->name);
    this->state.diagnostics.error(member->loc(), message);
    return;
  }
  if (receiver->isError()) {
    return;
  }
  auto message = ErrorTextCallMethod::noProperty(member->name, receiver->name());
  this->state.diagnostics.error(member->loc(), message);
}

}
