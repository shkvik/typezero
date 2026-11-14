#include "compiler/semantic/code/functions/closure_signatures/closure_signatures.h"
#include "compiler/program/library/builtin_names/keyword_table/keyword_table.h"
#include "compiler/semantic/bodies/shared_helpers/shared_helpers.h"
#include "compiler/semantic/code/functions/code_functions_registry_errors.h"

namespace tz {

namespace {

constexpr llvm::StringLiteral kHiddenPrefix = "%";
constexpr llvm::StringLiteral kRestPartSuffix = ".rest";

}

ClosureSignatures::ClosureSignatures(SemanticState &owner)
    : state(owner) {}

void ClosureSignatures::checkFunctionExpr(const CheckFunctionExpr &task) {
  FunctionExpr *functionExpr = task.functionExpr;
  FunctionDecl *functionDecl = functionExpr->fn;
  auto *function = this->makeClosureSymbol(functionDecl);
  auto plain = this->checkNotGeneric(functionDecl);
  bool isGeneric = plain.isErr();
  if (isGeneric) {
    this->state.diagnostics.error(functionExpr->loc(), plain.error().message);
    functionDecl->typeArgs.assign(functionDecl->typeParams.size(), Type::error());
    this->state.walk.scopes.openTypeScope();
  }
  bool isThisFromContext = this->addContextThis(functionExpr, task.expected);
  auto draft = ClosureDraft{
      .functionExpr = functionExpr,
      .function = function,
      .signature = task.expected,
      .isGeneric = isGeneric,
      .isThisFromContext = isThisFromContext,
      .index = 0,
  };
  if (!isGeneric) {
    return this->nextClosureParam(NextClosureParam{.draft = std::move(draft)});
  }
  this->state.walk.tasks.pushTask(NextClosureParam{.draft = std::move(draft)});

  return this->state.walk.tasks.pushTask(BindFunctionTypeParams{.functionDecl = functionDecl});
}

bool ClosureSignatures::addContextThis(FunctionExpr *functionExpr,
                                       const std::optional<Signature> &signature) {
  FunctionDecl *functionDecl = functionExpr->fn;
  bool isThisWritten = !functionDecl->params.empty() &&
                       functionDecl->params[0].name == KeywordTable::getName(Keyword::THIS) &&
                       !functionDecl->isArrow();
  bool isWanted =
      signature.has_value() && signature->hasThis && !signature->params.empty() && !isThisWritten;
  if (!isWanted) {
    return false;
  }
  llvm::StringRef name =
      functionDecl->isArrow() ? this->state.ast.hiddenName() : KeywordTable::getName(Keyword::THIS);
  auto self = Param{
      .name = name,
      .loc = functionExpr->loc(),
  };
  functionDecl->params.insert(functionDecl->params.begin(), self);

  return true;
}

void ClosureSignatures::nextClosureParam(const NextClosureParam &task) {
  auto draft = task.draft;
  std::vector<Param> &params = draft.functionExpr->fn->params;
  while (draft.index < params.size() && !params[draft.index].type.has_value()) {
    const Type *given = this->givenParam(draft);
    auto types = this->unwrittenParamTypes(params[draft.index], given);
    this->declareClosureParam(draft, types);
    draft.index += 1;
  }
  if (draft.index == params.size()) {
    return this->finishClosureParams(draft);
  }
  const Param &param = params[draft.index];
  this->state.types.context.enterListContext(param.rest());
  this->state.walk.tasks.pushTask(AfterClosureParamType{.draft = draft});

  return this->state.walk.tasks.pushTask(
      ResolveTypeAnswer{.ref = &*param.type, .use = TypeUse::STANDALONE});
}

void ClosureSignatures::afterClosureParamType(const AfterClosureParamType &task) {
  this->state.types.context.leaveListContext();
  const Type *written = this->state.walk.tasks.takeAnswer();
  auto draft = task.draft;
  const Param &param = draft.functionExpr->fn->params[draft.index];
  const Type *given = this->givenParam(draft);
  auto types = this->writtenParamTypes(param, written, given);
  this->declareClosureParam(draft, types);
  draft.index += 1;

  return this->nextClosureParam(NextClosureParam{.draft = std::move(draft)});
}

const Type *ClosureSignatures::givenParam(const ClosureDraft &draft) const {
  const std::optional<Signature> &signature = draft.signature;
  bool isGiven = signature.has_value() && draft.index < signature->params.size();

  return isGiven ? signature->params[draft.index] : nullptr;
}

ClosureSignatures::ParamTypes ClosureSignatures::unwrittenParamTypes(const Param &param,
                                                                     const Type *given) {
  if (given != nullptr) {
    return this->givenParamTypes(param, given);
  }
  const Type *literal =
      param.defaultValue() != nullptr ? SharedHelpers::literalType(param.defaultValue()) : nullptr;
  if (literal != nullptr) {
    return ParamTypes{
        .type = literal,
        .passed = Type::optional(literal),
    };
  }
  auto known = this->checkParamTypeKnown(param);
  if (known.isErr()) {
    this->state.diagnostics.error(param.loc, known.error().message);
  }

  return ParamTypes{
      .type = Type::error(),
      .passed = Type::error(),
  };
}

ClosureSignatures::ParamTypes
ClosureSignatures::writtenParamTypes(const Param &param, const Type *written, const Type *given) {
  const Type *type = param.optional() ? Type::optional(written) : written;
  const Type *passed = this->isOmittableParam(param) ? Type::optional(type) : type;
  auto agrees = this->checkParamAgrees(param, passed, given);
  if (agrees.isErr()) {
    this->state.diagnostics.error(param.loc, agrees.error().message);
    return ParamTypes{
        .type = this->givenInnerType(param, given),
        .passed = given,
    };
  }

  return ParamTypes{
      .type = type,
      .passed = passed,
  };
}

void ClosureSignatures::declareClosureParam(const ClosureDraft &draft, const ParamTypes &types) {
  FunctionDecl *functionDecl = draft.functionExpr->fn;
  FuncSymbol *function = draft.function;
  Param &param = functionDecl->params[draft.index];
  bool isOmittable = this->isOmittableParam(param);
  if (draft.index == 0) {
    this->walkClosureThis(draft, param, isOmittable);
  }
  param.declaredType = types.type;
  if (param.rest()) {
    return this->walkClosureRest(function, param, types.type);
  }
  if (!isOmittable) {
    this->walkRequiredParam(functionDecl, function, draft.index);
  }
  function->params.push_back(types.passed);
}

void ClosureSignatures::walkClosureThis(const ClosureDraft &draft, const Param &param,
                                        bool isParamOmittable) {
  if (draft.isThisFromContext) {
    draft.function->hasThis = true;
    return;
  }
  if (param.name != KeywordTable::getName(Keyword::THIS) || !param.type.has_value() ||
      param.rest()) {
    return;
  }
  auto valid = this->checkThisParam(draft.functionExpr->fn, param);
  if (valid.isErr()) {
    this->state.diagnostics.error(param.loc, valid.error().message);
    return;
  }
  draft.function->hasThis = !isParamOmittable;
}

void ClosureSignatures::finishClosureParams(const ClosureDraft &draft) {
  FunctionDecl *functionDecl = draft.functionExpr->fn;
  this->padHiddenParams(draft);
  this->expandArgumentList(functionDecl, draft.function);
  this->state.walk.tasks.pushTask(AfterClosureReturn{.draft = draft});
  if (functionDecl->isGenerator) {
    return this->startGeneratorReturn(draft);
  }
  if (functionDecl->isAsync) {
    return this->startAsyncReturn(draft);
  }

  return this->startPlainReturn(draft);
}

void ClosureSignatures::padHiddenParams(const ClosureDraft &draft) {
  FunctionDecl *functionDecl = draft.functionExpr->fn;
  FuncSymbol *function = draft.function;
  const std::optional<Signature> &signature = draft.signature;
  if (!signature.has_value() || !signature->pad || function->hasRest) {
    return;
  }
  size_t named = functionDecl->params.size();
  for (size_t index = named; index < signature->params.size(); ++index) {
    auto hidden = Param{
        .name = this->state.ast.hiddenName(),
        .loc = draft.functionExpr->loc(),
        .declaredType = signature->params[index],
    };
    functionDecl->params.push_back(hidden);
    function->params.push_back(signature->params[index]);
  }
  function->minArgs = signature->minArgs;
}

void ClosureSignatures::startGeneratorReturn(const ClosureDraft &draft) {
  FunctionDecl *functionDecl = draft.functionExpr->fn;
  const std::optional<Signature> &signature = draft.signature;
  if (functionDecl->returnType.has_value()) {
    return this->resolveClosureReturn(draft);
  }
  if (!signature.has_value() || signature->ret == nullptr) {
    return;
  }
  bool isIteratorResult =
      functionDecl->isAsync ? signature->ret->isAsyncIterator() : signature->ret->isIterator();
  if (isIteratorResult) {
    draft.function->setReturnType(signature->ret);
  }
}

void ClosureSignatures::startAsyncReturn(const ClosureDraft &draft) {
  FunctionDecl *functionDecl = draft.functionExpr->fn;
  FuncSymbol *function = draft.function;
  const std::optional<Signature> &signature = draft.signature;
  if (signature.has_value() && signature->voidResult) {
    auto awaited = this->checkPromiseAwaited(*signature);
    if (awaited.isErr()) {
      this->state.diagnostics.error(draft.functionExpr->loc(), awaited.error().message);
    }
    function->setReturnType(Type::voidTy());
    function->discardsResult = true;
    return;
  }
  if (functionDecl->returnType.has_value()) {
    return this->resolveClosureReturn(draft);
  }
  if (signature.has_value() && signature->ret != nullptr && signature->ret->isPromise()) {
    function->setReturnType(signature->ret);
    return;
  }
  if (!this->state.inference.returns.isReturningValue(functionDecl->body)) {
    function->setReturnType(Type::promise(Type::voidTy()));
  }
}

void ClosureSignatures::startPlainReturn(const ClosureDraft &draft) {
  FunctionDecl *functionDecl = draft.functionExpr->fn;
  FuncSymbol *function = draft.function;
  const std::optional<Signature> &signature = draft.signature;
  if (signature.has_value() && signature->voidResult) {
    function->setReturnType(Type::voidTy());
    function->discardsResult = true;
    return;
  }
  if (functionDecl->returnType.has_value()) {
    return this->state.walk.tasks.pushTask(DeclareMethodReturn{.functionDecl = functionDecl});
  }
  if (signature.has_value() && signature->ret != nullptr) {
    function->setReturnType(signature->ret);
    return;
  }
  if (!this->state.inference.returns.isReturningValue(functionDecl->body)) {
    function->setReturnType(Type::voidTy());
  }
}

void ClosureSignatures::resolveClosureReturn(const ClosureDraft &draft) {
  FunctionDecl *functionDecl = draft.functionExpr->fn;
  this->state.walk.tasks.pushTask(
      AfterClosureReturnType{.functionDecl = functionDecl, .function = draft.function});

  return this->state.walk.tasks.pushTask(
      ResolveTypeAnswer{.ref = &*functionDecl->returnType, .use = TypeUse::STANDALONE_OR_VOID});
}

void ClosureSignatures::afterClosureReturnType(const AfterClosureReturnType &task) {
  const Type *declared = this->state.walk.tasks.takeAnswer();
  task.function->setReturnType(declared);
  if (task.functionDecl->isGenerator) {
    return this->state.walk.tasks.pushTask(
        WalkGeneratorReturn{.functionDecl = task.functionDecl, .function = task.function});
  }

  return this->state.walk.tasks.pushTask(
      WalkAsyncReturn{.functionDecl = task.functionDecl, .function = task.function});
}

ClosureSignatures::ResultPromiseAwaited
ClosureSignatures::checkPromiseAwaited(const Signature &signature) const {
  if (!signature.asyncVoid) {
    auto message = ErrorTextPromiseAwaited::dropped();

    return err(CodeFunctionsRegistryPromiseAwaitedError{
        .code = CodeFunctionsRegistryPromiseAwaitedErrorCode::PROMISE_DROPPED,
        .message = message,
    });
  }

  return ok(&signature);
}

ClosureSignatures::ResultExprNotGeneric
ClosureSignatures::checkNotGeneric(const FunctionDecl *functionDecl) const {
  if (!functionDecl->typeParams.empty()) {
    auto message = ErrorTextNotGeneric::generic();

    return err(CodeFunctionsRegistryNotGenericError{
        .code = CodeFunctionsRegistryNotGenericErrorCode::GENERIC_FUNCTION_EXPRESSION,
        .message = message,
    });
  }

  return ok(functionDecl);
}

ClosureSignatures::ResultParamTypeAgree
ClosureSignatures::checkParamAgrees(const Param &param, const Type *passed,
                                    const Type *given) const {
  if (given != nullptr && passed != given && !passed->isError() && !given->isError() &&
      !param.rest()) {
    auto message = ErrorTextParamAgrees::declared(param.label(), passed->name(), given->name());

    return err(CodeFunctionsRegistryParamAgreesError{
        .code = CodeFunctionsRegistryParamAgreesErrorCode::DECLARED_DIFFERENTLY,
        .message = message,
    });
  }

  return ok(passed);
}

ClosureSignatures::ResultDefaultMatters
ClosureSignatures::checkDefaultMatters(const Param &param, const Type *given) const {
  if (param.defaultValue() != nullptr && !given->isOptional()) {
    auto message = ErrorTextDefaultMatters::unused(param.label());

    return err(CodeFunctionsRegistryDefaultMattersError{
        .code = CodeFunctionsRegistryDefaultMattersErrorCode::DEFAULT_NEVER_USED,
        .message = message,
    });
  }

  return ok(&param);
}

ClosureSignatures::ResultParamTypeKnown
ClosureSignatures::checkParamTypeKnown(const Param &param) const {
  if (!param.type) {
    auto message = ErrorTextParamTypeKnown::needsType(param.label());

    return err(CodeFunctionsRegistryClosureParamTypeKnownError{
        .code = CodeFunctionsRegistryClosureParamTypeKnownErrorCode::NEEDS_TYPE,
        .message = message,
    });
  }

  return ok(&param);
}

ClosureSignatures::ResultThisParamValid
ClosureSignatures::checkThisParam(const FunctionDecl *functionDecl, const Param &param) const {
  if (functionDecl->isArrow()) {
    auto message = ErrorTextThisParam::arrowThis();

    return err(CodeFunctionsRegistryClosureThisParamError{
        .code = CodeFunctionsRegistryClosureThisParamErrorCode::ARROW_THIS_PARAMETER,
        .message = message,
    });
  }

  return ok(&param);
}

ClosureSignatures::ResultRestParamArray ClosureSignatures::checkRestParam(const Param &param,
                                                                          const Type *type) const {
  if (!type->isArray() && !type->isListParam() && !type->isTuple() && !type->isError()) {
    auto message = ErrorTextRestParam::notArray(param.name);

    return err(CodeFunctionsRegistryRestParamError{
        .code = CodeFunctionsRegistryRestParamErrorCode::REST_NOT_ARRAY,
        .message = message,
    });
  }

  return ok(type);
}

ClosureSignatures::ResultParamOrderGood
ClosureSignatures::checkParamOrder(const Param &param, bool isAfterOptional) const {
  if (isAfterOptional) {
    auto message = ErrorTextParamOrder::order();

    return err(CodeFunctionsRegistryParamOrderError{
        .code = CodeFunctionsRegistryParamOrderErrorCode::REQUIRED_AFTER_OPTIONAL,
        .message = message,
    });
  }

  return ok(&param);
}

FuncSymbol *ClosureSignatures::makeClosureSymbol(FunctionDecl *functionDecl) {
  auto *function = this->state.ast.newFunc();
  function->setCompleter(this->state.completer);
  function->name = functionDecl->name;
  function->loc = functionDecl->loc();
  function->decl = functionDecl;
  functionDecl->sym = function;
  functionDecl->parent = this->state.walk.functions.getCurrentDecl();
  functionDecl->creationSeq = this->state.declaration.order.takeSequenceNumber();

  return function;
}

bool ClosureSignatures::isOmittableParam(const Param &param) const {
  return param.optional() || param.defaultValue() != nullptr;
}

void ClosureSignatures::walkClosureRest(FuncSymbol *function, const Param &param,
                                        const Type *type) {
  auto array = this->checkRestParam(param, type);
  if (array.isErr()) {
    this->state.diagnostics.error(param.loc, array.error().message);
  }
  function->hasRest = true;
  function->params.push_back(type);
}

void ClosureSignatures::walkRequiredParam(const FunctionDecl *functionDecl, FuncSymbol *function,
                                          size_t index) {
  const Param &param = functionDecl->params[index];
  bool isAfterOptional = this->hasOmittableBefore(functionDecl, index);
  auto order = this->checkParamOrder(param, isAfterOptional);
  if (order.isErr()) {
    this->state.diagnostics.error(param.loc, order.error().message);
  }
  function->minArgs = index + 1;
}

bool ClosureSignatures::hasOmittableBefore(const FunctionDecl *functionDecl, size_t index) const {
  for (size_t earlier = 0; earlier < index; ++earlier) {
    if (this->isOmittableParam(functionDecl->params[earlier])) {
      return true;
    }
  }

  return false;
}

ClosureSignatures::ParamTypes ClosureSignatures::givenParamTypes(const Param &param,
                                                                 const Type *given) {
  auto matters = this->checkDefaultMatters(param, given);
  if (matters.isErr()) {
    this->state.diagnostics.error(param.defaultValue()->loc(), matters.error().message);
  }

  return ParamTypes{
      .type = this->givenInnerType(param, given),
      .passed = given,
  };
}

const Type *ClosureSignatures::givenInnerType(const Param &param, const Type *given) const {
  if (param.defaultValue() != nullptr) {
    return given->nonOptional();
  }

  return given;
}

bool ClosureSignatures::isExpandableRest(const FunctionDecl *functionDecl,
                                         const FuncSymbol *function) const {
  if (!function->hasRest || functionDecl->params.empty()) {
    return false;
  }

  return functionDecl->params.back().rest() && functionDecl->listName.empty();
}

void ClosureSignatures::expandArgumentList(FunctionDecl *functionDecl, FuncSymbol *function) {
  if (!this->isExpandableRest(functionDecl, function)) {
    return;
  }
  const Type *list = function->params.back();
  auto flat = std::vector<const Type *>();
  size_t minArgs = function->minArgs;
  bool hasRest = function->hasRest;
  if (!ArgumentLists::flattenArguments(function->params, minArgs, hasRest, flat)) {
    return;
  }
  Param rest = functionDecl->params.back();
  functionDecl->params.pop_back();
  functionDecl->listName = rest.name;
  functionDecl->listStart = functionDecl->params.size();
  functionDecl->listType = list;
  for (size_t index = 0; index < list->fields().size(); ++index) {
    auto param = this->listPartParam(rest, list, index);
    functionDecl->params.push_back(param);
  }
  const Type *more = list->tupleRest();
  if (more != nullptr) {
    auto param = this->listRestParam(rest, more);
    functionDecl->params.push_back(param);
  }
  function->params = std::move(flat);
  function->minArgs = minArgs;
  function->hasRest = hasRest;
}

Param ClosureSignatures::listPartParam(const Param &rest, const Type *list, size_t index) {
  std::string name = kHiddenPrefix.str() + rest.name.str() + "." + std::to_string(index);
  Param param;
  param.name = this->state.ast.keepName(name);
  param.loc = rest.loc;
  if (index >= list->tupleMin()) {
    param.form = OptionalParam{};
  }
  param.declaredType = list->fields()[index].type;

  return param;
}

Param ClosureSignatures::listRestParam(const Param &rest, const Type *more) {
  std::string name = kHiddenPrefix.str() + rest.name.str() + kRestPartSuffix.str();
  Param param;
  param.name = this->state.ast.keepName(name);
  param.loc = rest.loc;
  param.form = RestParam{};
  param.declaredType = more;

  return param;
}

}
