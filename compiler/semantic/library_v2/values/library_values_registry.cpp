#include "compiler/semantic/library_v2/values/library_values_registry.h"
#include "compiler/program/library/array/array_method_table/array_method_table.h"
#include "compiler/program/library/builtin_names/builtin_property_table/builtin_property_table.h"
#include "compiler/program/library/builtin_names/global_member_table/global_member_table.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/program/library/builtin_names/type_name_table/type_name_table.h"
#include "compiler/program/library/dict/dict_method_table/dict_method_table.h"
#include "compiler/program/library/iterator/iterator_method_table/iterator_method_table.h"
#include "compiler/program/library/iterator/iterator_result_field_table/iterator_result_field_table.h"
#include "compiler/program/library/number/number_method_table/number_method_table.h"
#include "compiler/program/library/number/number_static_table/number_static_table.h"
#include "compiler/program/library/object/object_member_table/object_member_table.h"
#include "compiler/program/library/process/process_member_table/process_member_table.h"
#include "compiler/program/library/string/string_method_table/string_method_table.h"
#include "compiler/program/library/string/string_static_table/string_static_table.h"
#include "compiler/program/to_utf16/to_utf16.h"
#include "compiler/semantic/bodies/objects/dict_check_helpers/dict_check_helpers.h"
#include "compiler/semantic/code/dictionaries/code_dictionaries_registry_errors.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include <functional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

constexpr char kStringArg = 's';
constexpr char kPositionArg = 'p';
constexpr int64_t kMaxCodePoint = 0x10FFFF;
constexpr int64_t kMinRadix = 2;
constexpr int64_t kMaxRadix = 36;
constexpr int64_t kMaxFixedDigits = 100;
constexpr const char *kUnnamedNumber = "x";
constexpr unsigned kAnyCount = ~0u;
constexpr size_t kSplicePositions = 2;
constexpr const char *kEnumMemberPlaceholder = "A";

const GlobalName kGlobalFunctions[] = {
    GlobalName::STRING,     GlobalName::NUMBER,
    GlobalName::PARSE_INT,  GlobalName::PARSE_FLOAT,
    GlobalName::BOOLEAN,    GlobalName::DECIMAL,
    GlobalName::BIG_INT,    GlobalName::ENCODE_URI_COMPONENT,
    GlobalName::ENCODE_URI, GlobalName::DECODE_URI_COMPONENT,
    GlobalName::DECODE_URI, GlobalName::ESCAPE,
    GlobalName::UNESCAPE,
};

const std::pair<GlobalName, Builtin> kUriBuiltins[] = {
    {GlobalName::ENCODE_URI_COMPONENT, Builtin::UriEncodeComponent},
    {GlobalName::ENCODE_URI, Builtin::UriEncode},
    {GlobalName::DECODE_URI_COMPONENT, Builtin::UriDecodeComponent},
    {GlobalName::DECODE_URI, Builtin::UriDecode},
};

constexpr unsigned kMaxParseIntArgs = 2;

}

LibraryValuesRegistry::LibraryValuesRegistry(SemanticState &owner)
    : state(owner) {}

void LibraryValuesRegistry::pushLooseArgs(CallExpr *call) {
  for (Expr *&arg : llvm::reverse(call->args)) {
    this->state.walk.tasks.pushTask(CheckExprSlot{.slot = &arg, .expected = nullptr});
  }
}

void LibraryValuesRegistry::reportAfterLooseArgs(CallExpr *call, llvm::SMLoc loc,
                                                 const std::string &message) {
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(ReportError{.loc = loc, .message = message});

  return tasks.pushTask(WalkArgsLoosely{.call = call});
}

void LibraryValuesRegistry::checkStringMethod(const CheckStringMethod &task) {
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(AfterStringRegexTry{.call = task.call, .member = task.member});

  return tasks.pushTask(CheckStringRegexMethod{.call = task.call, .member = task.member});
}

void LibraryValuesRegistry::afterStringRegexTry(const AfterStringRegexTry &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  auto &tasks = this->state.walk.tasks;
  Expr *regex = tasks.takeChecked();
  if (regex != nullptr) {
    return tasks.pushChecked(regex);
  }
  tasks.pushChecked(call);
  auto found = this->checkMethodName(member->name);
  if (found.isErr()) {
    this->state.diagnostics.error(member->loc(), found.error().message);

    return tasks.pushTask(WalkArgsLoosely{.call = call});
  }
  const StringMethodInfo *method = found.value();
  const auto *stringCall = StringMethodTable::findCall(*method);
  auto arity =
      this->state.library.common.checkArity(call, stringCall->minArgs, stringCall->maxArgs);
  if (arity.isErr()) {
    return this->state.diagnostics.error(member->loc(), arity.error().message);
  }
  tasks.pushTask(FinishStringMethod{.call = call, .member = member, .method = method->method});

  return this->pushStringArgs(call, stringCall->args);
}

void LibraryValuesRegistry::pushStringArgs(CallExpr *call, llvm::StringRef argKinds) {
  auto &tasks = this->state.walk.tasks;
  size_t lastKind = argKinds.size() - 1;
  for (size_t index = call->args.size(); index > 0; --index) {
    Expr **slot = &call->args[index - 1];
    char kind = argKinds[std::min(index - 1, lastKind)];
    if (kind == kStringArg) {
      tasks.pushTask(CheckArgSlot{.slot = slot, .to = Type::string()});
    } else if (kind == kPositionArg) {
      tasks.pushTask(CheckPosition{.slot = slot});
    } else {
      tasks.pushTask(ToStringSlot{.slot = slot});
      tasks.pushTask(CheckExprSlot{.slot = slot, .expected = nullptr});
    }
  }
}

void LibraryValuesRegistry::finishStringMethod(const FinishStringMethod &task) {
  CallExpr *call = task.call;
  const auto *method = StringMethodTable::find(task.member->name);
  const auto *stringCall = StringMethodTable::findCall(*method);
  call->target = BuiltinCall{.builtin = stringCall->builtin};
  call->type = this->resultType(stringCall->result);

  return this->walkMethodRange(call, task.member, task.method);
}

void LibraryValuesRegistry::checkNumberStatic(const CheckNumberStatic &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  call->type = Type::error();
  bool isString = this->state.isGlobalName(member->object, GlobalName::STRING);
  if (isString) {
    return this->startStringStatic(call, member);
  }

  return this->startNumberStatic(call, member);
}

void LibraryValuesRegistry::startStringStatic(CallExpr *call, MemberExpr *member) {
  auto &tasks = this->state.walk.tasks;
  auto owner = GlobalNameTable::getName(GlobalName::STRING);
  auto found = StringStaticTable::find(member->name);
  if (!found.has_value()) {
    return this->reportNoProperty(call, member, owner);
  }
  switch (*found) {
    case StringStatic::RAW:
      return this->startStringRaw(call, member);
    case StringStatic::FROM_CHAR_CODE:
      tasks.pushChecked(call);
      return this->startFromCodes(call, Builtin::FromCharCode);
    case StringStatic::FROM_CODE_POINT:
      tasks.pushChecked(call);
      return this->startFromCodes(call, Builtin::FromCodePoint);
  }
}

void LibraryValuesRegistry::startNumberStatic(CallExpr *call, MemberExpr *member) {
  auto owner = GlobalNameTable::getName(GlobalName::NUMBER);
  auto found = NumberStaticTable::find(member->name);
  if (!found.has_value()) {
    return this->reportNoProperty(call, member, owner);
  }
  switch (*found) {
    case NumberStatic::PARSE_INT:
    case NumberStatic::PARSE_FLOAT:
      return this->startNumberParse(call, member);
    case NumberStatic::IS_INTEGER:
      return this->startNumberPredicate(call, member, Builtin::IsInteger);
    case NumberStatic::IS_SAFE_INTEGER:
      return this->startNumberPredicate(call, member, Builtin::IsSafeInteger);
  }
}

void LibraryValuesRegistry::reportNoProperty(CallExpr *call, MemberExpr *member,
                                             llvm::StringRef owner) {
  auto message = ErrorTextNumberStatic::noProperty(member->name, owner);
  this->state.walk.tasks.pushChecked(call);

  return this->reportAfterLooseArgs(call, member->loc(), message);
}

void LibraryValuesRegistry::startStringRaw(CallExpr *call, MemberExpr *member) {
  auto &tasks = this->state.walk.tasks;
  if (!call->tagged) {
    auto message = ErrorTextNumberStatic::rawTag();
    tasks.pushChecked(call);

    return this->reportAfterLooseArgs(call, member->loc(), message);
  }
  auto *texts = cast<ArrayLitExpr>(call->args[0]);
  auto *templateExpr = this->state.ast.make<TemplateExpr>(texts->loc());
  for (const ArrayLitExpr::Element &element : texts->elements) {
    auto *text = cast<StringLitExpr>(element.expr);
    templateExpr->quasis.push_back(text->value);
  }
  templateExpr->exprs.assign(call->args.begin() + 1, call->args.end());

  return tasks.pushTask(CheckTemplate{.templateExpr = templateExpr, .expected = nullptr});
}

void LibraryValuesRegistry::startFromCodes(CallExpr *call, Builtin builtin) {
  auto &tasks = this->state.walk.tasks;
  call->target = BuiltinCall{.builtin = builtin};
  call->type = Type::string();
  for (Expr *&arg : llvm::reverse(call->args)) {
    tasks.pushTask(AfterCodeArg{.slot = &arg, .isInt32 = builtin == Builtin::FromCharCode});
    tasks.pushTask(NumericArgSlot{.slot = &arg, .hint = nullptr});
  }
}

void LibraryValuesRegistry::afterCodeArg(const AfterCodeArg &task) {
  Expr **slot = task.slot;
  auto &tasks = this->state.walk.tasks;
  Expr *numeric = *slot;
  if (task.isInt32) {
    tasks.pushTask(WriteCheckedSlot{.slot = slot});
    tasks.pushTask(ToInt32{});

    return tasks.pushChecked(numeric);
  }
  if (numeric->type->isNumeric()) {
    this->walkCodePoint(numeric);
  }

  return tasks.pushTask(ConvertSlot{.slot = slot, .to = Type::doubleTy()});
}

void LibraryValuesRegistry::startNumberParse(CallExpr *call, MemberExpr *member) {
  auto draft = GlobalFunctionDraft{
      .call = call,
      .functionName = member->name,
      .loc = member->loc(),
      .isArgumentPlaced = false,
  };

  return this->startGlobalFunction(draft);
}

void LibraryValuesRegistry::startNumberPredicate(CallExpr *call, MemberExpr *member,
                                                 Builtin builtin) {
  auto &tasks = this->state.walk.tasks;
  tasks.pushChecked(call);
  auto arity = this->state.library.common.checkArity(call, 1, 1);
  if (arity.isErr()) {
    return this->state.diagnostics.error(member->loc(), arity.error().message);
  }
  call->target = BuiltinCall{.builtin = builtin};
  call->type = Type::boolTy();

  return tasks.pushTask(NumericArgSlot{.slot = &call->args[0], .hint = nullptr});
}

void LibraryValuesRegistry::checkNumberMethod(const CheckNumberMethod &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  auto &tasks = this->state.walk.tasks;
  tasks.pushChecked(call);
  auto converted = this->checkConversion(call, member);
  if (converted.isOk()) {
    return;
  }
  if (converted.error().code != LibraryValuesRegistryCheckConversionErrorCode::NOT_A_CONVERSION) {
    return this->state.diagnostics.error(member->loc(), converted.error().message);
  }
  const auto *method = NumberMethodTable::find(member->name);
  bool isToString = method != nullptr && method->method == NumberMethod::TO_STRING;
  bool isToFixed = method != nullptr && method->method == NumberMethod::TO_FIXED;
  if (isToString && call->args.size() == 1) {
    return this->startRadixString(call, member);
  }
  if (isToString) {
    return this->startPlainString(call, member);
  }
  if (isToFixed) {
    return this->startToFixed(call, member);
  }
  auto message = ErrorTextNumberMethod::noProperty(member->name, member->object->type->name());

  return this->reportAfterLooseArgs(call, member->loc(), message);
}

void LibraryValuesRegistry::startRadixString(CallExpr *call, MemberExpr *member) {
  auto &tasks = this->state.walk.tasks;
  call->target = BuiltinCall{.builtin = Builtin::NumToRadix};
  call->type = Type::string();
  auto what = ErrorTextNumberMethod::radix();
  tasks.pushTask(AfterDigitsArg{.call = call, .low = kMinRadix, .high = kMaxRadix, .what = what});
  tasks.pushTask(CheckPosition{.slot = &call->args[0]});

  return tasks.pushTask(ConvertSlot{.slot = &member->object, .to = Type::doubleTy()});
}

void LibraryValuesRegistry::startPlainString(CallExpr *call, MemberExpr *member) {
  auto &tasks = this->state.walk.tasks;
  auto arity = this->state.library.common.checkArity(call, 0, 0);
  if (arity.isErr()) {
    return this->state.diagnostics.error(member->loc(), arity.error().message);
  }
  call->args.push_back(member->object);
  call->target = BuiltinCall{.builtin = Builtin::StringOf};
  call->type = Type::string();

  return tasks.pushTask(ToStringSlot{.slot = &call->args.back()});
}

void LibraryValuesRegistry::startToFixed(CallExpr *call, MemberExpr *member) {
  auto &tasks = this->state.walk.tasks;
  auto arity = this->state.library.common.checkArity(call, 0, 1);
  if (arity.isErr()) {
    return this->state.diagnostics.error(member->loc(), arity.error().message);
  }
  call->target = BuiltinCall{.builtin = Builtin::NumToFixed};
  call->type = Type::string();
  if (!call->args.empty()) {
    auto what = ErrorTextNumberMethod::digits();
    tasks.pushTask(AfterDigitsArg{.call = call, .low = 0, .high = kMaxFixedDigits, .what = what});
    tasks.pushTask(CheckPosition{.slot = &call->args[0]});
  }

  return tasks.pushTask(ConvertSlot{.slot = &member->object, .to = Type::doubleTy()});
}

void LibraryValuesRegistry::afterDigitsArg(const AfterDigitsArg &task) {
  Expr *digits = task.call->args[0];
  auto between = this->state.flow.proofs.requireBetween(digits, task.low, task.high, task.what);
  bool isShown =
      between.isErr() && between.error().code != FlowProofsRequireBetweenErrorCode::EXEMPT;
  if (isShown) {
    this->state.diagnostics.error(digits->loc(), between.error().message);
  }
}

void LibraryValuesRegistry::checkGlobalCall(const CheckGlobalCall &task) {
  CallExpr *call = task.call;
  NameExpr *name = task.name;
  auto &tasks = this->state.walk.tasks;
  if (this->state.isGlobalName(name, GlobalName::REG_EXP)) {
    return this->startRegExpCall(call, name);
  }
  if (this->state.isGlobalName(name, GlobalName::DATE)) {
    return tasks.pushTask(CheckDateFunction{.call = call, .name = name});
  }
  if (!this->isGlobalFunction(name)) {
    return tasks.pushChecked(nullptr);
  }
  auto draft = GlobalFunctionDraft{
      .call = call,
      .functionName = name->name,
      .loc = name->loc(),
      .isArgumentPlaced = true,
  };

  return this->startGlobalFunction(draft);
}

void LibraryValuesRegistry::startRegExpCall(CallExpr *call, NameExpr *name) {
  auto &tasks = this->state.walk.tasks;
  if (this->state.library.common.hasSpread(call)) {
    tasks.pushChecked(call);

    return tasks.pushTask(RejectSpreadCall{.call = call});
  }

  return tasks.pushTask(CheckRegExpCall{.call = call, .name = name});
}

void LibraryValuesRegistry::startGlobalFunction(const GlobalFunctionDraft &draft) {
  CallExpr *call = draft.call;
  llvm::SMLoc loc = draft.loc;
  auto &tasks = this->state.walk.tasks;
  auto global = GlobalNameTable::find(draft.functionName);
  call->type = Type::error();
  if (this->state.library.common.hasSpread(call)) {
    tasks.pushChecked(call);

    return tasks.pushTask(RejectSpreadCall{.call = call});
  }
  if (global == GlobalName::DECIMAL) {
    return tasks.pushTask(CheckDecimalCall{.call = call, .loc = loc});
  }
  auto modern = this->checkNotLegacyEscape(call, global);
  if (modern.isErr()) {
    tasks.pushChecked(call);

    return this->reportAfterLooseArgs(call, loc, modern.error().message);
  }
  auto uri = this->uriBuiltin(global);
  if (uri.has_value()) {
    return this->startUriCall(draft, global, *uri);
  }
  if (global == GlobalName::BIG_INT) {
    auto quoted = ErrorTextBigInt::quoted(draft.functionName);
    auto bigint = ErrorTextBigInt::none(quoted);
    this->state.diagnostics.error(loc, bigint);
    tasks.pushChecked(call);

    return tasks.pushTask(WalkArgsLoosely{.call = call});
  }
  if (global == GlobalName::BOOLEAN) {
    return this->startBooleanCall(draft);
  }
  if (global == GlobalName::STRING) {
    return this->startStringCall(draft);
  }
  if (global == GlobalName::NUMBER) {
    return this->startNumberCall(draft);
  }
  if (global == GlobalName::PARSE_INT || global == GlobalName::PARSE_FLOAT) {
    return this->startParseCall(draft, global);
  }

  return tasks.pushChecked(call);
}

bool LibraryValuesRegistry::hasGlobalArity(const GlobalFunctionDraft &draft, unsigned min,
                                           unsigned max) {
  auto &tasks = this->state.walk.tasks;
  auto arity = this->state.library.common.checkArity(draft.call, min, max);
  if (arity.isOk()) {
    return true;
  }
  this->state.diagnostics.error(draft.loc, arity.error().message);
  tasks.pushChecked(draft.call);

  return false;
}

void LibraryValuesRegistry::startUriCall(const GlobalFunctionDraft &draft,
                                         std::optional<GlobalName> global, Builtin builtin) {
  CallExpr *call = draft.call;
  auto &tasks = this->state.walk.tasks;
  CallExpr *escaped = this->soleCallArg(call);
  bool isDecodingEscape = escaped != nullptr && global == GlobalName::DECODE_URI_COMPONENT &&
                          this->state.isGlobalName(escaped->callee, GlobalName::ESCAPE);
  if (isDecodingEscape) {
    auto message = ErrorTextGlobalFunction::fromBytes();
    escaped->type = Type::error();
    tasks.pushChecked(call);
    tasks.pushTask(ReportError{.loc = draft.loc, .message = message});

    return this->pushLooseArgs(escaped);
  }
  if (!this->hasGlobalArity(draft, 1, 1)) {
    return;
  }
  tasks.pushChecked(call);
  tasks.pushTask(AfterUriArg{.call = call, .global = global, .builtin = builtin});

  return tasks.pushTask(CheckExprSlot{.slot = &call->args[0], .expected = Type::string()});
}

void LibraryValuesRegistry::afterUriArg(const AfterUriArg &task) {
  CallExpr *call = task.call;
  auto &tasks = this->state.walk.tasks;
  const Type *argType = call->args[0]->type;
  bool isPrimitive = argType->isNumeric() || argType->isBool();
  call->target = BuiltinCall{.builtin = task.builtin};
  bool isDecoding =
      task.builtin == Builtin::UriDecodeComponent || task.builtin == Builtin::UriDecode;
  if (isDecoding) {
    tasks.pushTask(FinishGlobalResult{.call = call, .ok = Type::string()});
    tasks.pushTask(PreludeTypeOf{.type = PreludeType::URI_ERROR});
  } else {
    call->type = Type::string();
  }
  if (task.global == GlobalName::ENCODE_URI_COMPONENT && isPrimitive) {
    return tasks.pushTask(ToStringSlot{.slot = &call->args[0]});
  }

  return tasks.pushTask(CoerceSlot{.slot = &call->args[0], .to = Type::string(), .what = ""});
}

void LibraryValuesRegistry::finishGlobalResult(const FinishGlobalResult &task) {
  const Type *failure = this->state.walk.tasks.takeAnswer();
  task.call->type = Type::result(task.ok, failure);
}

void LibraryValuesRegistry::startBooleanCall(const GlobalFunctionDraft &draft) {
  CallExpr *call = draft.call;
  auto &tasks = this->state.walk.tasks;
  if (!this->hasGlobalArity(draft, 0, 1)) {
    return;
  }
  if (call->args.empty()) {
    auto *no = this->state.ast.make<BoolLitExpr>(call->loc(), false);
    no->type = Type::boolTy();

    return tasks.pushChecked(no);
  }
  tasks.pushTask(FinishBranchJoin{});

  return tasks.pushTask(StartBranch{.expr = call->args[0], .use = TruthUse::Condition});
}

void LibraryValuesRegistry::startStringCall(const GlobalFunctionDraft &draft) {
  CallExpr *call = draft.call;
  auto &tasks = this->state.walk.tasks;
  if (!this->hasGlobalArity(draft, 0, 1)) {
    return;
  }
  call->target = BuiltinCall{.builtin = Builtin::StringOf};
  call->type = Type::string();
  tasks.pushChecked(call);
  for (Expr *&arg : llvm::reverse(call->args)) {
    tasks.pushTask(ToStringSlot{.slot = &arg});
    tasks.pushTask(CheckExprSlot{.slot = &arg, .expected = nullptr});
  }
}

void LibraryValuesRegistry::startNumberCall(const GlobalFunctionDraft &draft) {
  CallExpr *call = draft.call;
  auto &tasks = this->state.walk.tasks;
  if (!this->hasGlobalArity(draft, 0, 1)) {
    return;
  }
  if (call->args.empty()) {
    tasks.pushChecked(call);

    return this->setNumberOfType(call);
  }
  tasks.pushTask(AfterNumberArg{.draft = draft});

  return tasks.pushTask(CheckExprSlot{.slot = &call->args[0], .expected = nullptr});
}

void LibraryValuesRegistry::afterNumberArg(const AfterNumberArg &task) {
  CallExpr *call = task.draft.call;
  auto &tasks = this->state.walk.tasks;
  Expr *arg = call->args[0];
  const Type *type = arg->type;
  if (type->isError()) {
    return tasks.pushChecked(call);
  }
  if (type->isDate()) {
    tasks.pushTask(DateToTime{});

    return tasks.pushChecked(arg);
  }
  tasks.pushChecked(call);
  auto numbered = this->checkNumberSource(type);
  if (numbered.isErr()) {
    llvm::SMLoc at = task.draft.isArgumentPlaced ? arg->loc() : task.draft.loc;

    return this->state.diagnostics.error(at, numbered.error().message);
  }
  if (type->nonOptional()->isNumeric()) {
    tasks.pushTask(FinishNumberOf{.call = call});

    return tasks.pushTask(ConvertSlot{.slot = &call->args[0], .to = Type::doubleTy()});
  }

  return this->setNumberOfType(call);
}

void LibraryValuesRegistry::finishNumberOf(const FinishNumberOf &task) {
  return this->setNumberOfType(task.call);
}

void LibraryValuesRegistry::setNumberOfType(CallExpr *call) {
  auto &tasks = this->state.walk.tasks;
  call->target = BuiltinCall{.builtin = Builtin::NumberOf};
  bool isParsing = !call->args.empty() && call->args[0]->type->isString();
  if (!isParsing) {
    call->type = Type::doubleTy();

    return;
  }
  tasks.pushTask(FinishGlobalResult{.call = call, .ok = Type::doubleTy()});

  return tasks.pushTask(PreludeTypeOf{.type = PreludeType::NUMBER_ERROR});
}

void LibraryValuesRegistry::startParseCall(const GlobalFunctionDraft &draft,
                                           std::optional<GlobalName> global) {
  CallExpr *call = draft.call;
  auto &tasks = this->state.walk.tasks;
  bool isInt = global == GlobalName::PARSE_INT;
  unsigned maxArgs = isInt ? kMaxParseIntArgs : 1;
  if (!this->hasGlobalArity(draft, 1, maxArgs)) {
    return;
  }
  tasks.pushChecked(call);
  call->target = BuiltinCall{.builtin = isInt ? Builtin::ParseInt : Builtin::ParseFloat};
  const Type *parsed = isInt ? Type::intTy() : Type::doubleTy();
  tasks.pushTask(FinishGlobalResult{.call = call, .ok = parsed});
  tasks.pushTask(PreludeTypeOf{.type = PreludeType::NUMBER_ERROR});
  if (call->args.size() == kMaxParseIntArgs) {
    tasks.pushTask(AfterCodeArg{.slot = &call->args[1], .isInt32 = true});
    tasks.pushTask(NumericArgSlot{.slot = &call->args[1], .hint = nullptr});
  }

  return tasks.pushTask(CheckArgSlot{.slot = &call->args[0], .to = Type::string()});
}

void LibraryValuesRegistry::checkArrayMethod(const CheckArrayMethod &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  auto &tasks = this->state.walk.tasks;
  const Type *arrayType = member->object->type;
  const auto *method = ArrayMethodTable::find(member->name);
  if (method == nullptr) {
    auto message = ErrorTextArrayMethod::noProperty(member->name, arrayType->name());
    tasks.pushChecked(call);

    return this->reportAfterLooseArgs(call, member->loc(), message);
  }
  auto draft = ArrayMethodDraft{
      .call = call,
      .member = member,
      .expected = task.expected,
      .arrayType = arrayType,
      .element = arrayType->inner(),
      .self = this->arrayVariable(member->object),
      .count = call->args.size(),
      .method = method,
  };
  bool isCallback = this->isCallbackMethod(method->method) ||
                    (method->method == ArrayMethod::SORT && draft.count != 0);
  if (isCallback) {
    return tasks.pushTask(
        CheckCallbackMethod{.call = call, .member = member, .expected = task.expected});
  }
  bool isIterating = method->method == ArrayMethod::KEYS || method->method == ArrayMethod::VALUES ||
                     method->method == ArrayMethod::ENTRIES;
  if (isIterating) {
    return tasks.pushTask(CheckArrayIterator{.call = call, .member = member});
  }
  tasks.pushChecked(call);

  return this->startArrayMethod(draft);
}

bool LibraryValuesRegistry::isCallbackMethod(ArrayMethod method) const {
  switch (method) {
    case ArrayMethod::FOR_EACH:
    case ArrayMethod::MAP:
    case ArrayMethod::FLAT_MAP:
    case ArrayMethod::FILTER:
    case ArrayMethod::REDUCE:
    case ArrayMethod::REDUCE_RIGHT:
    case ArrayMethod::FIND:
    case ArrayMethod::FIND_LAST:
    case ArrayMethod::FIND_INDEX:
    case ArrayMethod::FIND_LAST_INDEX:
    case ArrayMethod::SOME:
    case ArrayMethod::EVERY:
      return true;
    case ArrayMethod::KEYS:
    case ArrayMethod::VALUES:
    case ArrayMethod::ENTRIES:
    case ArrayMethod::PUSH:
    case ArrayMethod::UNSHIFT:
    case ArrayMethod::POP:
    case ArrayMethod::SHIFT:
    case ArrayMethod::AT:
    case ArrayMethod::SLICE:
    case ArrayMethod::CONCAT:
    case ArrayMethod::INDEX_OF:
    case ArrayMethod::LAST_INDEX_OF:
    case ArrayMethod::INCLUDES:
    case ArrayMethod::JOIN:
    case ArrayMethod::TO_STRING:
    case ArrayMethod::REVERSE:
    case ArrayMethod::FILL:
    case ArrayMethod::SORT:
    case ArrayMethod::SPLICE:
      return false;
  }

  return false;
}

void LibraryValuesRegistry::startArrayMethod(const ArrayMethodDraft &draft) {
  switch (draft.method->method) {
    case ArrayMethod::PUSH:
    case ArrayMethod::UNSHIFT:
      return this->startGrowingCall(draft);
    case ArrayMethod::POP:
    case ArrayMethod::SHIFT:
      return this->checkShrinkingCall(draft);
    case ArrayMethod::AT:
      return this->startAtCall(draft);
    case ArrayMethod::SLICE:
      return this->startSliceCall(draft);
    case ArrayMethod::CONCAT:
      return this->startConcatCall(draft);
    case ArrayMethod::INDEX_OF:
    case ArrayMethod::LAST_INDEX_OF:
    case ArrayMethod::INCLUDES:
      return this->startSearchCall(draft);
    case ArrayMethod::JOIN:
      return this->startJoinCall(draft);
    case ArrayMethod::TO_STRING:
      return this->checkToStringCall(draft);
    case ArrayMethod::REVERSE:
      return this->checkReverseCall(draft);
    case ArrayMethod::FILL:
      return this->startFillCall(draft);
    case ArrayMethod::SORT:
      return this->checkSortCall(draft);
    case ArrayMethod::SPLICE:
      return this->startSpliceCall(draft);
    case ArrayMethod::FOR_EACH:
    case ArrayMethod::MAP:
    case ArrayMethod::FLAT_MAP:
    case ArrayMethod::FILTER:
    case ArrayMethod::REDUCE:
    case ArrayMethod::REDUCE_RIGHT:
    case ArrayMethod::FIND:
    case ArrayMethod::FIND_LAST:
    case ArrayMethod::FIND_INDEX:
    case ArrayMethod::FIND_LAST_INDEX:
    case ArrayMethod::SOME:
    case ArrayMethod::EVERY:
    case ArrayMethod::KEYS:
    case ArrayMethod::VALUES:
    case ArrayMethod::ENTRIES:
      return;
  }
}

bool LibraryValuesRegistry::hasArrayArity(const ArrayMethodDraft &draft, unsigned min,
                                          unsigned max) {
  size_t count = draft.count;
  if (count >= min && count <= max) {
    return true;
  }
  auto message = ErrorTextArity::count(min, max, count);
  this->reportAfterLooseArgs(draft.call, draft.member->loc(), message);

  return false;
}

bool LibraryValuesRegistry::hasTextArity(const ArrayMethodDraft &draft, unsigned max) {
  auto text = this->checkTextElements(draft.element, draft.member->name);
  if (text.isErr()) {
    this->reportAfterLooseArgs(draft.call, draft.member->loc(), text.error().message);

    return false;
  }

  return this->hasArrayArity(draft, 0, max);
}

void LibraryValuesRegistry::pushElementArg(const ArrayMethodDraft &draft, size_t index) {
  auto &tasks = this->state.walk.tasks;
  Expr **slot = &draft.call->args[index];
  tasks.pushTask(CoerceSlot{.slot = slot, .to = draft.element, .what = ""});

  return tasks.pushTask(CheckExprSlot{.slot = slot, .expected = draft.element});
}

void LibraryValuesRegistry::pushPositionArgs(const ArrayMethodDraft &draft, size_t from) {
  for (size_t index = draft.count; index > from; --index) {
    this->state.walk.tasks.pushTask(CheckPosition{.slot = &draft.call->args[index - 1]});
  }
}

void LibraryValuesRegistry::setArrayCallType(const ArrayMethodDraft &draft, const Type *type) {
  draft.call->target = BuiltinCall{.builtin = draft.method->builtin};
  draft.call->type = type;
}

void LibraryValuesRegistry::startGrowingCall(const ArrayMethodDraft &draft) {
  CallExpr *call = draft.call;
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(FinishGrowingCall{.draft = draft, .isSpreading = call->hasSpread()});
  for (size_t index = draft.count; index > 0; --index) {
    this->pushPushedArg(draft, index - 1);
  }
}

void LibraryValuesRegistry::pushPushedArg(const ArrayMethodDraft &draft, size_t index) {
  auto &tasks = this->state.walk.tasks;
  if (!draft.call->spreads(index)) {
    return this->pushElementArg(draft, index);
  }
  Expr **slot = &draft.call->args[index];
  tasks.pushTask(CoerceSlot{.slot = slot, .to = draft.arrayType, .what = ""});

  return tasks.pushTask(CheckExprSlot{.slot = slot, .expected = draft.arrayType});
}

void LibraryValuesRegistry::finishGrowingCall(const FinishGrowingCall &task) {
  const ArrayMethodDraft &draft = task.draft;
  this->setArrayCallType(draft, Type::intTy());
  if (draft.self != nullptr && !task.isSpreading) {
    auto added = static_cast<int64_t>(draft.count);
    auto length = Term::lengthOf(draft.self);
    this->state.walk.flow.getFacts().shift(length, added);
  }
  const VarSymbol *except = task.isSpreading ? nullptr : draft.self;

  return this->state.walk.tasks.pushTask(
      NoteArrayResized{.array = draft.arrayType, .resize = Resize::GROWS, .except = except});
}

void LibraryValuesRegistry::checkShrinkingCall(const ArrayMethodDraft &draft) {
  if (!this->hasArrayArity(draft, 0, 0)) {
    return;
  }
  bool isNotEmpty = this->state.flow.proofs.canProveNotEmpty(draft.member->object);
  const Type *element = draft.element;
  const Type *taken = isNotEmpty ? element : Type::optional(element);
  this->setArrayCallType(draft, taken);
  draft.call->mayBeOutOfRange = !isNotEmpty;
  if (draft.self != nullptr) {
    this->shrinkLength(draft.self, isNotEmpty);
  }

  return this->state.walk.tasks.pushTask(
      NoteArrayResized{.array = draft.arrayType, .resize = Resize::SHRINKS, .except = draft.self});
}

void LibraryValuesRegistry::startAtCall(const ArrayMethodDraft &draft) {
  auto &tasks = this->state.walk.tasks;
  if (!this->hasArrayArity(draft, 1, 1)) {
    return;
  }
  tasks.pushTask(FinishAtCall{.draft = draft});

  return this->pushPositionArgs(draft, 0);
}

void LibraryValuesRegistry::finishAtCall(const FinishAtCall &task) {
  const ArrayMethodDraft &draft = task.draft;
  CallExpr *call = draft.call;
  bool isInBounds = this->state.flow.proofs.canProveAtInBounds(draft.member->object, call->args[0]);
  const Type *element = draft.element;
  const Type *taken = isInBounds ? element : Type::optional(element);
  this->setArrayCallType(draft, taken);
  call->mayBeOutOfRange = !isInBounds;
}

void LibraryValuesRegistry::startSliceCall(const ArrayMethodDraft &draft) {
  if (!this->hasArrayArity(draft, 0, 2)) {
    return;
  }
  this->setArrayCallType(draft, draft.arrayType);

  return this->pushPositionArgs(draft, 0);
}

void LibraryValuesRegistry::startConcatCall(const ArrayMethodDraft &draft) {
  auto &tasks = this->state.walk.tasks;
  this->setArrayCallType(draft, draft.arrayType);
  for (size_t index = draft.count; index > 0; --index) {
    Expr **slot = &draft.call->args[index - 1];
    tasks.pushTask(AfterConcatArg{.slot = slot, .draft = draft});
    tasks.pushTask(CheckExprSlot{.slot = slot, .expected = draft.arrayType});
  }
}

void LibraryValuesRegistry::afterConcatArg(const AfterConcatArg &task) {
  if ((*task.slot)->type == task.draft.arrayType) {
    return;
  }

  return this->state.walk.tasks.pushTask(
      CoerceSlot{.slot = task.slot, .to = task.draft.element, .what = ""});
}

void LibraryValuesRegistry::startSearchCall(const ArrayMethodDraft &draft) {
  if (!this->hasArrayArity(draft, 1, 2)) {
    return;
  }
  const Type *element = draft.element;
  if (element->holdsResults()) {
    auto message = ErrorTextArrayMethod::results(draft.member->name);

    return this->reportAfterLooseArgs(draft.call, draft.member->loc(), message);
  }
  if (element->nonOptional()->isFunction()) {
    this->state.inventory.comparesFunctions = true;
  }
  bool isIncludes = draft.method->method == ArrayMethod::INCLUDES;
  this->setArrayCallType(draft, isIncludes ? Type::boolTy() : Type::intTy());
  this->pushPositionArgs(draft, 1);

  return this->pushElementArg(draft, 0);
}

void LibraryValuesRegistry::startJoinCall(const ArrayMethodDraft &draft) {
  auto &tasks = this->state.walk.tasks;
  if (!this->hasTextArity(draft, 1)) {
    return;
  }
  this->setArrayCallType(draft, Type::string());
  if (draft.count != 1) {
    return;
  }
  Expr **separator = &draft.call->args[0];
  tasks.pushTask(CoerceSlot{.slot = separator, .to = Type::string(), .what = ""});

  return tasks.pushTask(CheckExprSlot{.slot = separator, .expected = Type::string()});
}

void LibraryValuesRegistry::checkToStringCall(const ArrayMethodDraft &draft) {
  if (!this->hasTextArity(draft, 0)) {
    return;
  }

  return this->setArrayCallType(draft, Type::string());
}

void LibraryValuesRegistry::checkReverseCall(const ArrayMethodDraft &draft) {
  if (!this->hasArrayArity(draft, 0, 0)) {
    return;
  }

  return this->setArrayCallType(draft, draft.arrayType);
}

void LibraryValuesRegistry::startFillCall(const ArrayMethodDraft &draft) {
  if (!this->hasArrayArity(draft, 1, 3)) {
    return;
  }
  this->setArrayCallType(draft, draft.arrayType);
  this->pushPositionArgs(draft, 1);

  return this->pushElementArg(draft, 0);
}

void LibraryValuesRegistry::checkSortCall(const ArrayMethodDraft &draft) {
  auto ordered = this->checkSortable(draft.element);
  if (ordered.isErr()) {
    return this->state.diagnostics.error(draft.member->loc(), ordered.error().message);
  }

  return this->setArrayCallType(draft, draft.arrayType);
}

void LibraryValuesRegistry::startSpliceCall(const ArrayMethodDraft &draft) {
  auto &tasks = this->state.walk.tasks;
  if (!this->hasArrayArity(draft, 1, kAnyCount)) {
    return;
  }
  this->setArrayCallType(draft, draft.arrayType);
  tasks.pushTask(
      NoteArrayResized{.array = draft.arrayType, .resize = Resize::CHANGES, .except = nullptr});
  for (size_t index = draft.count; index > 0; --index) {
    if (index - 1 >= kSplicePositions) {
      this->pushElementArg(draft, index - 1);
    } else {
      tasks.pushTask(CheckPosition{.slot = &draft.call->args[index - 1]});
    }
  }
}

void LibraryValuesRegistry::checkDictMethod(const CheckDictMethod &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  auto &tasks = this->state.walk.tasks;
  const Type *type = member->object->type;
  auto text = DictCheckHelpers::dictText(member->object, type);
  call->type = Type::error();
  tasks.pushChecked(call);
  if (type->isRecord()) {
    auto message = this->recordMethodMessage(call, member, text);

    return this->reportAfterLooseArgs(call, member->loc(), message);
  }
  bool isMap = type->isMap();
  const auto *found = DictMethodTable::find(member->name);
  bool isKnown = found != nullptr && this->isDictMethodOn(*found, type);
  if (!isKnown) {
    auto message = ErrorTextDictMethod::noProperty(member->name, type->name(), text, isMap);

    return this->reportAfterLooseArgs(call, member->loc(), message);
  }
  auto dict = DictDraft{
      .call = call,
      .member = member,
      .type = type,
      .key = type->keyType(),
      .value = type->valueType(),
      .builtin = DictMethodTable::getBuiltin(*found, isMap),
      .name = member->name,
      .isMap = isMap,
  };

  return this->startDictMethod(dict, *found);
}

std::string LibraryValuesRegistry::recordMethodMessage(const CallExpr *call,
                                                       const MemberExpr *member,
                                                       const std::string &text) const {
  const Type *type = member->object->type;
  std::string key = text + "." + member->name.str();
  auto inherited = ObjectMemberTable::find(member->name);
  bool hasOwn = inherited == ObjectMember::HAS_OWN_PROPERTY && call->args.size() == 1;
  if (hasOwn) {
    return ErrorTextRecordMethod::hasOwn(key, text);
  }
  if (type->valueType()->isFunction()) {
    return ErrorTextRecordMethod::undefined(key);
  }

  return ErrorTextRecordMethod::noMethods(key, type->valueType()->name(), text);
}

bool LibraryValuesRegistry::isDictMethodOn(const DictMethodInfo &method, const Type *type) const {
  if (type->isMap()) {
    return DictMethodTable::isOnMap(method);
  }

  return DictMethodTable::isOnSet(method);
}

bool LibraryValuesRegistry::hasDictArity(const DictDraft &dict, unsigned min, unsigned max) {
  auto arity = this->state.library.common.checkArity(dict.call, min, max);
  if (arity.isErr()) {
    this->state.diagnostics.error(dict.member->loc(), arity.error().message);
  }

  return arity.isOk();
}

void LibraryValuesRegistry::setDictCallType(const DictDraft &dict, const Type *type) {
  dict.call->target = BuiltinCall{.builtin = dict.builtin};
  dict.call->type = type;
}

void LibraryValuesRegistry::startDictMethod(const DictDraft &dict, const DictMethodInfo &found) {
  CallExpr *call = dict.call;
  auto &tasks = this->state.walk.tasks;
  switch (found.method) {
    case DictMethod::GET:
      if (!this->hasDictArity(dict, 1, 1)) {
        return;
      }
      this->setDictCallType(dict, Type::optional(dict.value));
      return tasks.pushTask(CheckArgSlot{.slot = &call->args[0], .to = dict.key});
    case DictMethod::SET:
      if (!this->hasDictArity(dict, 2, 2)) {
        return;
      }
      this->setDictCallType(dict, dict.type);
      tasks.pushTask(CheckArgSlot{.slot = &call->args[1], .to = dict.value});
      return tasks.pushTask(CheckArgSlot{.slot = &call->args[0], .to = dict.key});
    case DictMethod::ADD:
      if (!this->hasDictArity(dict, 1, 1)) {
        return;
      }
      this->setDictCallType(dict, dict.type);
      return tasks.pushTask(CheckArgSlot{.slot = &call->args[0], .to = dict.key});
    case DictMethod::HAS:
    case DictMethod::DELETE_KEY:
      if (!this->hasDictArity(dict, 1, 1)) {
        return;
      }
      this->setDictCallType(dict, Type::boolTy());
      return tasks.pushTask(CheckArgSlot{.slot = &call->args[0], .to = dict.key});
    case DictMethod::CLEAR:
      if (!this->hasDictArity(dict, 0, 0)) {
        return;
      }
      return this->setDictCallType(dict, Type::voidTy());
    case DictMethod::KEYS:
    case DictMethod::VALUES:
    case DictMethod::ENTRIES: {
      if (!this->hasDictArity(dict, 0, 0)) {
        return;
      }
      const Type *iterated = this->iteratedDictType(dict, found.method);
      return this->setDictCallType(dict, iterated);
    }
    case DictMethod::FOR_EACH:
      return this->startDictForEach(dict);
    case DictMethod::UNION:
    case DictMethod::INTERSECTION:
    case DictMethod::DIFFERENCE:
    case DictMethod::SYMMETRIC_DIFFERENCE:
    case DictMethod::IS_SUBSET_OF:
    case DictMethod::IS_SUPERSET_OF:
    case DictMethod::IS_DISJOINT_FROM: {
      if (!this->hasDictArity(dict, 1, 1)) {
        return;
      }
      bool isTesting = found.form.is<SetTestDictMethod>();
      tasks.pushTask(AfterSetCombineArg{.dict = dict, .isTesting = isTesting});
      return tasks.pushTask(CheckExprSlot{.slot = &call->args[0], .expected = dict.type});
    }
  }
}

const Type *LibraryValuesRegistry::iteratedDictType(const DictDraft &dict,
                                                    DictMethod method) const {
  if (method == DictMethod::ENTRIES) {
    const Type *second = dict.isMap ? dict.value : dict.key;
    const Type *pair = Type::tuple({dict.key, second});
    return Type::iterator(pair);
  }
  if (method == DictMethod::VALUES && dict.isMap) {
    return Type::iterator(dict.value);
  }

  return Type::iterator(dict.key);
}

void LibraryValuesRegistry::startDictForEach(const DictDraft &dict) {
  CallExpr *call = dict.call;
  auto &tasks = this->state.walk.tasks;
  bool isThisArg = call->args.size() == 2;
  if (isThisArg) {
    auto message = ErrorTextDictForEach::thisArg();

    return this->reportAfterLooseArgs(call, call->args[1]->loc(), message);
  }
  if (!this->hasDictArity(dict, 1, 1)) {
    return;
  }
  const Type *item = dict.isMap ? dict.value : dict.key;
  auto signature = Signature{
      .params = {item, dict.key, dict.type},
      .voidResult = true,
      .pad = false,
  };
  unsigned paramCount = dict.isMap ? 2 : 1;
  this->setDictCallType(dict, Type::voidTy());
  tasks.pushTask(CalledFunction{});

  return tasks.pushTask(CheckFunctionArgSlot{
      .slot = &call->args[0], .expected = signature, .primary = paramCount, .method = dict.name});
}

void LibraryValuesRegistry::afterSetCombineArg(const AfterSetCombineArg &task) {
  const DictDraft &dict = task.dict;
  Expr *other = dict.call->args[0];
  const Type *otherType = other->type;
  if (otherType->isError()) {
    return;
  }
  bool isSet = otherType->isDict() && !otherType->isRecord() && otherType->keyType() == dict.key;
  if (!isSet) {
    auto message = ErrorTextSetCombine::notSet(dict.name, dict.key->name(), otherType->name());

    return this->state.diagnostics.error(other->loc(), message);
  }

  return this->setDictCallType(dict, task.isTesting ? Type::boolTy() : dict.type);
}

void LibraryValuesRegistry::checkMapStatic(const CheckMapStatic &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  auto &tasks = this->state.walk.tasks;
  auto found = GlobalMemberTable::find(GlobalName::MAP, member->name);
  call->type = Type::error();
  if (found == GlobalMember::MAP_GROUP_BY) {
    return this->startGroupBy(call, member, GroupByOwner::MAP);
  }
  auto message = ErrorTextMapStatic::unknown(member->name);
  tasks.pushChecked(call);

  return this->reportAfterLooseArgs(call, member->loc(), message);
}

void LibraryValuesRegistry::startGroupBy(CallExpr *call, MemberExpr *member, GroupByOwner owner) {
  auto &tasks = this->state.walk.tasks;
  auto groupBy =
      owner == GroupByOwner::MAP ? GlobalMember::MAP_GROUP_BY : GlobalMember::OBJECT_GROUP_BY;
  std::string functionName = GlobalMemberTable::getQualifiedName(groupBy);
  tasks.pushChecked(call);
  if (this->state.library.common.hasSpread(call)) {
    return tasks.pushTask(RejectSpreadCall{.call = call});
  }
  auto arity = this->state.library.common.checkArity(call, 2, 2);
  if (arity.isErr()) {
    return this->state.diagnostics.error(member->loc(), arity.error().message);
  }
  auto what = ErrorTextFunctionName::quoted(functionName);
  auto draft = GroupByDraft{
      .call = call,
      .owner = owner,
      .functionName = functionName,
      .element = nullptr,
  };
  tasks.pushTask(AfterGroupSource{.draft = draft});

  return tasks.pushTask(CheckDictSourceSlot{
      .slot = &call->args[0], .isPairs = false, .wanted = nullptr, .what = what});
}

void LibraryValuesRegistry::afterGroupSource(const AfterGroupSource &task) {
  auto &tasks = this->state.walk.tasks;
  GroupByDraft draft = task.draft;
  CallExpr *call = draft.call;
  draft.element = tasks.takeAnswer();
  if (draft.element->isError()) {
    return tasks.pushTask(CheckExprSlot{.slot = &call->args[1], .expected = nullptr});
  }
  auto signature = Signature{
      .params = {draft.element, Type::intTy()},
      .pad = false,
  };
  tasks.pushTask(AfterGroupCallback{.draft = draft});
  tasks.pushTask(CalledFunction{});

  return tasks.pushTask(CheckFunctionArgSlot{
      .slot = &call->args[1], .expected = signature, .primary = 1, .method = draft.functionName});
}

void LibraryValuesRegistry::afterGroupCallback(const AfterGroupCallback &task) {
  const GroupByDraft &draft = task.draft;
  CallExpr *call = draft.call;
  const Type *callback = call->args[1]->type;
  if (!callback->isFunction()) {
    return;
  }
  auto key = this->checkGroupKey(draft);
  if (key.isErr()) {
    return this->state.diagnostics.error(call->args[1]->loc(), key.error().message);
  }
  auto kind = draft.owner == GroupByOwner::MAP ? Type::DictKind::Map : Type::DictKind::Record;
  const Type *grouped = Type::array(draft.element);
  call->target = BuiltinCall{.builtin = Builtin::DictGroupBy};
  call->type = Type::dict(kind, key.value(), grouped);
}

LibraryValuesRegistry::ResultGroupKeyType
LibraryValuesRegistry::checkGroupKey(const GroupByDraft &draft) const {
  const Type *key = draft.call->args[1]->type->ret();
  if (key->isVoid() || key->isUndefined()) {
    auto message = ErrorTextGroupBy::noKey(draft.functionName);

    return err(LibraryValuesRegistryGroupKeyError{
        .code = LibraryValuesRegistryGroupKeyErrorCode::NO_GROUP_KEY,
        .message = message,
    });
  }
  if (draft.owner == GroupByOwner::OBJECT && !key->isString()) {
    auto message = ErrorTextGroupBy::keys(key->name());

    return err(LibraryValuesRegistryGroupKeyError{
        .code = LibraryValuesRegistryGroupKeyErrorCode::KEYS_NOT_STRINGS,
        .message = message,
    });
  }
  if (key->isUnknown()) {
    auto message = ErrorTextKeyType::unknown();

    return err(LibraryValuesRegistryGroupKeyError{
        .code = LibraryValuesRegistryGroupKeyErrorCode::BAD_KEY_TYPE,
        .message = message,
    });
  }
  if (key->nonOptional()->isResult()) {
    auto message = ErrorTextKeyType::result();

    return err(LibraryValuesRegistryGroupKeyError{
        .code = LibraryValuesRegistryGroupKeyErrorCode::BAD_KEY_TYPE,
        .message = message,
    });
  }

  return ok(key);
}

void LibraryValuesRegistry::checkObjectCall(const CheckObjectCall &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  auto &tasks = this->state.walk.tasks;
  llvm::StringRef functionName = member->name;
  auto objectMember = GlobalMemberTable::find(GlobalName::OBJECT, functionName);
  call->type = Type::error();
  if (objectMember == GlobalMember::OBJECT_GROUP_BY) {
    return this->startGroupBy(call, member, GroupByOwner::OBJECT);
  }
  if (objectMember == GlobalMember::OBJECT_FROM_ENTRIES) {
    return this->startFromEntries(call, member, task.expected);
  }
  bool isListing = objectMember == GlobalMember::OBJECT_KEYS ||
                   objectMember == GlobalMember::OBJECT_VALUES ||
                   objectMember == GlobalMember::OBJECT_ENTRIES;
  if (!isListing) {
    auto message = ErrorTextObjectCall::noSupport(functionName);
    tasks.pushChecked(call);

    return this->reportAfterLooseArgs(call, member->loc(), message);
  }
  if (this->state.library.common.hasSpread(call)) {
    tasks.pushChecked(call);

    return tasks.pushTask(RejectSpreadCall{.call = call});
  }
  auto arity = this->state.library.common.checkArity(call, 1, 1);
  if (arity.isErr()) {
    this->state.diagnostics.error(member->loc(), arity.error().message);

    return tasks.pushChecked(call);
  }
  auto listing = ListingDraft{
      .call = call,
      .listing = *objectMember,
      .functionName = functionName,
  };

  return this->startObjectListing(listing);
}

void LibraryValuesRegistry::startFromEntries(CallExpr *call, MemberExpr *member,
                                             const Type *expected) {
  auto &tasks = this->state.walk.tasks;
  tasks.pushChecked(call);
  if (this->state.library.common.hasSpread(call)) {
    return tasks.pushTask(RejectSpreadCall{.call = call});
  }
  auto arity = this->state.library.common.checkArity(call, 1, 1);
  if (arity.isErr()) {
    return this->state.diagnostics.error(member->loc(), arity.error().message);
  }
  const Type *wanted = expected != nullptr ? expected->nonOptional() : nullptr;
  const Type *record = wanted != nullptr && wanted->isRecord() ? wanted : nullptr;
  if (record != nullptr) {
    return this->startEntriesSource(call, record);
  }
  tasks.pushTask(AfterEntriesPairs{.call = call});

  return tasks.pushTask(InferPairsSlot{.slot = &call->args[0]});
}

void LibraryValuesRegistry::afterEntriesPairs(const AfterEntriesPairs &task) {
  CallExpr *call = task.call;
  const Type *pair = this->state.walk.tasks.takeAnswer();
  if (pair == nullptr) {
    return this->startEntriesSource(call, nullptr);
  }
  if (pair->isError()) {
    return;
  }

  return this->finishEntries(call, nullptr, pair);
}

void LibraryValuesRegistry::startEntriesSource(CallExpr *call, const Type *record) {
  auto &tasks = this->state.walk.tasks;
  const Type *wanted =
      record != nullptr ? Type::tuple({Type::string(), record->valueType()}) : nullptr;
  auto functionName = GlobalMemberTable::getQualifiedName(GlobalMember::OBJECT_FROM_ENTRIES);
  auto quoted = ErrorTextFunctionName::quoted(functionName);
  tasks.pushTask(AfterEntriesSource{.call = call, .record = record});

  return tasks.pushTask(CheckDictSourceSlot{
      .slot = &call->args[0], .isPairs = true, .wanted = wanted, .what = quoted});
}

void LibraryValuesRegistry::afterEntriesSource(const AfterEntriesSource &task) {
  const Type *element = this->state.walk.tasks.takeAnswer();
  if (element->isError()) {
    return;
  }

  return this->finishEntries(task.call, task.record, element);
}

void LibraryValuesRegistry::finishEntries(CallExpr *call, const Type *record, const Type *element) {
  if (record != nullptr) {
    call->target = BuiltinCall{.builtin = Builtin::DictNew};
    call->type = record;

    return;
  }
  auto made = this->checkEntriesRecord(element);
  if (made.isErr()) {
    return this->state.diagnostics.error(call->args[0]->loc(), made.error().message);
  }
  call->target = BuiltinCall{.builtin = Builtin::DictNew};
  call->type = made.value();
}

void LibraryValuesRegistry::startObjectListing(const ListingDraft &listing) {
  CallExpr *call = listing.call;
  auto &tasks = this->state.walk.tasks;
  auto *name = dyn_cast<NameExpr>(call->args[0]);
  EnumDecl *enumDecl =
      name != nullptr ? this->state.walk.scopes.findName(name->name).enumDecl() : nullptr;
  if (enumDecl != nullptr) {
    tasks.pushTask(AfterListingEnum{.listing = listing, .enumDecl = enumDecl});

    return tasks.pushTask(ResolveEnum{.enumDecl = enumDecl});
  }
  tasks.pushChecked(call);
  tasks.pushTask(AfterListingArg{.listing = listing});

  return tasks.pushTask(CheckExprSlot{.slot = &call->args[0], .expected = nullptr});
}

void LibraryValuesRegistry::afterListingEnum(const AfterListingEnum &task) {
  const ListingDraft &listing = task.listing;
  CallExpr *call = listing.call;
  EnumDecl *enumDecl = task.enumDecl;
  auto &tasks = this->state.walk.tasks;
  const Type *values = enumDecl->valuesType();
  if (!enumDecl->isString && listing.listing != GlobalMember::OBJECT_KEYS) {
    llvm::StringRef first =
        enumDecl->members.empty() ? kEnumMemberPlaceholder : enumDecl->members.front().name;
    auto message = ErrorTextObjectCall::numberEnum(listing.functionName, enumDecl->name, first);
    this->state.diagnostics.error(call->args[0]->loc(), message);

    return tasks.pushChecked(call);
  }
  auto *list = this->state.ast.make<ArrayLitExpr>(call->loc());
  for (const EnumMember &enumMember : enumDecl->members) {
    auto *element = this->enumListElement(call, enumDecl, enumMember, listing.listing);
    list->elements.push_back(ArrayLitExpr::Element{.expr = element, .spread = false});
  }
  const Type *listType = this->objectCallType(listing.listing, values);
  tasks.pushTask(AfterEnumListing{.enumDecl = enumDecl, .at = call->args[0]->loc()});

  return tasks.pushTask(CheckExpr{.expr = list, .expected = listType});
}

void LibraryValuesRegistry::afterEnumListing(const AfterEnumListing &task) {
  auto &tasks = this->state.walk.tasks;
  Expr *checked = tasks.takeChecked();
  this->state.inventory.namedUses[checked] = NamedUse{task.enumDecl, task.at, ""};

  return tasks.pushChecked(checked);
}

void LibraryValuesRegistry::afterListingArg(const AfterListingArg &task) {
  const ListingDraft &listing = task.listing;
  CallExpr *call = listing.call;
  auto &tasks = this->state.walk.tasks;
  Expr *arg = call->args[0];
  const Type *type = arg->type;
  if (type->isError()) {
    return;
  }
  if (type->nonOptional()->isJson()) {
    return this->setListingType(listing, Type::json());
  }
  if (type->isOptional() && (type->inner()->isObject() || type->inner()->isDict())) {
    auto message = ErrorTextObjectCall::undefined();

    return this->state.diagnostics.error(arg->loc(), message);
  }
  if (type->isDict()) {
    auto listed = this->checkDictObjectCall(call, listing.functionName);
    if (listed.isErr()) {
      this->state.diagnostics.error(arg->loc(), listed.error().message);
    }
    return;
  }
  bool isNotObject = !type->isObject() || type->isTuple() || type->isRegExp() || type->isMatch() ||
                     type->isDate() || type->isIntl() || type->isBytes() || type->isCrypto();
  if (isNotObject) {
    auto message = ErrorTextObjectCall::needObject(listing.functionName, type->name());

    return this->state.diagnostics.error(arg->loc(), message);
  }
  if (listing.listing == GlobalMember::OBJECT_KEYS) {
    return this->setListingType(listing, nullptr);
  }
  ClassDecl *classDecl = type->classDecl();
  if (classDecl == nullptr) {
    return this->startListedFields(listing, type);
  }
  tasks.pushTask(AfterListedClass{.listing = listing, .type = type});

  return tasks.pushTask(CompleteClass{.classDecl = classDecl});
}

void LibraryValuesRegistry::setListingType(const ListingDraft &listing, const Type *value) {
  listing.call->target = BuiltinCall{.builtin = this->objectCallBuiltin(listing.listing)};
  listing.call->type = this->objectCallType(listing.listing, value);
}

void LibraryValuesRegistry::afterListedClass(const AfterListedClass &task) {
  const ListingDraft &listing = task.listing;
  Expr *arg = listing.call->args[0];
  ClassDecl *classDecl = task.type->classDecl();
  if (this->state.library.emitters.isEmitterClass(classDecl)) {
    auto message = ErrorTextObjectCall::emitter(listing.functionName);

    return this->state.diagnostics.error(arg->loc(), message);
  }
  if (classDecl->hasMethodFields()) {
    auto message = ErrorTextObjectCall::methods(listing.functionName);

    return this->state.diagnostics.error(arg->loc(), message);
  }

  return this->startListedFields(listing, task.type);
}

void LibraryValuesRegistry::startListedFields(const ListingDraft &listing, const Type *type) {
  auto &tasks = this->state.walk.tasks;
  auto subtypes = std::vector<const Type *>();
  for (const Type *candidate : this->state.inventory.subtypes) {
    if (candidate != type && candidate->isSubtypeOf(type)) {
      subtypes.push_back(candidate);
    }
  }
  tasks.pushTask(AfterListedFields{.listing = listing, .type = type, .subtypes = subtypes});
  for (const Type *subtype : llvm::reverse(subtypes)) {
    ClassDecl *subclass = subtype->classDecl();
    if (subclass != nullptr) {
      tasks.pushTask(CompleteClass{.classDecl = subclass});
    }
  }
}

void LibraryValuesRegistry::afterListedFields(const AfterListedFields &task) {
  const ListingDraft &listing = task.listing;
  Expr *arg = listing.call->args[0];
  auto fields = std::vector<OwnedField>();
  this->addOwnedFields(task.type, fields);
  for (const Type *subtype : task.subtypes) {
    this->addOwnedFields(subtype, fields);
  }
  auto element = this->checkListedElement(task.type, listing.functionName, fields);
  if (element.isErr()) {
    return this->state.diagnostics.error(arg->loc(), element.error().message);
  }

  return this->setListingType(listing, element.value());
}

void LibraryValuesRegistry::addOwnedFields(const Type *owner,
                                           std::vector<OwnedField> &fields) const {
  for (const Field &field : owner->fields()) {
    fields.push_back(OwnedField{.owner = owner, .field = &field});
  }
}

LibraryValuesRegistry::ResultListedType
LibraryValuesRegistry::checkListedElement(const Type *type, llvm::StringRef functionName,
                                          const std::vector<OwnedField> &fields) const {
  const Type *element = nullptr;
  for (const OwnedField &owned : fields) {
    const Type *candidate = owned.field->type->nonOptional();
    const Type *both = element != nullptr ? Type::unionOf({element, candidate}) : candidate;
    auto problem = UnionSlots::unionProblem(both);
    if (!problem.empty()) {
      auto message = ErrorTextObjectCall::noUnion(functionName, owned.owner->name(), problem);

      return err(LibraryValuesRegistryListedTypeError{
          .code = LibraryValuesRegistryListedTypeErrorCode::NO_COMMON_TYPE,
          .message = message,
      });
    }
    element = both;
  }
  if (element == nullptr) {
    auto message = ErrorTextObjectCall::noFields(type->name(), functionName);

    return err(LibraryValuesRegistryListedTypeError{
        .code = LibraryValuesRegistryListedTypeErrorCode::NO_FIELDS,
        .message = message,
    });
  }

  return ok(element);
}

Expr *LibraryValuesRegistry::enumListElement(const CallExpr *call, const EnumDecl *enumDecl,
                                             const EnumMember &enumMember, GlobalMember listing) {
  auto text = ToUtf16::toUtf16(enumMember.name);
  auto *name = this->state.ast.make<StringLitExpr>(call->loc(), text);
  auto *owner = this->state.ast.make<NameExpr>(call->loc(), enumDecl->name);
  auto *member = this->state.ast.make<MemberExpr>(call->loc(), owner, enumMember.name);
  if (listing == GlobalMember::OBJECT_KEYS) {
    return name;
  }
  if (listing != GlobalMember::OBJECT_ENTRIES) {
    return member;
  }
  auto *pair = this->state.ast.make<ArrayLitExpr>(call->loc());
  pair->elements.push_back(ArrayLitExpr::Element{.expr = name, .spread = false});
  pair->elements.push_back(ArrayLitExpr::Element{.expr = member, .spread = false});

  return pair;
}

void LibraryValuesRegistry::markFused(const MarkFused &task) {
  auto *call = dyn_cast<CallExpr>(task.expr);
  while (call != nullptr) {
    auto *source = this->markStageFused(call);
    call = llvm::dyn_cast_or_null<CallExpr>(source);
  }
}

LibraryValuesRegistry::ResultEntriesRecord
LibraryValuesRegistry::checkEntriesRecord(const Type *element) const {
  const Type *keys = element->fields()[0].type;
  if (!keys->isString()) {
    auto message = ErrorTextFromEntries::keys(keys->name());

    return err(LibraryValuesRegistryCheckFromEntriesError{
        .code = LibraryValuesRegistryCheckFromEntriesErrorCode::KEYS_NOT_STRINGS,
        .message = message,
    });
  }
  const Type *values = element->fields()[1].type;
  if (values->isUndefined()) {
    auto message = ErrorTextFromEntries::undefined();

    return err(LibraryValuesRegistryCheckFromEntriesError{
        .code = LibraryValuesRegistryCheckFromEntriesErrorCode::VALUES_UNDEFINED,
        .message = message,
    });
  }
  const Type *record = Type::dict(Type::DictKind::Record, Type::string(), values);

  return ok(record);
}

bool LibraryValuesRegistry::isHoldingUnknown(const Type *type) const {
  const Type *current = type;
  while (current->isOptional() || current->isArray()) {
    current = current->inner();
  }

  return current->isUnknown();
}

bool LibraryValuesRegistry::isHoldingJson(const Type *type) const {
  const Type *current = type;
  while (current->isOptional() || current->isArray()) {
    current = current->inner();
  }

  return current->isJson();
}

void LibraryValuesRegistry::checkArrayIterator(const CheckArrayIterator &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  auto &tasks = this->state.walk.tasks;
  tasks.pushChecked(call);
  const Type *element = member->object->type->inner();
  auto arity = this->state.library.common.checkArity(call, 0, 0);
  if (arity.isErr()) {
    return this->state.diagnostics.error(member->loc(), arity.error().message);
  }
  const auto *method = ArrayMethodTable::find(member->name);
  call->target = BuiltinCall{.builtin = this->arrayIteratorBuiltin(method)};
  call->type = this->arrayIteratorType(method, element);
}

void LibraryValuesRegistry::checkIteratorFrom(const CheckIteratorFrom &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  auto &tasks = this->state.walk.tasks;
  tasks.pushChecked(call);
  if (this->state.library.common.hasSpread(call)) {
    return tasks.pushTask(RejectSpreadCall{.call = call});
  }
  auto found = GlobalMemberTable::find(GlobalName::ITERATOR, member->name);
  if (found != GlobalMember::ITERATOR_FROM) {
    auto message = ErrorTextStaticFrom::iterator(member->name);

    return this->reportAfterLooseArgs(call, member->loc(), message);
  }
  auto arity = this->state.library.common.checkArity(call, 1, 1);
  if (arity.isErr()) {
    return this->state.diagnostics.error(member->loc(), arity.error().message);
  }
  tasks.pushTask(AfterIteratorFromSource{.call = call});
  tasks.pushTask(IterableOfSlot{.slot = &call->args[0]});

  return tasks.pushTask(CheckExprSlot{.slot = &call->args[0], .expected = nullptr});
}

void LibraryValuesRegistry::afterIteratorFromSource(const AfterIteratorFromSource &task) {
  CallExpr *call = task.call;
  const Type *type = call->args[0]->type;
  auto functionName = GlobalMemberTable::getQualifiedName(GlobalMember::ITERATOR_FROM);
  auto source = this->checkIteratedSource(type, functionName);
  if (source.isErr()) {
    bool isReported =
        source.error().code == LibraryValuesRegistryIteratedSourceErrorCode::ALREADY_REPORTED;
    if (!isReported) {
      this->state.diagnostics.error(call->args[0]->loc(), source.error().message);
    }
    return;
  }
  call->target = BuiltinCall{.builtin = Builtin::IterFrom};
  call->type = type->isIterator() ? type : Type::iterator(source.value());
}

void LibraryValuesRegistry::checkArrayFrom(const CheckArrayFrom &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  auto &tasks = this->state.walk.tasks;
  tasks.pushChecked(call);
  if (this->state.library.common.hasSpread(call)) {
    return tasks.pushTask(RejectSpreadCall{.call = call});
  }
  auto known = this->checkArrayStatic(member);
  if (known.isErr()) {
    this->state.diagnostics.error(member->loc(), known.error().message);

    return tasks.pushTask(WalkArgsLoosely{.call = call});
  }
  auto arity = this->state.library.common.checkArity(call, 1, 2);
  if (arity.isErr()) {
    return this->state.diagnostics.error(member->loc(), arity.error().message);
  }
  tasks.pushTask(AfterArrayFromSource{.call = call});
  tasks.pushTask(IterableOfSlot{.slot = &call->args[0]});
  tasks.pushTask(CheckExprSlot{.slot = &call->args[0], .expected = nullptr});

  return tasks.pushTask(MarkFused{.expr = call->args[0]});
}

void LibraryValuesRegistry::afterArrayFromSource(const AfterArrayFromSource &task) {
  CallExpr *call = task.call;
  auto &tasks = this->state.walk.tasks;
  const Type *type = call->args[0]->type;
  auto functionName = GlobalMemberTable::getQualifiedName(GlobalMember::ARRAY_FROM);
  auto source = this->checkIteratedSource(type, functionName);
  if (source.isErr()) {
    bool isReported =
        source.error().code == LibraryValuesRegistryIteratedSourceErrorCode::ALREADY_REPORTED;
    if (!isReported) {
      this->state.diagnostics.error(call->args[0]->loc(), source.error().message);
    }
    if (call->args.size() != 2) {
      return;
    }
    return tasks.pushTask(CheckExprSlot{.slot = &call->args[1], .expected = nullptr});
  }
  const Type *element = source.value();
  this->walkArrayFromSends(call, type);
  call->target = BuiltinCall{.builtin = Builtin::ArrayFrom};
  call->type = Type::array(element);
  if (call->args.size() != 2) {
    return tasks.pushTask(CalledFunction{});
  }
  auto signature = Signature{
      .params = {element, Type::intTy()},
      .pad = false,
  };
  tasks.pushTask(AfterArrayFromMapper{.call = call});

  return tasks.pushTask(CheckFunctionArgSlot{
      .slot = &call->args[1], .expected = signature, .primary = 1, .method = functionName});
}

void LibraryValuesRegistry::afterArrayFromMapper(const AfterArrayFromMapper &task) {
  CallExpr *call = task.call;
  const Type *callback = call->args[1]->type;
  if (!callback->isFunction()) {
    return;
  }
  auto functionName = GlobalMemberTable::getQualifiedName(GlobalMember::ARRAY_FROM);
  auto value = this->checkCallbackValue(callback->ret(), functionName);
  if (value.isErr()) {
    this->state.diagnostics.error(call->args[1]->loc(), value.error().message);
    call->type = Type::error();

    return;
  }
  call->type = Type::array(value.value());

  return this->state.walk.tasks.pushTask(CalledFunction{});
}

void LibraryValuesRegistry::checkIteratorMethod(const CheckIteratorMethod &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  auto &tasks = this->state.walk.tasks;
  tasks.pushChecked(call);
  const auto *method = IteratorMethodTable::find(member->name);
  const IteratorStepMethod *step =
      method != nullptr ? method->form.as<IteratorStepMethod>() : nullptr;
  if (step != nullptr) {
    return this->startIteratorStep(call, member, step->builtin);
  }
  auto helper = this->helperBuiltin(member);
  if (!helper.has_value()) {
    return tasks.pushTask(WalkArgsLoosely{.call = call});
  }
  switch (method->method) {
    case IteratorMethod::TAKE:
    case IteratorMethod::DROP:
      return this->startTakeDrop(call, member, *helper);
    case IteratorMethod::TO_ARRAY:
      return this->startToArray(call, member, *helper);
    case IteratorMethod::MAP:
    case IteratorMethod::FILTER:
    case IteratorMethod::FLAT_MAP:
    case IteratorMethod::FOR_EACH:
    case IteratorMethod::REDUCE:
    case IteratorMethod::SOME:
    case IteratorMethod::EVERY:
    case IteratorMethod::FIND:
      return this->startIteratorCallback(call, member, method->method, *helper);
    case IteratorMethod::NEXT:
    case IteratorMethod::RETURN:
    case IteratorMethod::THROW:
      return;
  }
}

bool LibraryValuesRegistry::hasIteratorArity(CallExpr *call, MemberExpr *member, unsigned min,
                                             unsigned max) {
  auto arity = this->state.library.common.checkArity(call, min, max);
  if (arity.isErr()) {
    this->state.diagnostics.error(member->loc(), arity.error().message);
  }

  return arity.isOk();
}

void LibraryValuesRegistry::startIteratorStep(CallExpr *call, MemberExpr *member, Builtin builtin) {
  auto &tasks = this->state.walk.tasks;
  const Type *iterator = member->object->type;
  if (!this->hasIteratorArity(call, member, 0, 1)) {
    return;
  }
  auto next = this->checkNextValue(iterator);
  if (next.isErr()) {
    this->state.diagnostics.error(member->loc(), next.error().message);

    return tasks.pushTask(WalkArgsLoosely{.call = call});
  }
  call->target = BuiltinCall{.builtin = builtin};
  call->type = this->stepResultType(next.value());
  tasks.pushTask(CalledFunction{});
  if (call->args.size() != 1) {
    return;
  }
  const Type *sent = iterator->sent();
  if (sent->isVoid()) {
    return tasks.pushTask(CheckExprSlot{.slot = &call->args[0], .expected = nullptr});
  }

  return tasks.pushTask(CheckArgSlot{.slot = &call->args[0], .to = sent});
}

std::optional<Builtin> LibraryValuesRegistry::helperBuiltin(const MemberExpr *member) {
  const Type *iterator = member->object->type;
  llvm::StringRef name = member->name;
  auto plain = this->checkPlainMethod(name);
  if (plain.isErr()) {
    this->state.diagnostics.error(member->loc(), plain.error().message);
    return std::nullopt;
  }
  auto found = this->checkIteratorHelper(name, iterator);
  if (found.isErr()) {
    this->state.diagnostics.error(member->loc(), found.error().message);
    return std::nullopt;
  }
  auto what = ErrorTextFunctionName::quoted(name);
  auto nothing = this->state.code.iteration.checkSendsNothing(iterator, what);
  if (nothing.isErr()) {
    this->state.diagnostics.error(member->loc(), nothing.error().message);
    return std::nullopt;
  }

  return found.value();
}

void LibraryValuesRegistry::startTakeDrop(CallExpr *call, MemberExpr *member, Builtin builtin) {
  auto &tasks = this->state.walk.tasks;
  if (!this->hasIteratorArity(call, member, 1, 1)) {
    return;
  }
  call->target = BuiltinCall{.builtin = builtin};
  call->type = Type::iterator(member->object->type->yielded());
  tasks.pushTask(AfterTakeCount{.call = call, .member = member});

  return tasks.pushTask(CheckPosition{.slot = &call->args[0]});
}

void LibraryValuesRegistry::afterTakeCount(const AfterTakeCount &task) {
  Expr *count = task.call->args[0];
  auto counted = ErrorTextIteratorMethod::countOf(task.member->name);
  auto between = this->state.flow.proofs.requireBetween(count, 0, std::nullopt, counted);
  bool isShown =
      between.isErr() && between.error().code != FlowProofsRequireBetweenErrorCode::EXEMPT;
  if (isShown) {
    this->state.diagnostics.error(count->loc(), between.error().message);
  }
}

void LibraryValuesRegistry::startToArray(CallExpr *call, MemberExpr *member, Builtin builtin) {
  if (!this->hasIteratorArity(call, member, 0, 0)) {
    return;
  }
  call->target = BuiltinCall{.builtin = builtin};
  call->type = Type::array(member->object->type->yielded());

  return this->state.walk.tasks.pushTask(CalledFunction{});
}

void LibraryValuesRegistry::startIteratorCallback(CallExpr *call, MemberExpr *member,
                                                  IteratorMethod method, Builtin builtin) {
  auto &tasks = this->state.walk.tasks;
  const Type *element = member->object->type->yielded();
  llvm::StringRef name = member->name;
  size_t count = call->args.size();
  bool isReduce = method == IteratorMethod::REDUCE;
  auto passed = this->checkCallbackGiven(count, name, isReduce);
  if (passed.isErr()) {
    this->state.diagnostics.error(member->loc(), passed.error().message);

    return tasks.pushTask(WalkArgsLoosely{.call = call});
  }
  auto draft = IteratorCallbackDraft{
      .call = call,
      .name = name,
      .method = method,
      .builtin = builtin,
      .element = element,
      .accumulator = element,
      .isBoolean = false,
      .count = count,
  };
  if (!isReduce || count != 2) {
    return this->startCallbackArgument(draft);
  }
  Expr *init = call->args[1];
  auto *unary = dyn_cast<UnaryExpr>(init);
  bool isNegative =
      unary != nullptr && unary->op == UnaryOp::Neg && isa<IntLitExpr>(unary->operand);
  bool isLiteral = isa<IntLitExpr>(init) || isNegative;
  const auto *apparent = this->state.generics.apparent.apparentType(element);
  const Type *hint = isLiteral && apparent->isNumeric() ? apparent : nullptr;
  tasks.pushTask(AfterReduceInitValue{.draft = draft});

  return tasks.pushTask(CheckExprSlot{.slot = &call->args[1], .expected = hint});
}

void LibraryValuesRegistry::afterReduceInitValue(const AfterReduceInitValue &task) {
  IteratorCallbackDraft draft = task.draft;
  CallExpr *call = draft.call;
  draft.accumulator = call->args[1]->type;
  auto initial = this->checkInitialValue(draft.accumulator);
  if (initial.isErr()) {
    this->state.diagnostics.error(call->args[1]->loc(), initial.error().message);

    return this->state.walk.tasks.pushTask(
        CheckExprSlot{.slot = &call->args[0], .expected = nullptr});
  }

  return this->startCallbackArgument(draft);
}

void LibraryValuesRegistry::startCallbackArgument(IteratorCallbackDraft draft) {
  CallExpr *call = draft.call;
  auto &tasks = this->state.walk.tasks;
  auto signature = this->callbackSignature(draft.method, draft.element, draft.accumulator);
  draft.isBoolean = this->state.isGlobalName(call->args[0], GlobalName::BOOLEAN);
  unsigned primary = draft.method == IteratorMethod::REDUCE ? 2 : 1;
  auto *given = dyn_cast<FunctionExpr>(call->args[0]);
  if (given != nullptr) {
    this->state.flow.callbacks.rememberIndexCallback(given->fn, primary);
  }
  tasks.pushTask(AfterIteratorCallback{.draft = draft});

  return tasks.pushTask(CheckFunctionArgSlot{
      .slot = &call->args[0], .expected = signature, .primary = primary, .method = draft.name});
}

void LibraryValuesRegistry::afterIteratorCallback(const AfterIteratorCallback &task) {
  const IteratorCallbackDraft &draft = task.draft;
  CallExpr *call = draft.call;
  auto &tasks = this->state.walk.tasks;
  auto *checked = dyn_cast<FunctionExpr>(call->args[0]);
  if (BuiltinGroups::isIteratorStage(draft.builtin) && checked != nullptr) {
    checked->fn->nonEscaping = call->fused;
  }
  call->target = BuiltinCall{.builtin = draft.builtin};
  bool isFunction = call->args[0]->type->isFunction();
  if (isFunction) {
    this->finishCallbackResult(draft);
  }
  if (BuiltinGroups::isIteratorSink(draft.builtin)) {
    return tasks.pushTask(CalledFunction{});
  }
}

void LibraryValuesRegistry::finishCallbackResult(const IteratorCallbackDraft &draft) {
  CallExpr *call = draft.call;
  auto &tasks = this->state.walk.tasks;
  const Type *result = call->args[0]->type->ret();
  switch (draft.method) {
    case IteratorMethod::MAP:
      return this->finishMapResult(draft, result);
    case IteratorMethod::FLAT_MAP:
      return this->finishFlatMapResult(draft, result);
    case IteratorMethod::FILTER:
      return tasks.pushTask(FinishIteratorFilter{
          .call = call, .element = draft.element, .isBoolean = draft.isBoolean});
    case IteratorMethod::FOR_EACH:
      call->type = Type::voidTy();
      return;
    case IteratorMethod::REDUCE:
      call->type = draft.count == 2 ? draft.accumulator : Type::optional(draft.accumulator);
      if (draft.count == 2 && call->args.size() == 2) {
        tasks.pushTask(CoerceSlot{.slot = &call->args[1], .to = draft.accumulator, .what = ""});
      }
      return;
    case IteratorMethod::SOME:
    case IteratorMethod::EVERY:
      call->type = Type::boolTy();
      return;
    case IteratorMethod::FIND:
      call->type = Type::optional(draft.element);
      return;
    case IteratorMethod::NEXT:
    case IteratorMethod::RETURN:
    case IteratorMethod::THROW:
    case IteratorMethod::TAKE:
    case IteratorMethod::DROP:
    case IteratorMethod::TO_ARRAY:
      return;
  }
}

void LibraryValuesRegistry::finishMapResult(const IteratorCallbackDraft &draft,
                                            const Type *result) {
  CallExpr *call = draft.call;
  auto value = this->checkCallbackValue(result, draft.name);
  if (value.isErr()) {
    return this->state.diagnostics.error(call->args[0]->loc(), value.error().message);
  }
  call->type = Type::iterator(result);
}

void LibraryValuesRegistry::finishFlatMapResult(const IteratorCallbackDraft &draft,
                                                const Type *result) {
  CallExpr *call = draft.call;
  auto inner = this->checkFlatMapInner(result);
  if (inner.isErr()) {
    return this->state.diagnostics.error(call->args[0]->loc(), inner.error().message);
  }
  if (result->isIterator()) {
    auto quoted = ErrorTextFunctionName::quoted(draft.name);
    auto flatSends = this->state.code.iteration.checkSendsNothing(result, quoted);
    if (flatSends.isErr()) {
      return this->state.diagnostics.error(call->args[0]->loc(), flatSends.error().message);
    }
  }
  call->type = Type::iterator(inner.value());
}

const Type *LibraryValuesRegistry::resultType(StringResult result) const {
  switch (result) {
    case StringResult::INT:
      return Type::intTy();
    case StringResult::BOOL:
      return Type::boolTy();
    case StringResult::STRING:
      return Type::string();
    case StringResult::STRINGS:
      return Type::array(Type::string());
  }

  return Type::error();
}

LibraryValuesRegistry::ResultStringMethod
LibraryValuesRegistry::checkMethodName(llvm::StringRef name) const {
  const auto *method = StringMethodTable::find(name);
  bool isCallable = method != nullptr && StringMethodTable::findCall(*method) != nullptr;
  if (!isCallable) {
    auto message = ErrorTextStringMethod::missing(name);

    return err(LibraryValuesRegistryStringMethodError{
        .code = LibraryValuesRegistryStringMethodErrorCode::NO_SUCH_METHOD,
        .message = message,
    });
  }

  return ok(method);
}

void LibraryValuesRegistry::walkMethodRange(CallExpr *call, const MemberExpr *member,
                                            StringMethod method) {
  bool isCodeAt = method == StringMethod::CHAR_CODE_AT || method == StringMethod::CODE_POINT_AT;
  if (method == StringMethod::REPEAT) {
    this->walkRepeatCount(call);
    return;
  }
  if (isCodeAt) {
    call->mayBeOutOfRange =
        !this->state.flow.proofs.canProveInBounds(member->object, call->args[0]);
    call->type = call->mayBeOutOfRange ? Type::optional(Type::intTy()) : Type::intTy();
    return;
  }
  if (method == StringMethod::AT) {
    call->mayBeOutOfRange =
        !this->state.flow.proofs.canProveAtInBounds(member->object, call->args[0]);
    call->type = call->mayBeOutOfRange ? Type::optional(Type::string()) : Type::string();
  }
}

void LibraryValuesRegistry::walkRepeatCount(const CallExpr *call) {
  auto counted = ErrorTextStringMethod::repeatOf();
  auto between = this->state.flow.proofs.requireBetween(call->args[0], 0, std::nullopt, counted);
  if (between.isErr() && between.error().code != FlowProofsRequireBetweenErrorCode::EXEMPT) {
    this->state.diagnostics.error(call->args[0]->loc(), between.error().message);
  }
}

void LibraryValuesRegistry::walkCodePoint(const Expr *arg) {
  auto what = ErrorTextNumberMethod::codePoint();
  auto between = this->state.flow.proofs.requireBetween(arg, 0, kMaxCodePoint, what);
  if (between.isErr() && between.error().code != FlowProofsRequireBetweenErrorCode::EXEMPT) {
    this->state.diagnostics.error(arg->loc(), between.error().message);
  }
}

LibraryValuesRegistry::ResultConversionMade
LibraryValuesRegistry::checkConversion(CallExpr *call, MemberExpr *member) {
  const auto *to = this->conversionTarget(member->name);
  if (to == nullptr) {
    return err(LibraryValuesRegistryCheckConversionError{
        .code = LibraryValuesRegistryCheckConversionErrorCode::NOT_A_CONVERSION,
        .message = "",
    });
  }
  call->type = Type::error();
  auto arity = this->state.library.common.checkArity(call, 0, 0);
  if (arity.isErr()) {
    return err(LibraryValuesRegistryCheckConversionError{
        .code = LibraryValuesRegistryCheckConversionErrorCode::WRONG_ARITY,
        .message = arity.error().message,
    });
  }
  auto allowed = this->checkConversionAllowed(member, to);
  if (allowed.isErr()) {
    return err(allowed.error());
  }

  call->target = BuiltinCall{.builtin = Builtin::NumConvert};
  call->type = to;

  return ok(call);
}

const Type *LibraryValuesRegistry::conversionTarget(llvm::StringRef name) const {
  const auto *method = NumberMethodTable::find(name);
  if (method == nullptr || method->convertsTo.empty()) {
    return nullptr;
  }

  return Type::fromName(method->convertsTo);
}

std::string LibraryValuesRegistry::shownNumber(const Expr *object) const {
  auto *name = dyn_cast<NameExpr>(object);
  if (name == nullptr) {
    return kUnnamedNumber;
  }

  return name->name.str();
}

LibraryValuesRegistry::ResultConversionMade
LibraryValuesRegistry::checkConversionAllowed(const MemberExpr *member, const Type *to) const {
  const Type *from = member->object->type;
  auto text = this->shownNumber(member->object);
  if (from->isFloat() && to->isDecimal()) {
    auto message = ErrorTextConversion::toDecimal(text);

    return err(LibraryValuesRegistryCheckConversionError{
        .code = LibraryValuesRegistryCheckConversionErrorCode::DOUBLE_TO_DECIMAL,
        .message = message,
    });
  }
  auto *rounding = dyn_cast<CallExpr>(member->object);
  bool isRounded = rounding != nullptr && BuiltinGroups::isRoundingBuiltin(rounding->builtin());
  if (from->isFloat() && to->isInteger() && !isRounded) {
    auto message = ErrorTextConversion::toInteger(text, member->name);

    return err(LibraryValuesRegistryCheckConversionError{
        .code = LibraryValuesRegistryCheckConversionErrorCode::DOUBLE_TO_INTEGER,
        .message = message,
    });
  }

  return ok(member->object);
}

const VarSymbol *LibraryValuesRegistry::arrayVariable(const Expr *object) const {
  auto *conversion = dyn_cast<CastExpr>(object);
  bool isOptionalWrap =
      conversion != nullptr && conversion->isImplicit() && conversion->operand->type->isOptional();
  const Expr *inner = isOptionalWrap ? conversion->operand : object;
  auto *name = dyn_cast<NameExpr>(inner);
  if (name == nullptr || name->var() == nullptr || !name->var()->type->nonOptional()->isArray()) {
    return nullptr;
  }

  return name->var();
}

LibraryValuesRegistry::ResultElementsAreText
LibraryValuesRegistry::checkTextElements(const Type *element, llvm::StringRef name) const {
  if (element->holdsFunctions()) {
    auto message = ErrorTextArrayText::functions();

    return err(LibraryValuesRegistryTextElementsError{
        .code = LibraryValuesRegistryTextElementsErrorCode::FUNCTIONS,
        .message = message,
    });
  }
  if (element->holdsResults()) {
    auto message = ErrorTextArrayText::results(name);

    return err(LibraryValuesRegistryTextElementsError{
        .code = LibraryValuesRegistryTextElementsErrorCode::RESULTS,
        .message = message,
    });
  }
  if (this->isHoldingUnknown(element)) {
    auto message = ErrorTextArrayText::unknown(name);

    return err(LibraryValuesRegistryTextElementsError{
        .code = LibraryValuesRegistryTextElementsErrorCode::UNKNOWN,
        .message = message,
    });
  }
  if (this->isHoldingJson(element)) {
    auto message = ErrorTextArrayText::json(name);

    return err(LibraryValuesRegistryTextElementsError{
        .code = LibraryValuesRegistryTextElementsErrorCode::JSON,
        .message = message,
    });
  }
  if (element->holdsObjects() && !element->holdsDates()) {
    auto message = ErrorTextArrayText::objects(name);

    return err(LibraryValuesRegistryTextElementsError{
        .code = LibraryValuesRegistryTextElementsErrorCode::OBJECTS,
        .message = message,
    });
  }

  return ok(element);
}

LibraryValuesRegistry::ResultSortableElement
LibraryValuesRegistry::checkSortable(const Type *element) const {
  auto problem = this->sortProblem(element);
  if (!problem.has_value()) {
    return ok(element);
  }
  auto message = this->sortMessage(*problem, element);

  return err(LibraryValuesRegistrySortableError{
      .code = *problem,
      .message = message,
  });
}

std::optional<LibraryValuesRegistrySortableErrorCode>
LibraryValuesRegistry::sortProblem(const Type *element) const {
  if (element->holdsFunctions()) {
    return LibraryValuesRegistrySortableErrorCode::FUNCTIONS;
  }
  if (this->isHoldingUnknown(element)) {
    return LibraryValuesRegistrySortableErrorCode::UNKNOWN;
  }
  if (this->isHoldingJson(element)) {
    return LibraryValuesRegistrySortableErrorCode::JSON;
  }
  if (element->holdsDates()) {
    return LibraryValuesRegistrySortableErrorCode::DATES;
  }
  if (element->holdsObjects() || element->holdsResults()) {
    return LibraryValuesRegistrySortableErrorCode::OBJECTS;
  }
  const Type *base = element->nonOptional();
  if (base->isUnion() && !this->areAllTextual(base->members())) {
    return LibraryValuesRegistrySortableErrorCode::MIXED_UNION;
  }
  bool hasNumbers = base->isNumeric() || base->isDecimal() ||
                    this->state.generics.apparent.apparentType(base)->isNumeric();
  if (hasNumbers) {
    return LibraryValuesRegistrySortableErrorCode::NUMBERS;
  }

  return std::nullopt;
}

bool LibraryValuesRegistry::areAllTextual(llvm::ArrayRef<const Type *> members) const {
  for (const Type *member : members) {
    const Type *memberBase = member->isLiteral() ? member->literalBase() : member;
    if (!memberBase->isString() && !memberBase->isBool()) {
      return false;
    }
  }

  return true;
}

std::string LibraryValuesRegistry::sortMessage(LibraryValuesRegistrySortableErrorCode code,
                                               const Type *element) const {
  switch (code) {
    case LibraryValuesRegistrySortableErrorCode::FUNCTIONS:
      return ErrorTextArraySort::functions();
    case LibraryValuesRegistrySortableErrorCode::UNKNOWN:
      return ErrorTextArraySort::unknown();
    case LibraryValuesRegistrySortableErrorCode::JSON:
      return ErrorTextArraySort::json();
    case LibraryValuesRegistrySortableErrorCode::DATES:
      return ErrorTextArraySort::dates();
    case LibraryValuesRegistrySortableErrorCode::OBJECTS:
      return ErrorTextArraySort::objects();
    case LibraryValuesRegistrySortableErrorCode::MIXED_UNION:
      return ErrorTextArraySort::mixed(element->nonOptional()->name());
    case LibraryValuesRegistrySortableErrorCode::NUMBERS:
      return ErrorTextArraySort::numbers(element->isOptional());
  }

  return ErrorTextArraySort::objects();
}

void LibraryValuesRegistry::shrinkLength(const VarSymbol *self, bool isNotEmpty) {
  auto &facts = this->state.walk.flow.getFacts();
  if (isNotEmpty) {
    facts.shift(Term::lengthOf(self), -1);
    return;
  }
  auto isLengthFact = std::bind_front(&LibraryValuesRegistry::isLengthFact, this, self);
  facts.forgetIf(isLengthFact);
}

bool LibraryValuesRegistry::isLengthFact(const VarSymbol *self, const Facts::Fact &fact) const {
  return fact.rhs == Term::lengthOf(self);
}

LibraryValuesRegistry::ResultDictObjectCalled
LibraryValuesRegistry::checkDictObjectCall(CallExpr *call, llvm::StringRef functionName) {
  Expr *arg = call->args[0];
  const Type *type = arg->type;
  auto objectMember = GlobalMemberTable::find(GlobalName::OBJECT, functionName);
  if (!type->isRecord()) {
    auto text = DictCheckHelpers::dictText(arg, type);
    auto collection = type->isMap() ? GlobalName::MAP : GlobalName::SET;
    auto kind = GlobalNameTable::getName(collection);
    bool isSetEntries = objectMember == GlobalMember::OBJECT_ENTRIES && type->isSet();
    llvm::StringRef method =
        isSetEntries ? DictMethodTable::getName(DictMethod::VALUES) : functionName;
    auto message = ErrorTextDictObject::empty(functionName, kind, text, method);

    return err(LibraryValuesRegistryCheckDictObjectCallError{
        .code = LibraryValuesRegistryCheckDictObjectCallErrorCode::ALWAYS_EMPTY,
        .message = message,
    });
  }
  call->target = BuiltinCall{.builtin = this->objectCallBuiltin(objectMember)};
  call->type = this->objectCallType(objectMember, type->valueType());

  return ok(call);
}

Builtin LibraryValuesRegistry::objectCallBuiltin(std::optional<GlobalMember> member) const {
  if (member == GlobalMember::OBJECT_KEYS) {
    return Builtin::ObjectKeys;
  }
  if (member == GlobalMember::OBJECT_VALUES) {
    return Builtin::ObjectValues;
  }

  return Builtin::ObjectEntries;
}

const Type *LibraryValuesRegistry::objectCallType(std::optional<GlobalMember> member,
                                                  const Type *value) const {
  if (member == GlobalMember::OBJECT_KEYS) {
    return Type::array(Type::string());
  }
  if (member == GlobalMember::OBJECT_VALUES) {
    return Type::array(value);
  }
  const auto *entry = Type::tuple({Type::string(), value});

  return Type::array(entry);
}

Expr *LibraryValuesRegistry::markStageFused(CallExpr *call) const {
  auto *member = dyn_cast<MemberExpr>(call->callee);
  if (member == nullptr || member->optional || call->optional) {
    return nullptr;
  }
  bool isIteratorFrom =
      this->state.isGlobalMember(member, GlobalMember::ITERATOR_FROM) && call->args.size() == 1;
  if (isIteratorFrom) {
    return call->args[0];
  }
  const auto *stage = IteratorMethodTable::find(member->name);
  if (stage == nullptr || !stage->form.is<IteratorStageMethod>()) {
    return nullptr;
  }
  call->fused = true;

  return member->object;
}

Builtin LibraryValuesRegistry::arrayIteratorBuiltin(const ArrayMethodInfo *method) const {
  if (method != nullptr && method->method == ArrayMethod::KEYS) {
    return Builtin::ArrKeys;
  }
  if (method != nullptr && method->method == ArrayMethod::VALUES) {
    return Builtin::ArrValues;
  }

  return Builtin::ArrEntries;
}

const Type *LibraryValuesRegistry::arrayIteratorType(const ArrayMethodInfo *method,
                                                     const Type *element) const {
  if (method != nullptr && method->method == ArrayMethod::KEYS) {
    return Type::iterator(Type::intTy());
  }
  if (method != nullptr && method->method == ArrayMethod::VALUES) {
    return Type::iterator(element);
  }
  const auto *entry = Type::tuple({Type::intTy(), element});

  return Type::iterator(entry);
}

LibraryValuesRegistry::ResultSourceIsIterable
LibraryValuesRegistry::checkIteratedSource(const Type *type, llvm::StringRef functionName) const {
  const auto *element = this->state.code.iteration.iteratedType(type);
  if (element == nullptr && type->isError()) {
    return err(LibraryValuesRegistryIteratedSourceError{
        .code = LibraryValuesRegistryIteratedSourceErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  if (element == nullptr) {
    auto message = ErrorTextIterSource::notIter(functionName, type->name());

    return err(LibraryValuesRegistryIteratedSourceError{
        .code = LibraryValuesRegistryIteratedSourceErrorCode::NOT_ITERABLE,
        .message = message,
    });
  }

  return ok(element);
}

LibraryValuesRegistry::ResultArrayStaticKnown
LibraryValuesRegistry::checkArrayStatic(const MemberExpr *member) const {
  auto found = GlobalMemberTable::find(GlobalName::ARRAY, member->name);
  if (found != GlobalMember::ARRAY_FROM) {
    auto message = ErrorTextStaticFrom::array(member->name);

    return err(LibraryValuesRegistryArrayStaticError{
        .code = LibraryValuesRegistryArrayStaticErrorCode::NOT_SUPPORTED,
        .message = message,
    });
  }

  return ok(member);
}

LibraryValuesRegistry::ResultCallbackHasValue
LibraryValuesRegistry::checkCallbackValue(const Type *result, llvm::StringRef functionName) const {
  if (result->isVoid() || result->isUndefined()) {
    auto message = ErrorTextCallbackValue::missing(functionName);

    return err(LibraryValuesRegistryCallbackValueError{
        .code = LibraryValuesRegistryCallbackValueErrorCode::NO_VALUE,
        .message = message,
    });
  }

  return ok(result);
}

void LibraryValuesRegistry::walkArrayFromSends(const CallExpr *call, const Type *type) {
  if (!type->isIterator()) {
    return;
  }
  auto functionName = GlobalMemberTable::getQualifiedName(GlobalMember::ARRAY_FROM);
  auto quoted = ErrorTextFunctionName::quoted(functionName);
  auto nothing = this->state.code.iteration.checkSendsNothing(type, quoted);
  if (nothing.isErr()) {
    this->state.diagnostics.error(call->args[0]->loc(), nothing.error().message);
  }
}

LibraryValuesRegistry::ResultNextValueOneType
LibraryValuesRegistry::checkNextValue(const Type *iterator) const {
  const Type *element = iterator->yielded();
  const Type *returned = iterator->returned();
  const Type *value = returned->isVoid() ? element : Type::unionOf({element, returned});
  auto problem = UnionSlots::unionProblem(value);
  if (!problem.empty()) {
    auto message =
        ErrorTextNextValue::noUnion(iterator->name(), element->name(), returned->name(), problem);

    return err(LibraryValuesRegistryNextValueError{
        .code = LibraryValuesRegistryNextValueErrorCode::UNION_PROBLEM,
        .message = message,
    });
  }

  return ok(value);
}

LibraryValuesRegistry::ResultNotThrowOrReturn
LibraryValuesRegistry::checkPlainMethod(llvm::StringRef name) const {
  const auto *method = IteratorMethodTable::find(name);
  if (method != nullptr && method->method == IteratorMethod::THROW) {
    auto message = ErrorTextIteratorMethod::noThrow();

    return err(LibraryValuesRegistryPlainMethodError{
        .code = LibraryValuesRegistryPlainMethodErrorCode::NO_THROW,
        .message = message,
    });
  }
  if (method != nullptr && method->method == IteratorMethod::RETURN) {
    auto message = ErrorTextIteratorMethod::noReturn();

    return err(LibraryValuesRegistryPlainMethodError{
        .code = LibraryValuesRegistryPlainMethodErrorCode::NO_RETURN,
        .message = message,
    });
  }

  return ok(name);
}

LibraryValuesRegistry::ResultKnownHelperFound
LibraryValuesRegistry::checkIteratorHelper(llvm::StringRef name, const Type *iterator) const {
  const auto *helper = IteratorMethodTable::find(name);
  const auto *stage = helper != nullptr ? helper->form.as<IteratorStageMethod>() : nullptr;
  const auto *consumer = helper != nullptr ? helper->form.as<IteratorConsumerMethod>() : nullptr;
  if (stage == nullptr && consumer == nullptr) {
    auto message = ErrorTextIteratorMethod::noProperty(name, iterator->name());

    return err(LibraryValuesRegistryIteratorHelperError{
        .code = LibraryValuesRegistryIteratorHelperErrorCode::NO_SUCH_PROPERTY,
        .message = message,
    });
  }
  Builtin builtin = stage != nullptr ? stage->builtin : consumer->builtin;

  return ok(builtin);
}

LibraryValuesRegistry::ResultCallbackIsPassed
LibraryValuesRegistry::checkCallbackGiven(size_t count, llvm::StringRef name, bool isReduce) const {
  if (count == 0) {
    auto message = ErrorTextIteratorMethod::noFunction(name);

    return err(LibraryValuesRegistryCallbackGivenError{
        .code = LibraryValuesRegistryCallbackGivenErrorCode::NO_FUNCTION,
        .message = message,
    });
  }
  unsigned most = isReduce ? 2u : 1u;
  if (count > most) {
    auto message = ErrorTextArity::count(1, most, count);

    return err(LibraryValuesRegistryCallbackGivenError{
        .code = LibraryValuesRegistryCallbackGivenErrorCode::TOO_MANY_ARGUMENTS,
        .message = message,
    });
  }

  return ok(count);
}

LibraryValuesRegistry::ResultInitialValueType
LibraryValuesRegistry::checkInitialValue(const Type *accumulator) const {
  if (accumulator->isUndefined() || accumulator->isVoid()) {
    auto message = ErrorTextReduceInit::untyped();

    return err(LibraryValuesRegistryInitialValueError{
        .code = LibraryValuesRegistryInitialValueErrorCode::UNTYPED_INITIAL,
        .message = message,
    });
  }

  return ok(accumulator);
}

LibraryValuesRegistry::ResultFlatMapInnerType
LibraryValuesRegistry::checkFlatMapInner(const Type *result) const {
  const Type *inner =
      result->isString() ? nullptr : this->state.code.iteration.iteratedType(result);
  if (inner == nullptr) {
    auto message = ErrorTextFlatMap::notIter(result->name());

    return err(LibraryValuesRegistryFlatMapInnerError{
        .code = LibraryValuesRegistryFlatMapInnerErrorCode::NOT_ITERABLE,
        .message = message,
    });
  }

  return ok(inner);
}

const Type *LibraryValuesRegistry::stepResultType(const Type *value) const {
  auto resultName = TypeNameTable::getName(TypeName::ITERATOR_RESULT);
  auto name = resultName.str() + "<" + value->name().str() + ">";

  return Type::object(
      {{IteratorResultFieldTable::getName(IteratorResultField::VALUE).str(), Type::optional(value),
        false},
       {IteratorResultFieldTable::getName(IteratorResultField::DONE).str(), Type::boolTy(), false}},
      name);
}

Signature LibraryValuesRegistry::callbackSignature(IteratorMethod method, const Type *element,
                                                   const Type *accumulator) const {
  bool isReduce = method == IteratorMethod::REDUCE;
  auto params = isReduce ? std::vector<const Type *>{accumulator, element, Type::intTy()}
                         : std::vector<const Type *>{element, Type::intTy()};

  return Signature{
      .params = params,
      .ret = this->callbackReturn(method, accumulator),
      .voidResult = method == IteratorMethod::FOR_EACH,
      .pad = false,
  };
}

const Type *LibraryValuesRegistry::callbackReturn(IteratorMethod method,
                                                  const Type *accumulator) const {
  switch (method) {
    case IteratorMethod::FILTER:
    case IteratorMethod::SOME:
    case IteratorMethod::EVERY:
    case IteratorMethod::FIND:
      return Type::boolTy();
    case IteratorMethod::REDUCE:
      return accumulator;
    case IteratorMethod::NEXT:
    case IteratorMethod::RETURN:
    case IteratorMethod::THROW:
    case IteratorMethod::TAKE:
    case IteratorMethod::DROP:
    case IteratorMethod::TO_ARRAY:
    case IteratorMethod::MAP:
    case IteratorMethod::FLAT_MAP:
    case IteratorMethod::FOR_EACH:
      return nullptr;
  }

  return nullptr;
}

bool LibraryValuesRegistry::isGlobalFunction(const NameExpr *name) const {
  for (GlobalName global : kGlobalFunctions) {
    if (this->state.isGlobalName(name, global)) {
      return true;
    }
  }

  return false;
}

CallExpr *LibraryValuesRegistry::soleCallArg(const CallExpr *call) const {
  if (call->args.size() != 1) {
    return nullptr;
  }

  return dyn_cast<CallExpr>(call->args[0]);
}

LibraryValuesRegistry::ResultFunctionCalled
LibraryValuesRegistry::checkNotLegacyEscape(CallExpr *call, std::optional<GlobalName> global) {
  const auto *encoded = this->soleCallArg(call);
  bool isEncodedInner = encoded != nullptr &&
                        this->state.isGlobalName(encoded->callee, GlobalName::ENCODE_URI_COMPONENT);
  if (global == GlobalName::UNESCAPE && isEncodedInner) {
    auto message = ErrorTextGlobalFunction::toBytes();

    return err(LibraryValuesRegistryCheckGlobalFunctionError{
        .code = LibraryValuesRegistryCheckGlobalFunctionErrorCode::UNESCAPE_OF_ENCODE,
        .message = message,
    });
  }
  if (global == GlobalName::ESCAPE) {
    auto message = ErrorTextGlobalFunction::escape();

    return err(LibraryValuesRegistryCheckGlobalFunctionError{
        .code = LibraryValuesRegistryCheckGlobalFunctionErrorCode::LEGACY_ESCAPE,
        .message = message,
    });
  }
  if (global == GlobalName::UNESCAPE) {
    auto message = ErrorTextGlobalFunction::unescape();

    return err(LibraryValuesRegistryCheckGlobalFunctionError{
        .code = LibraryValuesRegistryCheckGlobalFunctionErrorCode::LEGACY_UNESCAPE,
        .message = message,
    });
  }

  return ok(call);
}

std::optional<Builtin> LibraryValuesRegistry::uriBuiltin(std::optional<GlobalName> global) const {
  for (const auto &[uri, builtin] : kUriBuiltins) {
    if (global == uri) {
      return builtin;
    }
  }

  return std::nullopt;
}

LibraryValuesRegistry::ResultNumberSource
LibraryValuesRegistry::checkNumberSource(const Type *type) const {
  const Type *base = type->nonOptional();
  if (type->isOptional() || type->isUndefined()) {
    auto message = ErrorTextGlobalFunction::undefined();

    return err(LibraryValuesRegistryCheckGlobalFunctionError{
        .code = LibraryValuesRegistryCheckGlobalFunctionErrorCode::NUMBER_OF_UNDEFINED,
        .message = message,
    });
  }
  bool isConvertible = base->isNumeric() || base->isBool() || base->isString() || base->isDecimal();
  if (!isConvertible) {
    auto message = ErrorTextGlobalFunction::wrongType(type->name());

    return err(LibraryValuesRegistryCheckGlobalFunctionError{
        .code = LibraryValuesRegistryCheckGlobalFunctionErrorCode::NUMBER_OF_WRONG_TYPE,
        .message = message,
    });
  }

  return ok(type);
}

}
