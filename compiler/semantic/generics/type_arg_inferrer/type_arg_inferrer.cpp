#include "compiler/semantic/generics/type_arg_inferrer/type_arg_inferrer.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/functions/closure_checker/closure_checker.h"
#include "compiler/semantic/bodies/functions/function_value_checker/function_value_checker.h"
#include "compiler/semantic/bodies/iterators/iterable_checker/iterable_checker.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
#include "compiler/semantic/generics/arg_list_checker/arg_list_checker.h"
#include "compiler/semantic/generics/generic_check_helpers/generic_check_helpers.h"
#include "compiler/semantic/generics/generic_checker/generic_checker.h"
#include "compiler/semantic/generics/generic_instantiator/generic_instantiator.h"
#include "compiler/semantic/generics/inference/inference.h"
#include "compiler/semantic/generics/type_params/type_params.h"
#include "compiler/semantic/infer/late_classes/late_classes.h"
#include "compiler/semantic/library/result/result_types/result_types.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/literal_types/literal_types.h"
#include "compiler/semantic/types/type_parts/type_routes.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "compiler/semantic/types/type_resolver/type_resolver.h"
#include "compiler/semantic/types/utility_types/utility_types.h"
#include "llvm/ADT/STLExtras.h"
#include <functional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

constexpr unsigned kAnyArgs = ~0u;
constexpr llvm::StringLiteral kIntSample = "int";
constexpr llvm::StringLiteral kListSample = "[int]";

const GlobalName kContextFunctions[] = {
    GlobalName::STRING,     GlobalName::NUMBER,
    GlobalName::PARSE_INT,  GlobalName::PARSE_FLOAT,
    GlobalName::BOOLEAN,    GlobalName::ENCODE_URI_COMPONENT,
    GlobalName::ENCODE_URI, GlobalName::DECODE_URI_COMPONENT,
    GlobalName::DECODE_URI,
};

const GlobalName kContextObjects[] = {
    GlobalName::MATH,
    GlobalName::NUMBER,
    GlobalName::STRING,
    GlobalName::CONSOLE,
};

}

TypeArgInferrer::TypeArgInferrer(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->typeParams = &all.typeParams;
  this->callChecker = &all.callChecker;
  this->exprChecker = &all.exprChecker;
  this->lateClasses = &all.lateClasses;
  this->resultTypes = &all.resultTypes;
  this->typeResolver = &all.typeResolver;
  this->utilityTypes = &all.utilityTypes;
  this->literalTypes = &all.literalTypes;
  this->typeRelations = &all.typeRelations;
  this->closureChecker = &all.closureChecker;
  this->genericChecker = &all.genericChecker;
  this->argumentChecker = &all.argumentChecker;
  this->iterableChecker = &all.iterableChecker;
  this->genericInstantiator = &all.genericInstantiator;
  this->functionValueChecker = &all.functionValueChecker;
}

Inference::TypeOf TypeArgInferrer::elementOfType() const {
  return std::bind_front(&IterableChecker::elementTypeOf, this->iterableChecker);
}

Inference::TypeOf TypeArgInferrer::asyncOfType() const {
  return std::bind_front(&IterableChecker::asyncIterableType, this->iterableChecker);
}

bool TypeArgInferrer::isGrowing(const Type *type, llvm::ArrayRef<const Type *> params) const {
  const Type *bare = type;
  while (bare->isOptional()) {
    bare = bare->inner();
  }
  if (this->isGrowingAt(bare, params, GrowthPlace::TOP)) {
    return true;
  }
  auto parts = TypeParts::collect(bare, kGrowsRoute);
  for (const Type *part : parts.getTypes()) {
    if (part != bare && this->isGrowingAt(part, params, GrowthPlace::INSIDE)) {
      return true;
    }
  }

  return false;
}

bool TypeArgInferrer::isGrowingAt(const Type *type, llvm::ArrayRef<const Type *> params,
                                  GrowthPlace place) const {
  if (type->isParam()) {
    return place == GrowthPlace::INSIDE && llvm::is_contained(params, type);
  }
  bool isHolder = type->isObject() || type->isIterator() || type->isDict();

  return isHolder && GenericCheckHelpers::mentionsAnyOf(type, params);
}

bool TypeArgInferrer::isContextSensitive(const Expr *arg) const {
  for (const Expr *leaf : ExprParts::literalLeaves(arg)) {
    if (this->isNeedingContext(leaf)) {
      return true;
    }
  }

  return false;
}

bool TypeArgInferrer::hasUntypedParam(const FunctionDecl *functionDecl) const {
  for (const Param &param : functionDecl->params) {
    if (!param.type.has_value()) {
      return true;
    }
  }

  return false;
}

bool TypeArgInferrer::isNeedingContext(const Expr *arg) const {
  auto *functionExpr = dyn_cast<FunctionExpr>(arg);
  auto *name = dyn_cast<NameExpr>(arg);
  auto *member = dyn_cast<MemberExpr>(arg);
  if (functionExpr != nullptr) {
    return this->hasUntypedParam(functionExpr->fn);
  }
  if (name != nullptr) {
    auto global = GlobalNameTable::find(name->name);
    bool isContextFunction = global.has_value() && llvm::is_contained(kContextFunctions, *global);

    return name->typeArgs.empty() && isContextFunction;
  }
  auto *object = member != nullptr ? dyn_cast<NameExpr>(member->object) : nullptr;
  if (object != nullptr) {
    auto global = GlobalNameTable::find(object->name);

    return global.has_value() && llvm::is_contained(kContextObjects, *global);
  }

  return false;
}

bool TypeArgInferrer::hasSpecialProperty(const ObjectLitExpr *literal) const {
  for (const ObjectLitExpr::Property &property : literal->props) {
    if (property.method() || property.key() || property.defaulted()) {
      return true;
    }
  }

  return false;
}

bool TypeArgInferrer::isPrecheckable(const Field *field, const Expr *value,
                                     llvm::ArrayRef<const Type *> genericTypeParams) const {
  if (field == nullptr || !GenericCheckHelpers::mentionsAnyOf(field->type, genericTypeParams)) {
    return false;
  }
  if (this->isContextSensitive(value) || isa<ObjectLitExpr>(value) || isa<ArrayLitExpr>(value)) {
    return false;
  }

  return this->genericChecker->literalShape(value) == nullptr;
}

void TypeArgInferrer::precheckFields(ObjectLitExpr *literal, const Type *pattern,
                                     llvm::ArrayRef<const Type *> genericTypeParams,
                                     Inference &inference) {
  const Type *target = pattern->nonOptional();
  bool isPlainObject = target->isObject() && !target->isClass() && !target->isTuple();
  if (!isPlainObject || literal->hasSpread() || this->hasSpecialProperty(literal)) {
    return;
  }
  for (ObjectLitExpr::Property &property : literal->props) {
    const auto *field = target->field(property.name);
    if (this->isPrecheckable(field, property.value, genericTypeParams)) {
      property.value = this->exprChecker->checkExpr(property.value);
      this->state.getExpressionMarks().markPrechecked(property.value);
      inference.from(field->type, property.value->type);
    }
  }
}

std::string TypeArgInferrer::sampleArgs(llvm::ArrayRef<const Type *> params) const {
  auto text = std::string();
  for (const Type *param : params) {
    if (!text.empty()) {
      text += ", ";
    }
    text += param->isListParam() ? kListSample.str() : kIntSample.str();
  }

  return text;
}

TypeArgInferrer::ResultParamInferred TypeArgInferrer::paramInferred(const FuncSymbol *generic,
                                                                    const Type *bound, size_t index,
                                                                    TypeArgUse use) const {
  llvm::ArrayRef<const Type *> params = generic->typeParams;
  if (bound == nullptr) {
    auto samples = this->sampleArgs(params);
    bool isCall = use == TypeArgUse::CALL;
    auto message =
        ErrorTextParamInferred::cannot(params[index]->name(), generic->name, isCall, samples);

    return err(TypeArgInferrerParamInferredError{
        .code = TypeArgInferrerParamInferredErrorCode::CANNOT_INFER,
        .message = message,
    });
  }

  return ok(bound);
}

TypeArgInferrer::ResultIterableMatch TypeArgInferrer::iterableFits(const FuncSymbol *generic,
                                                                   const Type *bound,
                                                                   const Type *given,
                                                                   const Type *wanted) const {
  bool isFitting = given == wanted || wanted->isError() || bound->isError();
  if (!isFitting && given != nullptr) {
    auto message = ErrorTextIterableFits::elements(bound->name(), given->name(), wanted->name(),
                                                   generic->name);

    return err(TypeArgInferrerIterableFitsError{
        .code = TypeArgInferrerIterableFitsErrorCode::WRONG_ELEMENTS,
        .message = message,
    });
  }
  if (!isFitting) {
    auto message = ErrorTextIterableFits::notIter(bound->name(), wanted->name(), generic->name);

    return err(TypeArgInferrerIterableFitsError{
        .code = TypeArgInferrerIterableFitsErrorCode::NOT_ITERABLE,
        .message = message,
    });
  }

  return ok(bound);
}

TypeArgInferrer::ResultConstraintMet
TypeArgInferrer::constraintMet(const FuncSymbol *generic, std::vector<const Type *> &bound,
                               size_t index) {
  llvm::ArrayRef<const Type *> params = generic->typeParams;
  const Type *param = params[index];
  if (!this->typeParams->isSatisfying(bound[index], param, params, bound)) {
    llvm::ArrayRef<TypeParamDecl> decls = generic->decl->typeParams;
    const TypeParamDecl *written = index < decls.size() ? &decls[index] : nullptr;
    auto constraint = GenericCheckHelpers::constraintSpelling(param, written);
    auto message = ErrorTextConstraintMet::unsatisfied(bound[index]->name(), constraint,
                                                       param->name(), generic->name);

    return err(TypeArgInferrerConstraintMetError{
        .code = TypeArgInferrerConstraintMetErrorCode::CONSTRAINT_UNSATISFIED,
        .message = message,
    });
  }

  return ok(bound[index]);
}

TypeArgInferrer::ResultTypeArgsCount
TypeArgInferrer::typeArgCount(const FuncSymbol *generic, llvm::ArrayRef<TypeRef> typeArgs) const {
  if (typeArgs.size() > generic->typeParams.size()) {
    auto message =
        ErrorTextTypeArgCount::tooMany(generic->name, generic->typeParams.size(), typeArgs.size());

    return err(TypeArgInferrerTypeArgCountError{
        .code = TypeArgInferrerTypeArgCountErrorCode::TOO_MANY_TYPE_ARGUMENTS,
        .message = message,
    });
  }

  return ok(typeArgs.size());
}

TypeArgInferrer::ResultCandidateJoin TypeArgInferrer::candidatesJoin(const FuncSymbol *generic,
                                                                     const Type *param,
                                                                     const Type *type,
                                                                     const Type *candidate) const {
  const Type *next = type != nullptr ? this->typeRelations->unify(type, candidate) : candidate;
  if (next == nullptr) {
    auto message = ErrorTextCandidates::conflict(param->name(), generic->name, type->name(),
                                                 candidate->name());

    return err(TypeArgInferrerCandidatesJoinError{
        .code = TypeArgInferrerCandidatesJoinErrorCode::CONFLICTING_CANDIDATES,
        .message = message,
    });
  }

  return ok(next);
}

TypeArgInferrer::ResultCallbackParam
TypeArgInferrer::callbackParam(const FuncSymbol *generic, const Type *param, const Type *bound,
                               bool isMentioned, const FunctionExpr *functionExpr,
                               size_t position) const {
  if (bound == nullptr && isMentioned) {
    bool isNamed = functionExpr != nullptr && position < functionExpr->fn->params.size();
    std::string what = isNamed
                           ? ErrorTextCallbackParam::param(functionExpr->fn->params[position].name)
                           : ErrorTextCallbackParam::params();
    auto message = ErrorTextCallbackParam::uninferred(param->name(), generic->name, what,
                                                      functionExpr != nullptr);

    return err(TypeArgInferrerCallbackParamError{
        .code = TypeArgInferrerCallbackParamErrorCode::CALLBACK_PARAM_UNINFERRED,
        .message = message,
    });
  }

  return ok(param);
}

TypeArgInferrer::ResultArgNotGrowing
TypeArgInferrer::notGrowing(const FuncSymbol *generic, const Type *bound, const Type *param) const {
  if (this->isGrowing(bound, generic->typeParams)) {
    auto message = ErrorTextNotGrowing::grows(generic->name, bound->name(), param->name());

    return err(TypeArgInferrerNotGrowingError{
        .code = TypeArgInferrerNotGrowingErrorCode::GROWING_TYPE_ARGUMENT,
        .message = message,
    });
  }

  return ok(bound);
}

bool TypeArgInferrer::finishTypeArgs(FuncSymbol *generic, std::vector<const Type *> &bound,
                                     llvm::SMLoc loc, TypeArgUse use) {
  return this->finishTypeArgsWith(generic, bound, loc, use, MissingTypeArgs::REPORTED);
}

bool TypeArgInferrer::finishTypeArgsWith(FuncSymbol *generic, std::vector<const Type *> &bound,
                                         llvm::SMLoc loc, TypeArgUse use, MissingTypeArgs missing) {
  bool isInferred = true;
  for (size_t index = 0; index < generic->typeParams.size(); ++index) {
    this->applyTypeDefault(generic, bound, index);
    bool isKnown = this->finishTypeArg(generic, bound, index, loc, use, missing);
    if (!isKnown) {
      isInferred = false;
    }
  }

  return isInferred;
}

void TypeArgInferrer::applyTypeDefault(const FuncSymbol *generic, std::vector<const Type *> &bound,
                                       size_t index) {
  bool hasDefault = index < generic->typeDefaults.size() && generic->typeDefaults[index] != nullptr;
  if (bound[index] != nullptr || !hasDefault) {
    return;
  }
  llvm::ArrayRef<const Type *> params = generic->typeParams;
  const auto *fallback = this->typeParams->substitute(generic->typeDefaults[index], params, bound);
  if (!this->isMentioningOpen(fallback, params, bound)) {
    bound[index] = fallback;
  }
}

bool TypeArgInferrer::isMentioningOpen(const Type *type, llvm::ArrayRef<const Type *> params,
                                       llvm::ArrayRef<const Type *> bound) const {
  for (size_t index = 0; index < params.size(); ++index) {
    if (bound[index] == nullptr && GenericCheckHelpers::mentionsAnyOf(type, params[index])) {
      return true;
    }
  }

  return false;
}

bool TypeArgInferrer::finishTypeArg(FuncSymbol *generic, std::vector<const Type *> &bound,
                                    size_t index, llvm::SMLoc loc, TypeArgUse use,
                                    MissingTypeArgs missing) {
  if (bound[index] != nullptr && bound[index]->isError()) {
    return false;
  }
  if (bound[index] == nullptr && missing == MissingTypeArgs::SILENT) {
    bound[index] = Type::error();

    return false;
  }
  auto paramKnown = this->paramInferred(generic, bound[index], index, use);
  if (paramKnown.isErr()) {
    this->state.getDiagnostics().error(loc, paramKnown.error().message);
    bound[index] = Type::error();

    return false;
  }
  const Type *element = generic->typeParams[index]->iterated();
  if (element != nullptr) {
    return this->fitIteratedArg(generic, bound, index, element, loc);
  }
  auto met = this->constraintMet(generic, bound, index);
  if (met.isErr()) {
    this->state.getDiagnostics().error(loc, met.error().message);
    bound[index] = Type::error();

    return false;
  }

  return true;
}

bool TypeArgInferrer::fitIteratedArg(const FuncSymbol *generic, std::vector<const Type *> &bound,
                                     size_t index, const Type *element, llvm::SMLoc loc) {
  const auto *wanted = this->typeParams->substitute(element, generic->typeParams, bound);
  const auto *elements = this->iterableChecker->elementTypeOf(bound[index]);
  auto matched = this->iterableFits(generic, bound[index], elements, wanted);
  if (matched.isErr()) {
    this->state.getDiagnostics().error(loc, matched.error().message);
    bound[index] = Type::error();

    return false;
  }

  return true;
}

bool TypeArgInferrer::isConcreteCall(const FuncSymbol *generic,
                                     llvm::ArrayRef<const Type *> bound) const {
  for (const Type *type : bound) {
    if (TypeQueries::mentionsParam(type)) {
      return false;
    }
  }
  const ClassDecl *classDecl = generic->decl != nullptr ? generic->decl->cls() : nullptr;

  return classDecl == nullptr || generic->decl->isStatic() || classDecl->isEmitted();
}

Expr *TypeArgInferrer::checkGenericCall(CallExpr *call, FuncSymbol *generic, llvm::SMLoc loc,
                                        const Type *expected, llvm::ArrayRef<TypeRef> typeArgs) {
  auto bound = std::vector<const Type *>();
  if (!this->bindGenericCall(call, generic, loc, expected, typeArgs, bound)) {
    return call;
  }
  call->target = FunctionCall{.func = generic};
  bool isCopyingReturn = this->lateClasses->isTemplate(generic) && !generic->hasReturnType();
  bool isInstantiable = this->isConcreteCall(generic, bound) &&
                        !this->state.getFunctionStack().isInsideTrial() && !generic->inferring &&
                        (generic->hasReturnType() || isCopyingReturn);
  if (!isInstantiable) {
    return call;
  }
  auto exact = this->genericInstantiator->exactClassArgs(call, generic);
  auto *instance = this->genericInstantiator->instantiate(generic, bound, loc, exact);
  if (instance == nullptr) {
    return call;
  }
  call->target = FunctionCall{.func = instance};
  if (isCopyingReturn) {
    call->type = instance->hasReturnType() ? instance->getKnownReturnType() : Type::error();
  }

  return call;
}

bool TypeArgInferrer::bindGenericCall(CallExpr *call, FuncSymbol *generic, llvm::SMLoc loc,
                                      const Type *expected, llvm::ArrayRef<TypeRef> typeArgs,
                                      std::vector<const Type *> &bound) {
  call->type = Type::error();
  const auto *result = generic->getReturnType();
  bool isOwnClass = this->genericInstantiator->returnsOwnClass(generic, result);
  GenericCallBinding binding{
      .call = call,
      .generic = generic,
      .loc = loc,
      .typeParams = generic->typeParams,
      .result = isOwnClass ? nullptr : result,
      .listRest = this->listRestOf(generic),
      .fixed = generic->params.size() - (generic->hasRest ? 1 : 0),
      .errorsBefore = this->state.getDiagnostics().getWrittenErrorCount(),
      .bound = bound,
  };
  if (this->hasRejectedSpread(binding)) {
    return false;
  }
  auto minArgs = static_cast<unsigned>(generic->minArgs);
  unsigned maxArgs = generic->hasRest ? kAnyArgs : static_cast<unsigned>(generic->params.size());
  auto arity = this->argumentChecker->checkArity(call, minArgs, maxArgs);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);

    return false;
  }
  bound.assign(binding.typeParams.size(), nullptr);
  binding.checked.assign(call->args.size(), false);
  if (typeArgs.empty()) {
    this->inferTypeArgs(binding, expected);
  } else {
    this->bindExplicitTypeArgs(binding, typeArgs);
  }
  this->finishBinding(binding);
  if (!binding.inferred) {
    this->walkUncheckedArguments(binding);

    return false;
  }
  auto checks = typeArgs.empty() ? ArgumentChecks::KEEP_CHECKED : ArgumentChecks::CHECK_ALL;

  return this->instantiateCall(binding, checks);
}

const Type *TypeArgInferrer::listRestOf(const FuncSymbol *generic) const {
  if (!generic->hasRest) {
    return nullptr;
  }
  const Type *rest = generic->params.back();
  bool isList = rest->isListParam() || ArgListChecker::isSpreadingLists(rest);

  return isList ? rest : nullptr;
}

const Type *TypeArgInferrer::parameterFor(const GenericCallBinding &binding, size_t index) const {
  const FuncSymbol *generic = binding.generic;
  if (!generic->hasRest || index < binding.fixed) {
    return generic->params[index];
  }
  const Type *rest = generic->params.back();
  bool isElement = !binding.call->spreads(index) && rest->isArray();

  return isElement ? rest->inner() : rest;
}

bool TypeArgInferrer::hasRejectedSpread(const GenericCallBinding &binding) {
  CallExpr *call = binding.call;
  size_t limit = binding.generic->hasRest ? binding.fixed : call->args.size();
  size_t count = std::min(call->args.size(), limit);
  for (size_t index = 0; index < count; ++index) {
    if (call->spreads(index) && this->callChecker->rejectSpread(call)) {
      return true;
    }
  }

  return false;
}

void TypeArgInferrer::bindExplicitTypeArgs(GenericCallBinding &binding,
                                           llvm::ArrayRef<TypeRef> typeArgs) {
  auto counted = this->typeArgCount(binding.generic, typeArgs);
  if (counted.isErr()) {
    this->state.getDiagnostics().error(binding.loc, counted.error().message);
    binding.inferred = false;
  }
  size_t count = std::min(typeArgs.size(), binding.typeParams.size());
  for (size_t index = 0; index < count; ++index) {
    binding.bound[index] = this->typeResolver->resolveTypeOrVoid(typeArgs[index]);
  }
}

void TypeArgInferrer::inferTypeArgs(GenericCallBinding &binding, const Type *expected) {
  Inference inference(binding.typeParams, this->state.getAliasStore(), this->elementOfType());
  inference.setAsyncOf(this->asyncOfType());
  for (size_t index = 0; index < binding.call->args.size(); ++index) {
    this->walkArgument(binding, inference, index);
  }
  this->inferFromListRest(binding, inference);
  this->settleWithCandidates(binding, inference);
  if (inference.hasDeferred()) {
    inference.retry(binding.bound);
    this->settleWithCandidates(binding, inference);
  }
  this->inferFromExpected(binding, expected);
  for (size_t index = 0; index < binding.typeParams.size(); ++index) {
    this->settleTypeParam(binding, inference, index);
  }
  for (size_t index : binding.contextual) {
    this->walkContextualArgument(binding, inference, index);
  }
}

void TypeArgInferrer::walkArgument(GenericCallBinding &binding, Inference &inference,
                                   size_t index) {
  auto form = this->argumentForm(binding, index);
  switch (form.kind()) {
    case GenericArgumentKind::LIST_REST:
      return this->walkArgumentNow(binding, index, nullptr);
    case GenericArgumentKind::PLAIN:
      return this->walkArgumentNow(binding, index, this->parameterFor(binding, index));
    case GenericArgumentKind::LITERAL_TEXT:
      return this->inferFromLiteralText(binding, inference, index,
                                        form.as<LiteralTextArgument>()->text);
    case GenericArgumentKind::CONTEXTUAL:
      return binding.contextual.push_back(index);
    case GenericArgumentKind::LITERAL_SHAPE:
      return this->inferFromLiteralShape(binding, inference, index,
                                         form.as<LiteralShapeArgument>()->shape);
    case GenericArgumentKind::CONTEXTUAL_LITERAL:
      return this->deferLiteral(binding, inference, index);
    case GenericArgumentKind::INFERRED:
      return this->inferFromCheckedArgument(binding, inference, index);
  }
}

GenericArgumentForm TypeArgInferrer::argumentForm(const GenericCallBinding &binding, size_t index) {
  Expr *arg = binding.call->args[index];
  const auto *pattern = this->parameterFor(binding, index);
  if (binding.listRest != nullptr && index >= binding.fixed) {
    return ListRestArgument{};
  }
  if (!GenericCheckHelpers::mentionsAnyOf(pattern, binding.typeParams)) {
    return PlainArgument{};
  }
  auto *text = dyn_cast<StringLitExpr>(arg);
  bool isLiteralText = text != nullptr && llvm::is_contained(binding.typeParams, pattern) &&
                       this->utilityTypes->isExpectingLiteral(pattern);
  if (isLiteralText) {
    return LiteralTextArgument{.text = text};
  }
  bool isContextual =
      this->isContextSensitive(arg) || this->isMakingResults(arg) || this->isBareGenericName(arg);
  if (isContextual) {
    return ContextualArgument{};
  }
  const auto *shape = this->genericChecker->literalShape(arg);
  if (shape != nullptr) {
    return LiteralShapeArgument{.shape = shape};
  }
  if (isa<ArrayLitExpr>(arg) || isa<ObjectLitExpr>(arg)) {
    return ContextualLiteralArgument{};
  }

  return InferredArgument{};
}

bool TypeArgInferrer::isMakingResults(const Expr *arg) const {
  auto *name = dyn_cast<NameExpr>(arg);
  if (name == nullptr) {
    return false;
  }
  auto which = this->resultTypes->resultName(name->name);

  return which == GlobalName::OK || which == GlobalName::ERR;
}

bool TypeArgInferrer::isBareGenericName(Expr *arg) const {
  auto *name = dyn_cast<NameExpr>(arg);
  if (name == nullptr || !name->typeArgs.empty()) {
    return false;
  }

  return this->genericChecker->genericNamed(name) != nullptr;
}

void TypeArgInferrer::walkArgumentNow(GenericCallBinding &binding, size_t index, const Type *hint) {
  Expr *&arg = binding.call->args[index];
  arg = this->exprChecker->checkExpr(arg, hint);
  binding.checked[index] = true;
}

void TypeArgInferrer::inferFromLiteralText(GenericCallBinding &binding, Inference &inference,
                                           size_t index, const StringLitExpr *text) {
  const auto *literal = Type::stringLiteral(text->value);
  this->walkArgumentNow(binding, index, literal);
  const auto *pattern = this->parameterFor(binding, index);
  inference.from(pattern, literal);
}

void TypeArgInferrer::inferFromLiteralShape(GenericCallBinding &binding, Inference &inference,
                                            size_t index, const Type *shape) {
  const Expr *arg = binding.call->args[index];
  const auto *pattern = this->parameterFor(binding, index);
  binding.literals.push_back(index);
  bool isIntLiteral = shape->isInteger() && this->typeRelations->isIntLiteralTree(arg);
  inference.markArgument(ArgumentStrength::WEAK, isIntLiteral);
  inference.from(pattern, shape);
  inference.forgetArgumentMarks();
}

void TypeArgInferrer::deferLiteral(GenericCallBinding &binding, Inference &inference,
                                   size_t index) {
  binding.contextual.push_back(index);
  auto *object = dyn_cast<ObjectLitExpr>(binding.call->args[index]);
  if (object != nullptr) {
    const auto *pattern = this->parameterFor(binding, index);
    this->precheckFields(object, pattern, binding.typeParams, inference);
  }
}

void TypeArgInferrer::inferFromCheckedArgument(GenericCallBinding &binding, Inference &inference,
                                               size_t index) {
  this->walkArgumentNow(binding, index, nullptr);
  const Expr *arg = binding.call->args[index];
  const auto *pattern = this->parameterFor(binding, index);
  bool isIntLiteral = arg->type->isInteger() && this->typeRelations->isIntLiteralTree(arg);
  inference.markArgument(ArgumentStrength::STRONG, isIntLiteral);
  inference.from(pattern, arg->type);
  inference.forgetArgumentMarks();
}

void TypeArgInferrer::inferFromListRest(GenericCallBinding &binding, Inference &inference) {
  if (binding.listRest == nullptr) {
    return;
  }
  CallExpr *call = binding.call;
  auto parts = std::vector<const Type *>();
  auto spread = std::vector<bool>();
  for (size_t index = binding.fixed; index < call->args.size(); ++index) {
    parts.push_back(call->args[index]->type);
    spread.push_back(call->spreads(index));
  }
  auto why = std::string();
  const auto *list = ArgumentLists::spreadTuple(parts, spread, why);
  if (list != nullptr) {
    inference.from(binding.listRest, list);
  }
}

void TypeArgInferrer::settleWithCandidates(GenericCallBinding &binding, Inference &inference) {
  for (size_t index = 0; index < binding.typeParams.size(); ++index) {
    if (!inference.getCandidates(index).empty()) {
      this->settleTypeParam(binding, inference, index);
    }
  }
}

void TypeArgInferrer::settleTypeParam(GenericCallBinding &binding, Inference &inference,
                                      size_t index) {
  if (binding.bound[index] != nullptr) {
    return;
  }
  auto joined = this->joinCandidates(binding, inference, index);
  if (joined.isErr()) {
    this->state.getDiagnostics().error(binding.loc, joined.error().message);
    binding.bound[index] = Type::error();
    binding.inferred = false;
    return;
  }
  const auto *widened = this->widenToConstraint(binding, inference, index, joined.value());
  binding.bound[index] = this->withUndefined(inference, index, widened);
}

TypeArgInferrer::ResultCandidateType
TypeArgInferrer::joinCandidates(const GenericCallBinding &binding, const Inference &inference,
                                size_t index) {
  auto strong = this->joinStrongCandidates(binding, inference, index);
  if (strong.isErr()) {
    return err(strong.error());
  }
  auto weak = this->joinWeakCandidates(binding, inference, index, strong.value());
  if (weak.isErr()) {
    return err(weak.error());
  }

  return this->joinLiteralCandidates(binding, inference, index, weak.value());
}

TypeArgInferrer::ResultCandidateType
TypeArgInferrer::joinStrongCandidates(const GenericCallBinding &binding, const Inference &inference,
                                      size_t index) const {
  const Type *param = binding.typeParams[index];
  const Type *type = nullptr;
  for (const Type *candidate : inference.getCandidates(index)) {
    auto joined = this->candidatesJoin(binding.generic, param, type, candidate);
    if (joined.isErr()) {
      return err(TypeArgInferrerJoinCandidatesError{
          .code = TypeArgInferrerJoinCandidatesErrorCode::CANDIDATES_CLASH,
          .message = joined.error().message,
      });
    }
    type = joined.value();
  }

  return ok(type);
}

TypeArgInferrer::ResultCandidateType
TypeArgInferrer::joinWeakCandidates(const GenericCallBinding &binding, const Inference &inference,
                                    size_t index, const Type *strong) const {
  const Type *param = binding.typeParams[index];
  bool isStrong = !inference.getCandidates(index).empty();
  const Type *type = strong;
  for (const Type *candidate : inference.getWeakCandidates(index)) {
    auto joined = this->candidatesJoin(binding.generic, param, type, candidate);
    if (joined.isErr() && !isStrong) {
      return err(TypeArgInferrerJoinCandidatesError{
          .code = TypeArgInferrerJoinCandidatesErrorCode::CANDIDATES_CLASH,
          .message = joined.error().message,
      });
    }
    if (joined.isOk()) {
      type = joined.value();
    }
  }

  return ok(type);
}

bool TypeArgInferrer::isOtherNumber(const Type *type, const Type *candidate) const {
  if (type == nullptr || type == candidate) {
    return false;
  }

  return type->isNumeric() || type->isDecimal();
}

TypeArgInferrer::ResultCandidateType
TypeArgInferrer::joinLiteralCandidates(const GenericCallBinding &binding,
                                       const Inference &inference, size_t index,
                                       const Type *known) const {
  const Type *param = binding.typeParams[index];
  const Type *type = known;
  for (const Type *candidate : inference.getLiteralCandidates(index)) {
    if (!this->isOtherNumber(type, candidate)) {
      auto joined = this->candidatesJoin(binding.generic, param, type, candidate);
      if (joined.isErr()) {
        return err(TypeArgInferrerJoinCandidatesError{
            .code = TypeArgInferrerJoinCandidatesErrorCode::CANDIDATES_CLASH,
            .message = joined.error().message,
        });
      }
      type = joined.value();
    }
  }

  return ok(type);
}

const Type *TypeArgInferrer::widenToConstraint(const GenericCallBinding &binding,
                                               const Inference &inference, size_t index,
                                               const Type *type) const {
  const Type *constraint = binding.typeParams[index]->constraint();
  if (constraint != nullptr && constraint->isUnion()) {
    constraint = this->literalTypes->numberMemberOf(constraint, NumberSpelling::WHOLE);
  }
  bool isOnlyLiterals =
      inference.getCandidates(index).empty() && inference.getWeakCandidates(index).empty();
  bool isFractional = constraint != nullptr && (constraint->isFloat() || constraint->isDecimal());
  bool isWidening = type != nullptr && type->isInteger() && isOnlyLiterals && isFractional;

  return isWidening ? constraint : type;
}

const Type *TypeArgInferrer::withUndefined(const Inference &inference, size_t index,
                                           const Type *type) const {
  if (!inference.hasSeenUndefined(index)) {
    return type;
  }

  return type != nullptr ? Type::optional(type) : Type::voidTy();
}

void TypeArgInferrer::inferFromExpected(GenericCallBinding &binding, const Type *expected) {
  const Type *result = binding.result;
  bool isOpen = llvm::is_contained(binding.bound, nullptr);
  bool isUseful = expected != nullptr && isOpen && result != nullptr &&
                  GenericCheckHelpers::mentionsAnyOf(result, binding.typeParams);
  if (!isUseful) {
    return;
  }
  Inference fromExpected(binding.typeParams, this->state.getAliasStore(), nullptr);
  fromExpected.from(result, expected);
  for (size_t index = 0; index < binding.typeParams.size(); ++index) {
    bool hasCandidates = !fromExpected.getCandidates(index).empty();
    if (binding.bound[index] == nullptr && hasCandidates) {
      binding.bound[index] = this->expectedCandidate(fromExpected, index);
    }
  }
}

const Type *TypeArgInferrer::expectedCandidate(const Inference &fromExpected, size_t index) const {
  const Type *type = nullptr;
  for (const Type *candidate : fromExpected.getCandidates(index)) {
    type = type != nullptr ? this->typeRelations->unify(type, candidate) : candidate;
  }

  return type;
}

void TypeArgInferrer::walkContextualArgument(GenericCallBinding &binding, Inference &inference,
                                             size_t index) {
  const auto *pattern = this->parameterFor(binding, index);
  const auto *partial = this->typeParams->substitute(pattern, binding.typeParams, binding.bound);
  const Type *function = partial->nonOptional();
  if (function->isFunction()) {
    this->walkCallbackArgument(binding, index, function);
  } else {
    this->walkArgumentWithDefaults(binding, index, pattern, partial);
  }
  binding.checked[index] = true;
  this->adoptArgumentType(binding, inference, index, pattern);
}

void TypeArgInferrer::walkCallbackArgument(GenericCallBinding &binding, size_t index,
                                           const Type *function) {
  Expr *&arg = binding.call->args[index];
  auto signature = this->callbackSignature(binding.typeParams, function);
  auto *name = dyn_cast<NameExpr>(arg);
  FuncSymbol *other = name != nullptr ? this->genericChecker->genericNamed(name) : nullptr;
  if (other == nullptr) {
    this->walkCallbackParams(binding, index, function);
  }
  arg = this->checkCallbackValue(arg, other, signature);
}

Expr *TypeArgInferrer::checkCallbackValue(Expr *arg, FuncSymbol *other, Signature &signature) {
  auto *closure = dyn_cast<FunctionExpr>(arg);
  if (closure != nullptr) {
    return this->closureChecker->checkFunctionExpr(closure, &signature);
  }
  if (other != nullptr) {
    auto *name = cast<NameExpr>(arg);

    return this->genericInstantiator->checkGenericValue(name, other, &signature);
  }
  auto arity = static_cast<unsigned>(signature.params.size());
  auto *wrapper = this->functionValueChecker->builtinFunctionValue(arg, signature, arity, {});
  if (wrapper != nullptr) {
    return this->closureChecker->checkFunctionExpr(wrapper, &signature);
  }

  return this->exprChecker->checkExpr(arg);
}

Signature TypeArgInferrer::callbackSignature(llvm::ArrayRef<const Type *> openParams,
                                             const Type *function) const {
  Signature signature;
  for (const Type *param : function->params()) {
    bool isOpen = GenericCheckHelpers::mentionsAnyOf(param, openParams);
    signature.params.push_back(isOpen ? Type::error() : param);
  }
  bool isOpenResult = GenericCheckHelpers::mentionsAnyOf(function->ret(), openParams);
  signature.minArgs = function->minArgs();
  signature.ret = isOpenResult ? nullptr : function->ret();
  signature.voidResult = function->ret()->isVoid();

  return signature;
}

void TypeArgInferrer::walkCallbackParams(GenericCallBinding &binding, size_t index,
                                         const Type *function) {
  auto *closure = dyn_cast<FunctionExpr>(binding.call->args[index]);
  for (size_t position = 0; position < function->params().size(); ++position) {
    bool isWritten = closure != nullptr && position < closure->fn->params.size() &&
                     closure->fn->params[position].type.has_value();
    if (!isWritten) {
      this->walkCallbackParam(binding, index, function->params()[position], position);
    }
  }
}

void TypeArgInferrer::walkCallbackParam(GenericCallBinding &binding, size_t index,
                                        const Type *callbackParam, size_t position) {
  const Expr *arg = binding.call->args[index];
  auto *closure = dyn_cast<FunctionExpr>(arg);
  for (size_t typeIndex = 0; typeIndex < binding.typeParams.size(); ++typeIndex) {
    const Type *param = binding.typeParams[typeIndex];
    bool isMentioned = GenericCheckHelpers::mentionsAnyOf(callbackParam, param);
    auto known = this->callbackParam(binding.generic, param, binding.bound[typeIndex], isMentioned,
                                     closure, position);
    if (known.isErr()) {
      this->state.getDiagnostics().error(arg->loc(), known.error().message);
      binding.bound[typeIndex] = Type::error();
      binding.inferred = false;
    }
  }
}

void TypeArgInferrer::walkArgumentWithDefaults(GenericCallBinding &binding, size_t index,
                                               const Type *pattern, const Type *partial) {
  llvm::ArrayRef<const Type *> params = binding.typeParams;
  bool isOpen = GenericCheckHelpers::mentionsAnyOf(partial, params);
  bool isDefaulted = isOpen && this->fillTypeDefaults(binding);
  const Type *hint =
      isDefaulted ? this->typeParams->substitute(pattern, params, binding.bound) : partial;
  bool isStillOpen = GenericCheckHelpers::mentionsAnyOf(hint, params);
  this->walkArgumentNow(binding, index, isStillOpen ? nullptr : hint);
}

bool TypeArgInferrer::fillTypeDefaults(GenericCallBinding &binding) {
  bool isDefaulted = false;
  for (size_t index = 0; index < binding.typeParams.size(); ++index) {
    const auto *fallback = this->typeDefault(binding.generic, index);
    if (binding.bound[index] == nullptr && fallback != nullptr) {
      binding.bound[index] = fallback;
      isDefaulted = true;
    }
  }

  return isDefaulted;
}

const Type *TypeArgInferrer::typeDefault(const FuncSymbol *generic, size_t index) const {
  bool hasDefault = index < generic->typeDefaults.size() && generic->typeDefaults[index] != nullptr;
  if (!hasDefault) {
    return nullptr;
  }
  const Type *fallback = generic->typeDefaults[index];
  bool isClosed = !GenericCheckHelpers::mentionsAnyOf(fallback, generic->typeParams);

  return isClosed ? fallback : nullptr;
}

void TypeArgInferrer::adoptArgumentType(GenericCallBinding &binding, Inference &inference,
                                        size_t index, const Type *pattern) {
  Inference more(binding.typeParams, this->state.getAliasStore(), this->elementOfType());
  more.from(pattern, binding.call->args[index]->type);
  for (size_t typeIndex = 0; typeIndex < binding.typeParams.size(); ++typeIndex) {
    if (binding.bound[typeIndex] == nullptr) {
      inference.adoptCandidates(typeIndex, more);
      this->settleTypeParam(binding, inference, typeIndex);
    }
  }
}

void TypeArgInferrer::finishBinding(GenericCallBinding &binding) {
  auto missing =
      this->hasArgumentErrors(binding) ? MissingTypeArgs::SILENT : MissingTypeArgs::REPORTED;
  bool isFinished = this->finishTypeArgsWith(binding.generic, binding.bound, binding.loc,
                                             TypeArgUse::CALL, missing);
  binding.inferred = isFinished && binding.inferred;
  for (size_t index = 0; index < binding.typeParams.size() && binding.inferred; ++index) {
    auto growing =
        this->notGrowing(binding.generic, binding.bound[index], binding.typeParams[index]);
    if (growing.isErr()) {
      this->state.getDiagnostics().error(binding.loc, growing.error().message);
      binding.inferred = false;
    }
  }
}

bool TypeArgInferrer::hasArgumentErrors(const GenericCallBinding &binding) const {
  bool isReported = this->state.getDiagnostics().getWrittenErrorCount() > binding.errorsBefore;
  if (!isReported) {
    return false;
  }
  for (size_t index = 0; index < binding.call->args.size(); ++index) {
    if (binding.checked[index] && binding.call->args[index]->type->isError()) {
      return true;
    }
  }

  return false;
}

void TypeArgInferrer::walkUncheckedArguments(GenericCallBinding &binding) {
  for (size_t index = 0; index < binding.call->args.size(); ++index) {
    if (!binding.checked[index]) {
      this->walkArgumentNow(binding, index, nullptr);
    }
  }
}

bool TypeArgInferrer::instantiateCall(GenericCallBinding &binding, ArgumentChecks checks) {
  CallExpr *call = binding.call;
  const FuncSymbol *generic = binding.generic;
  this->state.getGenericStore().enterInstanceLoc(binding.loc);
  auto params = std::vector<const Type *>();
  for (const Type *param : generic->params) {
    const auto *bare = this->typeParams->substituteValue(param, binding.typeParams, binding.bound);
    params.push_back(bare);
  }
  for (size_t index : binding.literals) {
    const auto *pattern = this->parameterFor(binding, index);
    const auto *hint =
        this->typeParams->substituteValue(pattern, binding.typeParams, binding.bound);
    this->walkArgumentNow(binding, index, hint);
  }
  const Type *result = binding.result;
  call->type = result != nullptr
                   ? this->typeParams->substitute(result, binding.typeParams, binding.bound)
                   : Type::error();
  this->state.getGenericStore().leaveInstanceLoc();
  size_t minArgs = generic->minArgs;
  bool hasRest = generic->hasRest;
  auto flat = std::vector<const Type *>();
  if (ArgumentLists::flattenArguments(params, minArgs, hasRest, flat)) {
    params = std::move(flat);
  }
  std::vector<bool> *checked = checks == ArgumentChecks::KEEP_CHECKED ? &binding.checked : nullptr;
  this->argumentChecker->checkArguments(call, params, minArgs, hasRest, binding.loc, checked);

  return result != nullptr;
}

const Type *TypeArgInferrer::argumentPattern(const FuncSymbol *generic, size_t index) const {
  size_t fixed = generic->params.size() - (generic->hasRest ? 1 : 0);
  if (index < fixed) {
    return generic->params[index];
  }
  if (generic->hasRest && generic->params.back()->isArray()) {
    return generic->params.back()->inner();
  }

  return nullptr;
}

bool TypeArgInferrer::inferFromArguments(FuncSymbol *generic, llvm::ArrayRef<Expr *> args,
                                         std::vector<const Type *> &bound) {
  llvm::ArrayRef<const Type *> genericTypeParams = generic->typeParams;
  Inference inference(genericTypeParams, this->state.getAliasStore(), this->elementOfType());
  inference.setAsyncOf(this->asyncOfType());
  for (size_t index = 0; index < args.size(); ++index) {
    const auto *pattern = this->argumentPattern(generic, index);
    if (pattern != nullptr && args[index]->type != nullptr) {
      inference.from(pattern, args[index]->type);
    }
  }
  bound.assign(genericTypeParams.size(), nullptr);
  for (size_t index = 0; index < genericTypeParams.size(); ++index) {
    auto candidate = this->argumentCandidate(inference, index);
    if (!candidate.has_value()) {
      return false;
    }
    bound[index] = *candidate;
  }
  for (size_t index = 0; index < genericTypeParams.size(); ++index) {
    if (!this->isFittingArgument(generic, bound, index)) {
      return false;
    }
  }

  return true;
}

std::optional<const Type *> TypeArgInferrer::argumentCandidate(const Inference &inference,
                                                               size_t index) const {
  const Type *type = nullptr;
  for (const Type *candidate : inference.getCandidates(index)) {
    type = type != nullptr ? this->typeRelations->unify(type, candidate) : candidate;
    if (type == nullptr) {
      return std::nullopt;
    }
  }
  if (!inference.hasSeenUndefined(index)) {
    return type;
  }

  return type != nullptr ? Type::optional(type) : Type::voidTy();
}

bool TypeArgInferrer::isFittingArgument(const FuncSymbol *generic, std::vector<const Type *> &bound,
                                        size_t index) {
  llvm::ArrayRef<const Type *> params = generic->typeParams;
  bool hasDefault = index < generic->typeDefaults.size() && generic->typeDefaults[index] != nullptr;
  if (bound[index] == nullptr && hasDefault) {
    bound[index] = this->typeParams->substitute(generic->typeDefaults[index], params, bound);
  }
  if (bound[index] == nullptr || GenericCheckHelpers::mentionsAnyOf(bound[index], params)) {
    return false;
  }

  return this->typeParams->isSatisfying(bound[index], params[index], params, bound);
}

}
