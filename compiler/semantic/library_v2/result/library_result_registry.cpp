#include "compiler/semantic/library_v2/result/library_result_registry.h"
#include "compiler/program/library/builtin_names/builtin_property_table/builtin_property_table.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/program/library/builtin_names/type_name_table/type_name_table.h"
#include "compiler/program/library/result/result_method_table/result_method_table.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

constexpr llvm::StringLiteral kInternalPrefix = "%";
constexpr llvm::StringLiteral kResultPlaceholder = "r";

}

LibraryResultRegistry::LibraryResultRegistry(SemanticState &owner)
    : state(owner) {}

void LibraryResultRegistry::checkResultCall(const CheckResultCall &task) {
  auto which = this->state.library.result.resultName(task.name->name);
  if (which == GlobalName::OK) {
    return this->checkOkErr(task.call, task.name, ResultHalf::OK, task.expected);
  }
  if (which == GlobalName::ERR) {
    return this->checkOkErr(task.call, task.name, ResultHalf::ERR, task.expected);
  }

  return this->state.walk.tasks.pushChecked(nullptr);
}

void LibraryResultRegistry::checkOkErr(CallExpr *call, NameExpr *name, ResultHalf half,
                                       const Type *expected) {
  auto &tasks = this->state.walk.tasks;
  tasks.pushChecked(call);
  if (this->state.library.common.hasSpread(call)) {
    return tasks.pushTask(RejectSpreadCall{.call = call});
  }
  auto arity = this->state.library.common.checkArity(call, 0, 1);
  if (arity.isErr()) {
    return this->state.diagnostics.error(name->loc(), arity.error().message);
  }
  if (name->typeArgs.empty()) {
    return this->startHalfValue(call, name, half, this->expectedResult(expected));
  }
  auto pair = this->checkOkErrTypeArgs(name);
  if (pair.isErr()) {
    this->state.diagnostics.error(name->loc(), pair.error().message);

    return tasks.pushTask(MarkCallFailed{.call = call});
  }
  tasks.pushTask(AfterWrittenResult{.call = call, .name = name, .half = half});
  tasks.pushTask(ResolveTypeAnswer{.ref = &name->typeArgs[1], .use = TypeUse::STANDALONE_OR_VOID});

  return tasks.pushTask(
      ResolveTypeAnswer{.ref = &name->typeArgs[0], .use = TypeUse::STANDALONE_OR_VOID});
}

void LibraryResultRegistry::afterWrittenResult(const AfterWrittenResult &task) {
  auto halves = this->state.walk.tasks.takeAnswers(2);
  const Type *value = halves[0];
  const Type *error = halves[1];
  if (value->isError() || error->isError()) {
    return this->state.walk.tasks.pushTask(MarkCallFailed{.call = task.call});
  }
  const auto *written = Type::result(value, error);

  return this->startHalfValue(task.call, task.name, task.half, written);
}

void LibraryResultRegistry::startHalfValue(CallExpr *call, NameExpr *name, ResultHalf half,
                                           const Type *wanted) {
  auto &tasks = this->state.walk.tasks;
  if (call->args.empty()) {
    auto finish =
        FinishOkErr{.call = call, .half = half, .wanted = wanted, .given = Type::voidTy()};

    return this->finishOkErr(finish);
  }
  const Type *wantedHalf = wanted != nullptr ? this->halfType(wanted, half) : nullptr;
  bool isHinted = wantedHalf != nullptr && this->isGivingType(wantedHalf);
  tasks.pushTask(AfterHalfValue{.call = call, .name = name, .half = half, .wanted = wanted});

  return tasks.pushTask(
      CheckExprSlot{.slot = &call->args[0], .expected = isHinted ? wantedHalf : nullptr});
}

void LibraryResultRegistry::afterHalfValue(const AfterHalfValue &task) {
  CallExpr *call = task.call;
  ResultHalf half = task.half;
  const Type *wanted = task.wanted;
  auto &tasks = this->state.walk.tasks;
  const Expr *arg = call->args[0];
  const Type *given = arg->type;
  const Type *wantedHalf = wanted != nullptr ? this->halfType(wanted, half) : nullptr;
  bool isHinted = wantedHalf != nullptr && this->isGivingType(wantedHalf);
  if (given->isVoid()) {
    auto message = ErrorTextOkErr::voidValue(task.name->name);
    this->state.diagnostics.error(arg->loc(), message);
    auto failed = FinishOkErr{.call = call, .half = half, .wanted = wanted, .given = Type::error()};

    return this->finishOkErr(failed);
  }
  if (isHinted && this->state.types.relations.isImplicitlyConvertible(given, wantedHalf)) {
    tasks.pushTask(FinishOkErr{.call = call, .half = half, .wanted = wanted, .given = wantedHalf});

    return tasks.pushTask(ConvertSlot{.slot = &call->args[0], .to = wantedHalf});
  }
  if (task.name->typeArgs.size() == 2 && !given->isError()) {
    tasks.pushTask(FinishOkErr{.call = call, .half = half, .wanted = wanted, .given = wantedHalf});

    return tasks.pushTask(CoerceSlot{.slot = &call->args[0], .to = wantedHalf, .what = ""});
  }
  auto plain = FinishOkErr{.call = call, .half = half, .wanted = wanted, .given = given};

  return this->finishOkErr(plain);
}

void LibraryResultRegistry::finishOkErr(const FinishOkErr &task) {
  CallExpr *call = task.call;
  const Type *wanted = task.wanted;
  const Type *other = wanted != nullptr ? this->otherHalfType(wanted, task.half) : Type::never();
  if (task.half == ResultHalf::OK) {
    call->target = BuiltinCall{.builtin = Builtin::ResultOk};
    call->type = Type::result(task.given, other);

    return;
  }
  call->target = BuiltinCall{.builtin = Builtin::ResultErr};
  call->type = Type::result(other, task.given);
}

void LibraryResultRegistry::checkResultStatic(const CheckResultStatic &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  auto &tasks = this->state.walk.tasks;
  auto *name = dyn_cast<NameExpr>(member->object);
  bool isPlainCall = name != nullptr && !call->optional && !member->optional;
  auto which = isPlainCall ? this->state.library.result.resultName(name->name) : std::nullopt;
  if (which != GlobalName::RESULT) {
    return tasks.pushChecked(nullptr);
  }
  tasks.pushChecked(call);
  auto known = this->checkStaticMethod(name, member->name);
  if (known.isErr()) {
    this->state.diagnostics.error(member->loc(), known.error().message);

    return tasks.pushTask(MarkCallFailed{.call = call});
  }
  if (this->state.library.common.hasSpread(call)) {
    return tasks.pushTask(RejectSpreadCall{.call = call});
  }
  auto arity = this->state.library.common.checkArity(call, 1, 1);
  if (arity.isErr()) {
    return this->state.diagnostics.error(member->loc(), arity.error().message);
  }
  bool isAll = known.value() == ResultStatic::COMBINE_WITH_ALL_ERRORS;
  Builtin builtin = isAll ? Builtin::ResultCombineWithAllErrors : Builtin::ResultCombine;
  call->target = BuiltinCall{.builtin = builtin};
  auto combine = ResultCombineDraft{
      .call = call,
      .what = name->name.str() + "." + member->name.str(),
      .isAll = isAll,
  };
  auto *literal = dyn_cast<ArrayLitExpr>(call->args[0]);
  if (literal == nullptr || !this->isPlainList(literal)) {
    tasks.pushTask(AfterCombineArray{.combine = std::move(combine)});

    return tasks.pushTask(CheckExprSlot{.slot = &call->args[0], .expected = nullptr});
  }
  tasks.pushTask(AfterCombineList{.combine = std::move(combine), .literal = literal});
  for (ArrayLitExpr::Element &element : llvm::reverse(literal->elements)) {
    tasks.pushTask(CheckExprSlot{.slot = &element.expr, .expected = nullptr});
  }
}

void LibraryResultRegistry::afterCombineList(const AfterCombineList &task) {
  const ResultCombineDraft &combine = task.combine;
  ArrayLitExpr *literal = task.literal;
  auto results = std::vector<const Type *>();
  for (const ArrayLitExpr::Element &element : literal->elements) {
    results.push_back(element.expr->type);
  }
  for (const Type *type : results) {
    if (type->isError()) {
      return;
    }
  }
  for (size_t index = 0; index < results.size(); ++index) {
    if (!results[index]->isResult()) {
      auto message = ErrorTextCombine::notResult(combine.what, results[index]->name());

      return this->state.diagnostics.error(literal->elements[index].expr->loc(), message);
    }
  }
  const auto *common = this->commonResult(results);
  if (common != nullptr && !common->isError()) {
    return this->checkSameCombine(combine, literal, common);
  }

  return this->checkTupleCombine(combine, literal, results);
}

void LibraryResultRegistry::checkSameCombine(const ResultCombineDraft &combine,
                                             ArrayLitExpr *literal, const Type *common) {
  CallExpr *call = combine.call;
  if (!this->isHoldingValues(combine, common)) {
    return this->reportNoValues(combine, common, call->args[0]);
  }
  literal->type = Type::array(common);
  const auto *values = Type::array(common->okType());
  const auto *errors = this->errorsOf(combine, common->errType());
  call->type = Type::result(values, errors);
  for (ArrayLitExpr::Element &element : llvm::reverse(literal->elements)) {
    this->state.walk.tasks.pushTask(ConvertSlot{.slot = &element.expr, .to = common});
  }
}

void LibraryResultRegistry::afterCombineArray(const AfterCombineArray &task) {
  const ResultCombineDraft &combine = task.combine;
  CallExpr *call = combine.call;
  const Expr *list = call->args[0];
  const Type *type = list->type;
  if (type->isError()) {
    return;
  }
  if (!type->isArray() || !type->inner()->isResult()) {
    auto message = ErrorTextCombine::notArray(combine.what, type->name());

    return this->state.diagnostics.error(list->loc(), message);
  }
  const Type *element = type->inner();
  if (!this->isHoldingValues(combine, element)) {
    return this->reportNoValues(combine, element, list);
  }
  const auto *values = Type::array(element->okType());
  const auto *errors = this->errorsOf(combine, element->errType());
  call->type = Type::result(values, errors);
}

void LibraryResultRegistry::checkResultMember(const CheckResultMember &task) {
  MemberExpr *member = task.member;
  auto property = BuiltinPropertyTable::find(member->name);
  if (property == BuiltinProperty::RESULT_VALUE) {
    return this->checkHalfMember(member, ResultHalf::OK);
  }
  if (property == BuiltinProperty::RESULT_ERROR) {
    return this->checkHalfMember(member, ResultHalf::ERR);
  }
  auto receiver = this->receiverText(member->object);
  const auto *method = ResultMethodTable::find(member->name);
  std::string message =
      method != nullptr
          ? ErrorTextResultMember::method(member->name, receiver)
          : ErrorTextResultMember::noMember(member->name, member->object->type->name());
  this->state.diagnostics.error(member->loc(), message);

  return this->state.walk.tasks.pushChecked(member);
}

void LibraryResultRegistry::checkHalfMember(MemberExpr *member, ResultHalf half) {
  auto &tasks = this->state.walk.tasks;
  const Type *type = member->object->type;
  tasks.pushChecked(member);
  if (!this->otherHalfType(type, half)->isNever()) {
    auto receiver = this->receiverText(member->object);
    std::string message = half == ResultHalf::OK ? ErrorTextResultMember::value(receiver)
                                                 : ErrorTextResultMember::error(receiver);

    return this->state.diagnostics.error(member->loc(), message);
  }
  const auto *own = this->halfType(type, half);
  member->type = this->halfParam(own);
  const auto *narrowed = this->state.flow.tracked.narrowedFieldType(member);
  if (narrowed != nullptr) {
    return tasks.pushTask(Convert{.to = narrowed});
  }
  if (!member->type->isOptional()) {
    return;
  }
  auto path = this->state.flow.tracked.fieldPath(member);
  bool isDefined = path.has_value() && this->state.walk.flow.isFieldDefined(*path);
  if (isDefined) {
    return tasks.pushTask(Convert{.to = member->type->inner()});
  }
}

void LibraryResultRegistry::checkUsedValue(const CheckUsedValue &task) {
  auto used = this->checkUsed(task.expr, Discarded::BY_STATEMENT, task.at);
  if (used.isErr()) {
    return this->state.diagnostics.error(task.expr->loc(), used.error().message);
  }
}

void LibraryResultRegistry::checkUsedCallbackValue(const CheckUsedCallbackValue &task) {
  auto used = this->checkUsed(task.expr, Discarded::BY_CALLBACK, llvm::SMLoc());
  if (used.isErr()) {
    return this->state.diagnostics.error(task.expr->loc(), used.error().message);
  }
}

void LibraryResultRegistry::checkResultMethod(const CheckResultMethod &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  auto &tasks = this->state.walk.tasks;
  auto run = this->resultCallOf(call, member);
  tasks.pushChecked(call);
  if (this->state.library.common.hasSpread(call)) {
    return tasks.pushTask(RejectSpreadCall{.call = call});
  }
  const auto *method = ResultMethodTable::find(run.name);
  if (method == nullptr) {
    auto message = ErrorTextResultMethod::noMember(run.name, run.type->name());
    this->state.diagnostics.error(member->loc(), message);

    return tasks.pushTask(WalkArgsLoosely{.call = call});
  }
  int args = ResultMethodTable::countArgs(*method);
  auto arity = this->state.library.common.checkArity(call, args, args);
  if (arity.isErr()) {
    return this->state.diagnostics.error(member->loc(), arity.error().message);
  }
  run.builtin = method->builtin;
  call->target = BuiltinCall{.builtin = run.builtin};
  if (run.builtin == Builtin::ResultIsOk || run.builtin == Builtin::ResultIsErr) {
    call->type = Type::boolTy();

    return;
  }
  if (run.builtin == Builtin::ResultUnwrapOr) {
    const Type *hint = this->isGivingType(run.value) ? run.value : nullptr;
    tasks.pushTask(AfterUnwrapOrArg{.run = run});

    return tasks.pushTask(CheckExprSlot{.slot = &call->args[0], .expected = hint});
  }
  if (run.builtin == Builtin::ResultMatch) {
    return this->checkResultMatch(run, task.expected);
  }
  tasks.pushTask(AfterResultCallback{.run = run});
  tasks.pushTask(CalledFunction{});

  return this->pushCallbackReturn(run, task.expected);
}

void LibraryResultRegistry::afterUnwrapOrArg(const AfterUnwrapOrArg &task) {
  const ResultMethodDraft &run = task.run;
  CallExpr *call = run.call;
  const Expr *arg = call->args[0];
  const Type *other = arg->type;
  if (other->isError()) {
    return;
  }
  const Type *result =
      other->isVoid() ? nullptr : this->state.types.literals.unionUnify(run.value, other);
  if (result == nullptr || result->isError()) {
    auto message = ErrorTextResultMethod::unwrapOr(run.value->name(), other->name());

    return this->state.diagnostics.error(arg->loc(), message);
  }
  call->type = result;

  return this->state.walk.tasks.pushTask(ConvertSlot{.slot = &call->args[0], .to = result});
}

void LibraryResultRegistry::checkResultMatch(const ResultMethodDraft &run, const Type *expected) {
  auto &tasks = this->state.walk.tasks;
  const auto *wanted = this->wantedType(expected);
  const Type *both = wanted != nullptr && !wanted->isVoid() ? wanted : nullptr;
  tasks.pushTask(AfterResultMatch{.run = run});
  tasks.pushTask(CalledFunction{});
  tasks.pushTask(ResultHalfCallback{.run = run, .index = 1, .half = run.error, .returnType = both});

  return tasks.pushTask(
      ResultHalfCallback{.run = run, .index = 0, .half = run.value, .returnType = both});
}

void LibraryResultRegistry::afterResultMatch(const AfterResultMatch &task) {
  const ResultMethodDraft &run = task.run;
  CallExpr *call = run.call;
  auto returns = this->state.walk.tasks.takeAnswers(2);
  const Type *valueReturn = returns[0];
  const Type *errorReturn = returns[1];
  if (valueReturn == nullptr || errorReturn == nullptr) {
    return;
  }
  if (valueReturn->isVoid() && errorReturn->isVoid()) {
    call->type = valueReturn;

    return;
  }
  bool isOneVoid = valueReturn->isVoid() || errorReturn->isVoid();
  const Type *result =
      isOneVoid ? nullptr : this->state.types.literals.unionUnify(valueReturn, errorReturn);
  if (result == nullptr || result->isError()) {
    auto message = ErrorTextResultMethod::match(valueReturn->name(), errorReturn->name());

    return this->state.diagnostics.error(run.member->loc(), message);
  }
  call->type = result;
}

void LibraryResultRegistry::pushCallbackReturn(const ResultMethodDraft &run, const Type *expected) {
  Builtin builtin = run.builtin;
  bool isOnError = builtin == Builtin::ResultMapErr || builtin == Builtin::ResultOrElse ||
                   builtin == Builtin::ResultOrTee;
  const auto *wanted = this->wantedType(expected);
  const Type *wantedResult = wanted != nullptr && wanted->isResult() ? wanted : nullptr;
  const Type *half = isOnError ? run.error : run.value;
  const Type *returnType = this->callbackReturnType(builtin, wantedResult, isOnError);

  return this->state.walk.tasks.pushTask(
      ResultHalfCallback{.run = run, .index = 0, .half = half, .returnType = returnType});
}

void LibraryResultRegistry::resultHalfCallback(const ResultHalfCallback &task) {
  const ResultMethodDraft &run = task.run;
  auto &tasks = this->state.walk.tasks;
  Expr **slot = &run.call->args[task.index];
  bool isFunction = isa<FunctionExpr>(*slot);
  if (task.half->isNever() && !isFunction) {
    tasks.pushTask(AfterPlainHalfCallback{.run = run, .index = task.index});

    return tasks.pushTask(CheckExprSlot{.slot = slot, .expected = nullptr});
  }
  bool isTee = run.builtin == Builtin::ResultAndTee || run.builtin == Builtin::ResultOrTee;
  auto signature = Signature{
      .params = {this->halfParam(task.half)},
      .ret = isFunction && !isTee ? task.returnType : nullptr,
      .voidResult = isTee,
      .pad = false,
  };
  tasks.pushTask(AfterHalfCallback{.run = run, .index = task.index});

  return tasks.pushTask(CheckFunctionArgSlot{
      .slot = slot,
      .expected = std::move(signature),
      .primary = 1,
      .method = run.name,
  });
}

void LibraryResultRegistry::afterPlainHalfCallback(const AfterPlainHalfCallback &task) {
  const ResultMethodDraft &run = task.run;
  const Expr *arg = run.call->args[task.index];
  auto &tasks = this->state.walk.tasks;
  if (arg->type->isFunction()) {
    return tasks.pushAnswer(arg->type->ret());
  }
  if (!arg->type->isError()) {
    auto message = ErrorTextResultMethod::function(run.name, arg->type->name());
    this->state.diagnostics.error(arg->loc(), message);
  }

  return tasks.pushAnswer(nullptr);
}

void LibraryResultRegistry::afterHalfCallback(const AfterHalfCallback &task) {
  const Expr *arg = task.run.call->args[task.index];
  const Type *returned = arg->type->isFunction() ? arg->type->ret() : nullptr;

  return this->state.walk.tasks.pushAnswer(returned);
}

void LibraryResultRegistry::afterResultCallback(const AfterResultCallback &task) {
  const Type *returnType = this->state.walk.tasks.takeAnswer();
  if (returnType == nullptr) {
    return;
  }

  return this->typeResultCall(task.run, returnType);
}

void LibraryResultRegistry::resolveResultType(const ResolveResultType &task) {
  const TypeRef &ref = *task.ref;
  auto &tasks = this->state.walk.tasks;
  auto args = this->checkResultTypeArgs(ref);
  if (args.isErr()) {
    this->state.diagnostics.error(ref.loc, args.error().message);

    return tasks.pushAnswer(Type::error());
  }
  tasks.pushTask(FinishResultType{.typeName = task.typeName});
  tasks.pushTask(ResultHalfType{.arg = &ref.args[1]});

  return tasks.pushTask(ResultHalfType{.arg = &ref.args[0]});
}

void LibraryResultRegistry::resultHalfType(const ResultHalfType &task) {
  const TypeRef &arg = *task.arg;
  bool isNever =
      arg.kind() == TypeRefKind::NAME_REF && TypeNameTable::isName(arg.name, TypeName::NEVER);
  if (isNever) {
    return this->state.walk.tasks.pushAnswer(Type::never());
  }

  return this->state.walk.tasks.pushTask(ResolveTypeAnswer{.ref = &arg, .use = TypeUse::INNER});
}

void LibraryResultRegistry::finishResultType(const FinishResultType &task) {
  auto halves = this->state.walk.tasks.takeAnswers(2);
  const auto *value = task.typeName == TypeName::ERR ? Type::never() : halves[0];
  const auto *error = task.typeName == TypeName::OK ? Type::never() : halves[1];
  const auto *result = Type::result(value, error);

  return this->state.walk.tasks.pushAnswer(result);
}

void LibraryResultRegistry::neverOperands(const NeverOperands &task) {
  BinaryExpr *binary = task.binary;
  auto &tasks = this->state.walk.tasks;
  const Type *leftType = binary->lhs->type;
  const Type *rightType = binary->rhs->type;
  if (leftType->isNever() && rightType->isNever()) {
    tasks.pushTask(ConvertSlot{.slot = &binary->rhs, .to = Type::doubleTy()});

    return tasks.pushTask(ConvertSlot{.slot = &binary->lhs, .to = Type::doubleTy()});
  }
  if (leftType->isNever() && !rightType->isError() && !rightType->isVoid()) {
    return tasks.pushTask(ConvertSlot{.slot = &binary->lhs, .to = rightType});
  }
  if (rightType->isNever() && !leftType->isError() && !leftType->isVoid()) {
    return tasks.pushTask(ConvertSlot{.slot = &binary->rhs, .to = leftType});
  }
}

const Type *LibraryResultRegistry::callbackReturnType(Builtin builtin, const Type *wantedResult,
                                                      bool isOnError) const {
  bool isMapping = builtin == Builtin::ResultMap || builtin == Builtin::ResultMapErr;
  if (isMapping && wantedResult != nullptr) {
    const Type *wantedHalf = isOnError ? wantedResult->errType() : wantedResult->okType();
    bool isShaped = wantedHalf->holdsObjects() || wantedHalf->holdsResults();

    return isShaped ? wantedHalf : nullptr;
  }
  if (builtin == Builtin::ResultAndThen || builtin == Builtin::ResultOrElse) {
    return wantedResult;
  }

  return nullptr;
}

LibraryResultRegistry::ResultOkErrTypeArgs
LibraryResultRegistry::checkOkErrTypeArgs(const NameExpr *name) const {
  if (name->typeArgs.size() != 2) {
    auto message = ErrorTextOkErr::typeArgs(name->name);

    return err(LibraryResultRegistryOkErrTypeArgsError{
        .code = LibraryResultRegistryOkErrTypeArgsErrorCode::NOT_TWO_TYPE_ARGUMENTS,
        .message = message,
    });
  }

  return ok(name);
}

const Type *LibraryResultRegistry::halfType(const Type *result, ResultHalf half) const {
  return half == ResultHalf::OK ? result->okType() : result->errType();
}

const Type *LibraryResultRegistry::otherHalfType(const Type *result, ResultHalf half) const {
  return half == ResultHalf::OK ? result->errType() : result->okType();
}

std::string LibraryResultRegistry::receiverText(const Expr *expr) const {
  auto *conversion = dyn_cast<CastExpr>(expr);
  bool isConverted = conversion != nullptr && conversion->isImplicit();
  const Expr *bare = isConverted ? conversion->operand : expr;
  auto *name = dyn_cast<NameExpr>(bare);
  if (name == nullptr || name->name.starts_with(kInternalPrefix)) {
    return kResultPlaceholder.str();
  }

  return name->name.str();
}

const Type *LibraryResultRegistry::expectedResult(const Type *expected) const {
  if (expected == nullptr || !expected->nonOptional()->isResult()) {
    return nullptr;
  }

  return expected->nonOptional();
}

LibraryResultRegistry::ResultStaticIsKnown
LibraryResultRegistry::checkStaticMethod(const NameExpr *name, llvm::StringRef method) const {
  auto found = ResultStaticTable::find(method);
  if (!found.has_value()) {
    auto message = ErrorTextCombine::noStatic(name->name, method);

    return err(LibraryResultRegistryStaticMethodError{
        .code = LibraryResultRegistryStaticMethodErrorCode::NO_SUCH_STATIC_METHOD,
        .message = message,
    });
  }

  return ok(*found);
}

bool LibraryResultRegistry::isPlainList(const ArrayLitExpr *literal) const {
  if (literal->elements.empty()) {
    return false;
  }
  for (const ArrayLitExpr::Element &element : literal->elements) {
    if (element.spread || element.hole) {
      return false;
    }
  }

  return true;
}

bool LibraryResultRegistry::isHoldingValues(const ResultCombineDraft &combine,
                                            const Type *result) const {
  bool isLosingErrors = combine.isAll && result->errType()->isVoid();

  return this->isGivingType(result->okType()) && !isLosingErrors;
}

void LibraryResultRegistry::reportNoValues(const ResultCombineDraft &combine, const Type *result,
                                           const Expr *at) {
  bool hasValues = this->isGivingType(result->okType());
  auto message = ErrorTextCombine::holds(combine.what, hasValues, result->name());
  this->state.diagnostics.error(at->loc(), message);
}

const Type *LibraryResultRegistry::errorsOf(const ResultCombineDraft &combine,
                                            const Type *error) const {
  if (!combine.isAll || error->isNever()) {
    return error;
  }

  return Type::array(error);
}

const Type *LibraryResultRegistry::sameValues(const Type *first, const Type *second) const {
  const auto *value = this->joinedValue(first->okType(), second->okType());
  const auto *error = this->state.library.result.unifyHalf(first->errType(), second->errType());
  bool isJoined = value != nullptr && !value->isError() && error != nullptr;

  return isJoined ? Type::result(value, error) : nullptr;
}

const Type *LibraryResultRegistry::joinedValue(const Type *first, const Type *second) const {
  if (first == second || second->isNever()) {
    return first;
  }
  if (first->isNever()) {
    return second;
  }
  if (first->isVoid() || second->isVoid()) {
    return nullptr;
  }

  return this->state.types.relations.unify(first, second);
}

const Type *LibraryResultRegistry::commonResult(llvm::ArrayRef<const Type *> results) const {
  const Type *common = results[0];
  for (const Type *type : results) {
    if (common == nullptr) {
      return nullptr;
    }
    common = this->sameValues(common, type);
  }

  return common;
}

void LibraryResultRegistry::checkTupleCombine(const ResultCombineDraft &combine,
                                              ArrayLitExpr *literal,
                                              const std::vector<const Type *> &results) {
  CallExpr *call = combine.call;
  auto values = std::vector<const Type *>();
  const auto *error = Type::never();
  for (size_t index = 0; index < results.size(); ++index) {
    const Type *result = results[index];
    const Expr *element = literal->elements[index].expr;
    if (!this->isHoldingValues(combine, result)) {
      this->reportNoValues(combine, result, element);

      return;
    }
    values.push_back(result->okType());
    const auto *joined = this->state.library.result.unifyHalf(error, result->errType());
    if (joined == nullptr) {
      auto message = ErrorTextCombine::noUnion(combine.what, error, result->errType());
      this->state.diagnostics.error(element->loc(), message);

      return;
    }
    error = joined;
  }
  literal->type = Type::tuple(results);
  const auto *tuple = Type::tuple(values);
  const auto *errors = this->errorsOf(combine, error);
  call->type = Type::result(tuple, errors);
}

LibraryResultRegistry::ResultValueGetsUsed
LibraryResultRegistry::checkUsed(const Expr *expr, Discarded how, llvm::SMLoc at) {
  const Type *type = expr->type;
  if (type != nullptr && type->holdsPromises()) {
    return this->checkPromiseUsed(expr, how, at);
  }
  if (type == nullptr || !type->holdsResults() || isa<AssignExpr>(expr)) {
    return ok(expr);
  }
  if (this->state.library.async.isTakingElement(expr)) {
    return ok(expr);
  }
  bool isResult = type->nonOptional()->isResult();
  if (!isResult && this->state.library.async.isMappingCall(expr)) {
    auto message = ErrorTextUnusedResult::results();

    return err(LibraryResultRegistryUsedValueError{
        .code = LibraryResultRegistryUsedValueErrorCode::RESULTS_UNUSED,
        .message = message,
    });
  }
  if (!isResult) {
    return ok(expr);
  }
  if (how == Discarded::BY_CALLBACK) {
    auto message = ErrorTextUnusedResult::callback();

    return err(LibraryResultRegistryUsedValueError{
        .code = LibraryResultRegistryUsedValueErrorCode::CALLBACK_RESULT_DROPPED,
        .message = message,
    });
  }
  auto message = ErrorTextUnusedResult::result();

  return err(LibraryResultRegistryUsedValueError{
      .code = LibraryResultRegistryUsedValueErrorCode::RESULT_UNUSED,
      .message = message,
  });
}

LibraryResultRegistry::ResultValueGetsUsed
LibraryResultRegistry::checkPromiseUsed(const Expr *expr, Discarded how, llvm::SMLoc at) {
  bool isCallback = how == Discarded::BY_CALLBACK;
  auto handled = this->state.library.async.checkDiscardedPromise(expr, isCallback, at);
  if (handled.isErr()) {
    return err(LibraryResultRegistryUsedValueError{
        .code = LibraryResultRegistryUsedValueErrorCode::PROMISE_DROPPED,
        .message = handled.error().message,
    });
  }

  return ok(expr);
}

const Type *LibraryResultRegistry::halfParam(const Type *half) const {
  return half->isVoid() ? Type::undefinedTy() : half;
}

bool LibraryResultRegistry::isGivingType(const Type *half) const {
  return !half->isNever() && !half->isVoid();
}

ResultMethodDraft LibraryResultRegistry::resultCallOf(CallExpr *call, MemberExpr *member) const {
  const Type *type = member->object->type;
  const Type *value = type->okType();
  const Type *error = type->errType();
  auto *conversion = dyn_cast<CastExpr>(member->object);
  if (conversion != nullptr && conversion->isImplicit() && conversion->operand->type->isResult()) {
    if (value->isNever()) {
      value = conversion->operand->type->okType();
    }
    if (error->isNever()) {
      error = conversion->operand->type->errType();
    }
  }

  return ResultMethodDraft{
      .call = call,
      .member = member,
      .type = type,
      .value = value,
      .error = error,
      .builtin = Builtin::None,
      .name = member->name,
  };
}

const Type *LibraryResultRegistry::wantedType(const Type *expected) const {
  const Type *wanted = expected != nullptr ? expected->nonOptional() : nullptr;
  if (wanted != nullptr && wanted->isError()) {
    return nullptr;
  }

  return wanted;
}

void LibraryResultRegistry::typeResultCall(const ResultMethodDraft &run, const Type *returnType) {
  Builtin builtin = run.builtin;
  if (builtin == Builtin::ResultMap) {
    run.call->type = Type::result(returnType, run.error);
    return;
  }
  if (builtin == Builtin::ResultMapErr) {
    run.call->type = Type::result(run.value, returnType);
    return;
  }
  if (builtin == Builtin::ResultAndThen || builtin == Builtin::ResultAndThrough) {
    this->typeChainedCall(run, returnType);
    return;
  }
  if (builtin == Builtin::ResultOrElse) {
    this->typeOrElseCall(run, returnType);
    return;
  }
  run.call->type = run.type;
}

bool LibraryResultRegistry::reportNotResult(const ResultMethodDraft &run, const Type *returnType) {
  if (returnType->isResult()) {
    return false;
  }
  bool isAndThen = run.builtin == Builtin::ResultAndThen;
  auto message = ErrorTextResultMethod::result(run.name, returnType->name(), isAndThen);
  this->state.diagnostics.error(run.call->args[0]->loc(), message);

  return true;
}

void LibraryResultRegistry::typeChainedCall(const ResultMethodDraft &run, const Type *returnType) {
  if (this->reportNotResult(run, returnType)) {
    return;
  }
  const auto *common = this->state.library.result.unifyHalf(run.error, returnType->errType());
  if (common == nullptr) {
    auto message = ErrorTextResultMethod::errors(run.error, run.name, returnType->errType());
    this->state.diagnostics.error(run.call->args[0]->loc(), message);
    return;
  }
  const Type *okType = run.builtin == Builtin::ResultAndThen ? returnType->okType() : run.value;
  run.call->type = Type::result(okType, common);
}

void LibraryResultRegistry::typeOrElseCall(const ResultMethodDraft &run, const Type *returnType) {
  if (this->reportNotResult(run, returnType)) {
    return;
  }
  const auto *common = this->state.library.result.unifyHalf(run.value, returnType->okType());
  if (common == nullptr) {
    auto message = ErrorTextResultMethod::values(run.value, returnType->okType());
    this->state.diagnostics.error(run.call->args[0]->loc(), message);
    return;
  }
  run.call->type = Type::result(common, returnType->errType());
}

LibraryResultRegistry::ResultTypeArgsPair
LibraryResultRegistry::checkResultTypeArgs(const TypeRef &ref) const {
  if (ref.args.size() != 2) {
    auto message = ErrorTextResultType::typeArgs(ref.name);

    return err(LibraryResultRegistryResultTypeArgsError{
        .code = LibraryResultRegistryResultTypeArgsErrorCode::NOT_TWO_TYPE_ARGUMENTS,
        .message = message,
    });
  }

  return ok(&ref);
}

}
