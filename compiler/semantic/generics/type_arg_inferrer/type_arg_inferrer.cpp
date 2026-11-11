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

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

TypeArgInferrer::TypeArgInferrer(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->typeParams = &all.typeParams;
  this->callChecker = &all.callChecker;
  this->exprChecker = &all.exprChecker;
  this->lateClasses = &all.lateClasses;
  this->resultTypes = &all.resultTypes;
  this->typeResolver = &all.typeResolver;
  this->utilityTypes = &all.utilityTypes;
  this->closureChecker = &all.closureChecker;
  this->genericChecker = &all.genericChecker;
  this->argumentChecker = &all.argumentChecker;
  this->iterableChecker = &all.iterableChecker;
  this->genericInstantiator = &all.genericInstantiator;
  this->functionValueChecker = &all.functionValueChecker;
}

bool TypeArgInferrer::grows(const Type *type, llvm::ArrayRef<const Type *> params, bool inside) {
  const Type *bare = type;
  while (bare->isOptional()) {
    bare = bare->inner();
  }
  if (TypeArgInferrer::growsHere(bare, params, inside)) {
    return true;
  }
  TypeParts parts = TypeParts::collect(bare, kGrowsRoute);
  for (const Type *part : parts.getTypes()) {
    if (part != bare && TypeArgInferrer::growsHere(part, params, true)) {
      return true;
    }
  }

  return false;
}

bool TypeArgInferrer::growsHere(const Type *type, llvm::ArrayRef<const Type *> params,
                                bool inside) {
  if (type->isParam()) {
    return inside && llvm::is_contained(params, type);
  }
  bool holder = type->isObject() || type->isIterator() || type->isDict();

  return holder && GenericCheckHelpers::mentionsAnyOf(type, params);
}

bool TypeArgInferrer::isContextSensitive(const Expr *arg) {
  for (const Expr *leaf : ExprParts::literalLeaves(arg)) {
    if (TypeArgInferrer::needsContext(leaf)) {
      return true;
    }
  }

  return false;
}

bool TypeArgInferrer::needsContext(const Expr *arg) {
  if (auto *fe = dyn_cast<FunctionExpr>(arg)) {
    return llvm::any_of(fe->fn->params, [](const Param &p) { return !p.type; });
  }
  if (auto *name = dyn_cast<NameExpr>(arg)) {
    static const GlobalName functions[] = {GlobalName::STRING,     GlobalName::NUMBER,
                                           GlobalName::PARSE_INT,  GlobalName::PARSE_FLOAT,
                                           GlobalName::BOOLEAN,    GlobalName::ENCODE_URI_COMPONENT,
                                           GlobalName::ENCODE_URI, GlobalName::DECODE_URI_COMPONENT,
                                           GlobalName::DECODE_URI};
    std::optional<GlobalName> global = GlobalNameTable::find(name->name);
    return name->typeArgs.empty() && global.has_value() && llvm::is_contained(functions, *global);
  }
  if (auto *member = dyn_cast<MemberExpr>(arg)) {
    if (auto *object = dyn_cast<NameExpr>(member->object)) {
      std::optional<GlobalName> global = GlobalNameTable::find(object->name);
      return global == GlobalName::MATH || global == GlobalName::NUMBER ||
             global == GlobalName::STRING || global == GlobalName::CONSOLE;
    }
  }

  return false;
}

void TypeArgInferrer::precheckFields(ObjectLitExpr *lit, const Type *pattern,
                                     llvm::ArrayRef<const Type *> genericTypeParams,
                                     Inference &inference) {
  const Type *target = pattern->nonOptional();
  if (!target->isObject() || target->isClass() || target->isTuple() || lit->hasSpread() ||
      llvm::any_of(lit->props, [](const ObjectLitExpr::Property &p) {
        return p.method() || p.key() || p.defaulted();
      })) {
    return;
  }
  for (ObjectLitExpr::Property &prop : lit->props) {
    const Field *field = target->field(prop.name);
    if (!field || !GenericCheckHelpers::mentionsAnyOf(field->type, genericTypeParams) ||
        TypeArgInferrer::isContextSensitive(prop.value) || isa<ObjectLitExpr>(prop.value) ||
        isa<ArrayLitExpr>(prop.value) || GenericChecker::literalShape(prop.value)) {
      continue;
    }
    prop.value = this->exprChecker->checkExpr(prop.value);
    this->state.getExpressionMarks().markPrechecked(prop.value);
    inference.from(field->type, prop.value->type);
  }
}

TypeArgInferrer::ResultParamInferred
TypeArgInferrer::paramInferred(const FuncSymbol *generic, const Type *bound, size_t k, bool call) {
  llvm::ArrayRef<const Type *> params = generic->typeParams;
  if (!bound) {
    std::string ints;
    for (size_t i = 0; i < params.size(); ++i) {
      ints += (i == 0 ? "" : ", ") + std::string(params[i]->isListParam() ? "[int]" : "int");
    }
    std::string message =
        ErrorTextParamInferred::cannot(params[k]->name(), generic->name, call, ints);

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
                                                                   const Type *wanted) {
  bool fits = given == wanted || wanted->isError() || bound->isError();
  if (!fits && given) {
    std::string message = ErrorTextIterableFits::elements(bound->name(), given->name(),
                                                          wanted->name(), generic->name);

    return err(TypeArgInferrerIterableFitsError{
        .code = TypeArgInferrerIterableFitsErrorCode::WRONG_ELEMENTS,
        .message = message,
    });
  }
  if (!fits) {
    std::string message =
        ErrorTextIterableFits::notIter(bound->name(), wanted->name(), generic->name);

    return err(TypeArgInferrerIterableFitsError{
        .code = TypeArgInferrerIterableFitsErrorCode::NOT_ITERABLE,
        .message = message,
    });
  }

  return ok(bound);
}

TypeArgInferrer::ResultConstraintMet
TypeArgInferrer::constraintMet(const FuncSymbol *generic, std::vector<const Type *> &bound,
                               size_t k) {
  llvm::ArrayRef<const Type *> params = generic->typeParams;
  if (!this->typeParams->satisfies(bound[k], params[k], params, bound)) {
    llvm::ArrayRef<TypeParamDecl> decls = generic->decl->typeParams;
    std::string constraint =
        GenericCheckHelpers::constraintSpelling(params[k], k < decls.size() ? &decls[k] : nullptr);
    std::string message = ErrorTextConstraintMet::unsatisfied(bound[k]->name(), constraint,
                                                              params[k]->name(), generic->name);

    return err(TypeArgInferrerConstraintMetError{
        .code = TypeArgInferrerConstraintMetErrorCode::CONSTRAINT_UNSATISFIED,
        .message = message,
    });
  }

  return ok(bound[k]);
}

TypeArgInferrer::ResultTypeArgsCount
TypeArgInferrer::typeArgCount(const FuncSymbol *generic, llvm::ArrayRef<TypeRef> typeArgs) {
  if (typeArgs.size() > generic->typeParams.size()) {
    std::string message =
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
                                                                     const Type *candidate) {
  const Type *next = type ? TypeRelations::unify(type, candidate) : candidate;
  if (!next) {
    std::string message = ErrorTextCandidates::conflict(param->name(), generic->name, type->name(),
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
                               bool mentioned, const FunctionExpr *fe, size_t p) {
  if (!bound && mentioned) {
    std::string what = fe && p < fe->fn->params.size()
                           ? ErrorTextCallbackParam::param(fe->fn->params[p].name)
                           : ErrorTextCallbackParam::params();
    std::string message =
        ErrorTextCallbackParam::uninferred(param->name(), generic->name, what, fe != nullptr);

    return err(TypeArgInferrerCallbackParamError{
        .code = TypeArgInferrerCallbackParamErrorCode::CALLBACK_PARAM_UNINFERRED,
        .message = message,
    });
  }

  return ok(param);
}

TypeArgInferrer::ResultArgNotGrowing
TypeArgInferrer::notGrowing(const FuncSymbol *generic, const Type *bound, const Type *param) {
  if (TypeArgInferrer::grows(bound, generic->typeParams, false)) {
    std::string message = ErrorTextNotGrowing::grows(generic->name, bound->name(), param->name());

    return err(TypeArgInferrerNotGrowingError{
        .code = TypeArgInferrerNotGrowingErrorCode::GROWING_TYPE_ARGUMENT,
        .message = message,
    });
  }

  return ok(bound);
}

bool TypeArgInferrer::finishTypeArgs(FuncSymbol *generic, std::vector<const Type *> &bound,
                                     llvm::SMLoc loc, bool call, bool quiet) {
  llvm::ArrayRef<const Type *> params = generic->typeParams;
  bool inferred = true;
  for (size_t k = 0; k < params.size(); ++k) {
    if (!bound[k] && k < generic->typeDefaults.size() && generic->typeDefaults[k]) {
      const Type *fallback = this->typeParams->substitute(generic->typeDefaults[k], params, bound);
      bool open = false;
      for (size_t j = 0; j < params.size(); ++j) {
        open |= !bound[j] && GenericCheckHelpers::mentionsAnyOf(fallback, params[j]);
      }
      if (!open) {
        bound[k] = fallback;
      }
    }
    if (bound[k] && bound[k]->isError()) {
      inferred = false;
      continue;
    }
    if (!bound[k] && quiet) {
      bound[k] = Type::error();
      inferred = false;
      continue;
    }
    ResultParamInferred paramKnown = TypeArgInferrer::paramInferred(generic, bound[k], k, call);
    if (paramKnown.isErr()) {
      this->state.getDiagnostics().error(loc, paramKnown.error().message);
      bound[k] = Type::error();
      inferred = false;
      continue;
    }
    if (const Type *element = params[k]->iterated()) {
      const Type *wanted = this->typeParams->substitute(element, params, bound);
      const Type *elements = this->iterableChecker->elementTypeOf(bound[k]);
      ResultIterableMatch matched =
          TypeArgInferrer::iterableFits(generic, bound[k], elements, wanted);
      if (matched.isErr()) {
        this->state.getDiagnostics().error(loc, matched.error().message);
        bound[k] = Type::error();
        inferred = false;
      }
      continue;
    }
    ResultConstraintMet met = this->constraintMet(generic, bound, k);
    if (met.isErr()) {
      this->state.getDiagnostics().error(loc, met.error().message);
      bound[k] = Type::error();
      inferred = false;
    }
  }

  return inferred;
}

Expr *TypeArgInferrer::checkGenericCall(CallExpr *call, FuncSymbol *generic, llvm::SMLoc loc,
                                        const Type *expected, llvm::ArrayRef<TypeRef> typeArgs) {
  std::vector<const Type *> bound;
  if (!this->bindGenericCall(call, generic, loc, expected, typeArgs, bound)) {
    return call;
  }
  call->target = FunctionCall{.func = generic};
  const ClassDecl *cls = generic->decl ? generic->decl->cls() : nullptr;
  bool concrete =
      llvm::none_of(bound, [](const Type *t) { return TypeQueries::mentionsParam(t); }) &&
      (!cls || generic->decl->isStatic() || cls->isEmitted());
  bool copiesReturn = this->lateClasses->isTemplate(generic) && !generic->hasReturnType();
  if (concrete && !this->state.getFunctionStack().isInsideTrial() && !generic->inferring &&
      (generic->hasReturnType() || copiesReturn)) {
    if (FuncSymbol *instance = this->genericInstantiator->instantiate(
            generic, bound, loc, this->genericInstantiator->exactClassArgs(call, generic))) {
      call->target = FunctionCall{.func = instance};
      if (copiesReturn) {
        call->type = instance->hasReturnType() ? instance->getKnownReturnType() : Type::error();
      }
    }
  }

  return call;
}

bool TypeArgInferrer::bindGenericCall(CallExpr *call, FuncSymbol *generic, llvm::SMLoc loc,
                                      const Type *expected, llvm::ArrayRef<TypeRef> typeArgs,
                                      std::vector<const Type *> &bound) {
  call->type = Type::error();
  const Type *result = generic->getReturnType();
  bool ownClass = this->genericInstantiator->returnsOwnClass(generic, result);
  GenericCallBinding binding{
      .call = call,
      .generic = generic,
      .loc = loc,
      .typeParams = generic->typeParams,
      .result = ownClass ? nullptr : result,
      .listRest = TypeArgInferrer::listRestOf(generic),
      .fixed = generic->params.size() - (generic->hasRest ? 1 : 0),
      .errorsBefore = this->state.getDiagnostics().errorsSeen(),
      .bound = bound,
  };
  if (this->hasRejectedSpread(binding)) {
    return false;
  }
  unsigned minArgs = static_cast<unsigned>(generic->minArgs);
  unsigned maxArgs = generic->hasRest ? ~0u : static_cast<unsigned>(generic->params.size());
  ArgumentChecker::ResultArityMatches arity =
      this->argumentChecker->checkArity(call, minArgs, maxArgs);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
    return false;
  }

  bound.assign(binding.typeParams.size(), nullptr);
  binding.checked.assign(call->args.size(), false);
  if (!typeArgs.empty()) {
    this->bindExplicitTypeArgs(binding, typeArgs);
  } else {
    this->inferTypeArgs(binding, expected);
  }
  this->finishBinding(binding);
  if (!binding.inferred) {
    this->walkUncheckedArguments(binding);
    return false;
  }

  return this->instantiateCall(binding, typeArgs.empty());
}

const Type *TypeArgInferrer::listRestOf(const FuncSymbol *generic) {
  if (!generic->hasRest) {
    return nullptr;
  }
  const Type *rest = generic->params.back();
  bool list = rest->isListParam() || ArgListChecker::spreadsLists(rest);

  return list ? rest : nullptr;
}

const Type *TypeArgInferrer::parameterFor(const GenericCallBinding &binding, size_t i) {
  const FuncSymbol *generic = binding.generic;
  if (!generic->hasRest || i < binding.fixed) {
    return generic->params[i];
  }
  const Type *rest = generic->params.back();
  bool element = !binding.call->spreads(i) && rest->isArray();

  return element ? rest->inner() : rest;
}

bool TypeArgInferrer::hasRejectedSpread(const GenericCallBinding &binding) {
  CallExpr *call = binding.call;
  size_t limit = binding.generic->hasRest ? binding.fixed : call->args.size();
  for (size_t i = 0; i < call->args.size() && i < limit; ++i) {
    bool rejected = call->spreads(i) && this->callChecker->rejectSpread(call);
    if (rejected) {
      return true;
    }
  }

  return false;
}

void TypeArgInferrer::bindExplicitTypeArgs(GenericCallBinding &binding,
                                           llvm::ArrayRef<TypeRef> typeArgs) {
  ResultTypeArgsCount counted = TypeArgInferrer::typeArgCount(binding.generic, typeArgs);
  if (counted.isErr()) {
    this->state.getDiagnostics().error(binding.loc, counted.error().message);
    binding.inferred = false;
  }
  for (size_t k = 0; k < typeArgs.size() && k < binding.typeParams.size(); ++k) {
    binding.bound[k] = this->typeResolver->resolveType(typeArgs[k], true);
  }
}

void TypeArgInferrer::inferTypeArgs(GenericCallBinding &binding, const Type *expected) {
  auto elementOf = [this](const Type *type) { return this->iterableChecker->elementTypeOf(type); };
  auto asyncOf = [this](const Type *type) {
    return this->iterableChecker->asyncIterableType(type);
  };
  Inference inference(binding.typeParams, this->state.getAliasStore(), elementOf);
  inference.setAsyncOf(asyncOf);

  for (size_t i = 0; i < binding.call->args.size(); ++i) {
    this->walkArgument(binding, inference, i);
  }
  this->inferFromListRest(binding, inference);
  this->settleWithCandidates(binding, inference);
  if (inference.hasDeferred()) {
    inference.retry(binding.bound);
    this->settleWithCandidates(binding, inference);
  }
  this->inferFromExpected(binding, expected);
  for (size_t k = 0; k < binding.typeParams.size(); ++k) {
    this->settleTypeParam(binding, inference, k);
  }
  for (size_t i : binding.contextual) {
    this->walkContextualArgument(binding, inference, i);
  }
}

void TypeArgInferrer::walkArgument(GenericCallBinding &binding, Inference &inference, size_t i) {
  GenericArgumentForm form = this->argumentForm(binding, i);
  switch (form.kind()) {
    case GenericArgumentKind::LIST_REST:
      return this->checkArgumentNow(binding, i, nullptr);
    case GenericArgumentKind::PLAIN:
      return this->checkArgumentNow(binding, i, TypeArgInferrer::parameterFor(binding, i));
    case GenericArgumentKind::LITERAL_TEXT:
      return this->inferFromLiteralText(binding, inference, i,
                                        form.as<LiteralTextArgument>()->text);
    case GenericArgumentKind::CONTEXTUAL:
      return binding.contextual.push_back(i);
    case GenericArgumentKind::LITERAL_SHAPE:
      return this->inferFromLiteralShape(binding, inference, i,
                                         form.as<LiteralShapeArgument>()->shape);
    case GenericArgumentKind::CONTEXTUAL_LITERAL:
      return this->deferLiteral(binding, inference, i);
    case GenericArgumentKind::INFERRED:
      return this->inferFromCheckedArgument(binding, inference, i);
  }
}

GenericArgumentForm TypeArgInferrer::argumentForm(const GenericCallBinding &binding, size_t i) {
  Expr *arg = binding.call->args[i];
  const Type *pattern = TypeArgInferrer::parameterFor(binding, i);
  if (binding.listRest != nullptr && i >= binding.fixed) {
    return ListRestArgument{};
  }
  if (!GenericCheckHelpers::mentionsAnyOf(pattern, binding.typeParams)) {
    return PlainArgument{};
  }
  auto *text = dyn_cast<StringLitExpr>(arg);
  bool literalText = text != nullptr && llvm::is_contained(binding.typeParams, pattern) &&
                     this->utilityTypes->wantsLiteral(pattern);
  if (literalText) {
    return LiteralTextArgument{.text = text};
  }
  bool contextual = TypeArgInferrer::isContextSensitive(arg) || this->makesResults(arg) ||
                    this->isBareGenericName(arg);
  if (contextual) {
    return ContextualArgument{};
  }
  const Type *shape = GenericChecker::literalShape(arg);
  if (shape != nullptr) {
    return LiteralShapeArgument{.shape = shape};
  }
  if (isa<ArrayLitExpr>(arg) || isa<ObjectLitExpr>(arg)) {
    return ContextualLiteralArgument{};
  }

  return InferredArgument{};
}

bool TypeArgInferrer::makesResults(const Expr *arg) const {
  auto *name = dyn_cast<NameExpr>(arg);
  if (name == nullptr) {
    return false;
  }
  std::optional<GlobalName> which = this->resultTypes->resultName(name->name);

  return which == GlobalName::OK || which == GlobalName::ERR;
}

bool TypeArgInferrer::isBareGenericName(Expr *arg) const {
  auto *name = dyn_cast<NameExpr>(arg);

  return name != nullptr && this->genericChecker->genericNamed(name) != nullptr &&
         name->typeArgs.empty();
}

void TypeArgInferrer::checkArgumentNow(GenericCallBinding &binding, size_t i, const Type *hint) {
  Expr *&arg = binding.call->args[i];
  arg = this->exprChecker->checkExpr(arg, hint);
  binding.checked[i] = true;
}

void TypeArgInferrer::inferFromLiteralText(GenericCallBinding &binding, Inference &inference,
                                           size_t i, const StringLitExpr *text) {
  const Type *literal = Type::stringLiteral(text->value);
  this->checkArgumentNow(binding, i, literal);
  const Type *pattern = TypeArgInferrer::parameterFor(binding, i);
  inference.from(pattern, literal);
}

void TypeArgInferrer::inferFromLiteralShape(GenericCallBinding &binding, Inference &inference,
                                            size_t i, const Type *shape) {
  const Expr *arg = binding.call->args[i];
  const Type *pattern = TypeArgInferrer::parameterFor(binding, i);
  binding.literals.push_back(i);
  bool intLiteral = shape->isInteger() && TypeRelations::isIntLiteralTree(arg);
  inference.markArgument(true, intLiteral);
  inference.from(pattern, shape);
  inference.markArgument(false, false);
}

void TypeArgInferrer::deferLiteral(GenericCallBinding &binding, Inference &inference, size_t i) {
  binding.contextual.push_back(i);
  auto *object = dyn_cast<ObjectLitExpr>(binding.call->args[i]);
  if (object != nullptr) {
    const Type *pattern = TypeArgInferrer::parameterFor(binding, i);
    this->precheckFields(object, pattern, binding.typeParams, inference);
  }
}

void TypeArgInferrer::inferFromCheckedArgument(GenericCallBinding &binding, Inference &inference,
                                               size_t i) {
  this->checkArgumentNow(binding, i, nullptr);
  const Expr *arg = binding.call->args[i];
  const Type *pattern = TypeArgInferrer::parameterFor(binding, i);
  bool intLiteral = arg->type->isInteger() && TypeRelations::isIntLiteralTree(arg);
  inference.markArgument(false, intLiteral);
  inference.from(pattern, arg->type);
  inference.markArgument(false, false);
}

void TypeArgInferrer::inferFromListRest(GenericCallBinding &binding, Inference &inference) {
  if (binding.listRest == nullptr) {
    return;
  }
  CallExpr *call = binding.call;
  std::vector<const Type *> parts;
  std::vector<bool> spread;
  for (size_t i = binding.fixed; i < call->args.size(); ++i) {
    parts.push_back(call->args[i]->type);
    spread.push_back(call->spreads(i));
  }
  std::string why;
  const Type *list = ArgumentLists::spreadTuple(parts, spread, why);
  if (list != nullptr) {
    inference.from(binding.listRest, list);
  }
}

void TypeArgInferrer::settleWithCandidates(GenericCallBinding &binding, Inference &inference) {
  for (size_t k = 0; k < binding.typeParams.size(); ++k) {
    if (!inference.getCandidates(k).empty()) {
      this->settleTypeParam(binding, inference, k);
    }
  }
}

void TypeArgInferrer::settleTypeParam(GenericCallBinding &binding, Inference &inference, size_t k) {
  if (binding.bound[k] != nullptr) {
    return;
  }
  ResultCandidateType joined = this->joinCandidates(binding, inference, k);
  if (joined.isErr()) {
    this->state.getDiagnostics().error(binding.loc, joined.error().message);
    binding.bound[k] = Type::error();
    binding.inferred = false;
  } else {
    const Type *widened = TypeArgInferrer::widenToConstraint(binding, inference, k, joined.value());
    binding.bound[k] = TypeArgInferrer::withUndefined(inference, k, widened);
  }
}

TypeArgInferrer::ResultCandidateType
TypeArgInferrer::joinCandidates(const GenericCallBinding &binding, const Inference &inference,
                                size_t k) {
  const FuncSymbol *generic = binding.generic;
  const Type *param = binding.typeParams[k];
  bool strong = !inference.getCandidates(k).empty();
  const Type *type = nullptr;
  for (const Type *candidate : inference.getCandidates(k)) {
    ResultCandidateJoin joined = TypeArgInferrer::candidatesJoin(generic, param, type, candidate);
    if (joined.isErr()) {
      return err(TypeArgInferrerJoinCandidatesError{
          .code = TypeArgInferrerJoinCandidatesErrorCode::CANDIDATES_CLASH,
          .message = joined.error().message,
      });
    }
    type = joined.value();
  }
  for (const Type *candidate : inference.getWeakCandidates(k)) {
    ResultCandidateJoin joined = TypeArgInferrer::candidatesJoin(generic, param, type, candidate);
    if (joined.isErr() && strong) {
      continue;
    }
    if (joined.isErr()) {
      return err(TypeArgInferrerJoinCandidatesError{
          .code = TypeArgInferrerJoinCandidatesErrorCode::CANDIDATES_CLASH,
          .message = joined.error().message,
      });
    }
    type = joined.value();
  }
  for (const Type *candidate : inference.getLiteralCandidates(k)) {
    bool otherNumber =
        type != nullptr && type != candidate && (type->isNumeric() || type->isDecimal());
    if (otherNumber) {
      continue;
    }
    ResultCandidateJoin joined = TypeArgInferrer::candidatesJoin(generic, param, type, candidate);
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

const Type *TypeArgInferrer::widenToConstraint(const GenericCallBinding &binding,
                                               const Inference &inference, size_t k,
                                               const Type *type) {
  const Type *constraint = binding.typeParams[k]->constraint();
  if (constraint != nullptr && constraint->isUnion()) {
    constraint = LiteralTypes::numberMemberOf(constraint, false);
  }
  bool onlyLiterals = inference.getCandidates(k).empty() && inference.getWeakCandidates(k).empty();
  bool fractional = constraint != nullptr && (constraint->isFloat() || constraint->isDecimal());
  bool widens = type != nullptr && type->isInteger() && onlyLiterals && fractional;

  return widens ? constraint : type;
}

const Type *TypeArgInferrer::withUndefined(const Inference &inference, size_t k, const Type *type) {
  if (!inference.hasSeenUndefined(k)) {
    return type;
  }

  return type != nullptr ? Type::optional(type) : Type::voidTy();
}

void TypeArgInferrer::inferFromExpected(GenericCallBinding &binding, const Type *expected) {
  const Type *result = binding.result;
  bool open = llvm::is_contained(binding.bound, nullptr);
  bool useful = expected != nullptr && open && result != nullptr &&
                GenericCheckHelpers::mentionsAnyOf(result, binding.typeParams);
  if (!useful) {
    return;
  }
  Inference fromExpected(binding.typeParams, this->state.getAliasStore(), nullptr);
  fromExpected.from(result, expected);
  for (size_t k = 0; k < binding.typeParams.size(); ++k) {
    if (binding.bound[k] != nullptr || fromExpected.getCandidates(k).empty()) {
      continue;
    }
    const Type *type = nullptr;
    for (const Type *candidate : fromExpected.getCandidates(k)) {
      type = type != nullptr ? TypeRelations::unify(type, candidate) : candidate;
    }
    binding.bound[k] = type;
  }
}

void TypeArgInferrer::walkContextualArgument(GenericCallBinding &binding, Inference &inference,
                                             size_t i) {
  const Type *pattern = TypeArgInferrer::parameterFor(binding, i);
  const Type *partial = this->typeParams->substitute(pattern, binding.typeParams, binding.bound);
  const Type *function = partial->nonOptional();
  if (function->isFunction()) {
    this->walkCallbackArgument(binding, i, function);
  } else {
    this->walkArgumentWithDefaults(binding, i, pattern, partial);
  }
  binding.checked[i] = true;
  this->adoptArgumentType(binding, inference, i, pattern);
}

void TypeArgInferrer::walkCallbackArgument(GenericCallBinding &binding, size_t i,
                                           const Type *function) {
  Expr *&arg = binding.call->args[i];
  Signature signature = TypeArgInferrer::callbackSignature(binding.typeParams, function);
  auto *name = dyn_cast<NameExpr>(arg);
  FuncSymbol *other = name != nullptr ? this->genericChecker->genericNamed(name) : nullptr;
  if (other == nullptr) {
    this->walkCallbackParams(binding, i, function);
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
  unsigned arity = static_cast<unsigned>(signature.params.size());
  FunctionExpr *wrapper =
      this->functionValueChecker->builtinFunctionValue(arg, signature, arity, {});
  if (wrapper != nullptr) {
    return this->closureChecker->checkFunctionExpr(wrapper, &signature);
  }

  return this->exprChecker->checkExpr(arg);
}

Signature TypeArgInferrer::callbackSignature(llvm::ArrayRef<const Type *> typeParams,
                                             const Type *function) {
  Signature signature;
  for (const Type *param : function->params()) {
    bool open = GenericCheckHelpers::mentionsAnyOf(param, typeParams);
    signature.params.push_back(open ? Type::error() : param);
  }
  bool openResult = GenericCheckHelpers::mentionsAnyOf(function->ret(), typeParams);
  signature.minArgs = function->minArgs();
  signature.ret = openResult ? nullptr : function->ret();
  signature.voidResult = function->ret()->isVoid();

  return signature;
}

void TypeArgInferrer::walkCallbackParams(GenericCallBinding &binding, size_t i,
                                         const Type *function) {
  const Expr *arg = binding.call->args[i];
  auto *closure = dyn_cast<FunctionExpr>(arg);
  for (size_t p = 0; p < function->params().size(); ++p) {
    bool written = closure != nullptr && p < closure->fn->params.size() &&
                   closure->fn->params[p].type.has_value();
    if (written) {
      continue;
    }
    for (size_t k = 0; k < binding.typeParams.size(); ++k) {
      const Type *param = binding.typeParams[k];
      bool mentioned = GenericCheckHelpers::mentionsAnyOf(function->params()[p], param);
      ResultCallbackParam known = TypeArgInferrer::callbackParam(
          binding.generic, param, binding.bound[k], mentioned, closure, p);
      if (known.isErr()) {
        this->state.getDiagnostics().error(arg->loc(), known.error().message);
        binding.bound[k] = Type::error();
        binding.inferred = false;
      }
    }
  }
}

void TypeArgInferrer::walkArgumentWithDefaults(GenericCallBinding &binding, size_t i,
                                               const Type *pattern, const Type *partial) {
  llvm::ArrayRef<const Type *> params = binding.typeParams;
  bool open = GenericCheckHelpers::mentionsAnyOf(partial, params);
  bool defaulted = false;
  for (size_t k = 0; k < params.size() && open; ++k) {
    const Type *fallback = TypeArgInferrer::typeDefault(binding.generic, k);
    if (binding.bound[k] == nullptr && fallback != nullptr) {
      binding.bound[k] = fallback;
      defaulted = true;
    }
  }
  const Type *hint = partial;
  if (defaulted) {
    hint = this->typeParams->substitute(pattern, params, binding.bound);
  }
  bool stillOpen = GenericCheckHelpers::mentionsAnyOf(hint, params);
  this->checkArgumentNow(binding, i, stillOpen ? nullptr : hint);
}

const Type *TypeArgInferrer::typeDefault(const FuncSymbol *generic, size_t k) {
  bool hasDefault = k < generic->typeDefaults.size() && generic->typeDefaults[k] != nullptr;
  if (!hasDefault) {
    return nullptr;
  }
  const Type *fallback = generic->typeDefaults[k];
  bool closed = !GenericCheckHelpers::mentionsAnyOf(fallback, generic->typeParams);

  return closed ? fallback : nullptr;
}

void TypeArgInferrer::adoptArgumentType(GenericCallBinding &binding, Inference &inference, size_t i,
                                        const Type *pattern) {
  auto elementOf = [this](const Type *type) { return this->iterableChecker->elementTypeOf(type); };
  Inference more(binding.typeParams, this->state.getAliasStore(), elementOf);
  more.from(pattern, binding.call->args[i]->type);
  for (size_t k = 0; k < binding.typeParams.size(); ++k) {
    if (binding.bound[k] == nullptr) {
      inference.adoptCandidates(k, more);
      this->settleTypeParam(binding, inference, k);
    }
  }
}

void TypeArgInferrer::finishBinding(GenericCallBinding &binding) {
  bool quiet = this->hasArgumentErrors(binding);
  bool finished = this->finishTypeArgs(binding.generic, binding.bound, binding.loc, true, quiet);
  binding.inferred = finished && binding.inferred;
  for (size_t k = 0; k < binding.typeParams.size() && binding.inferred; ++k) {
    ResultArgNotGrowing growing =
        TypeArgInferrer::notGrowing(binding.generic, binding.bound[k], binding.typeParams[k]);
    if (growing.isErr()) {
      this->state.getDiagnostics().error(binding.loc, growing.error().message);
      binding.inferred = false;
    }
  }
}

bool TypeArgInferrer::hasArgumentErrors(const GenericCallBinding &binding) const {
  bool failed = false;
  for (size_t i = 0; i < binding.call->args.size(); ++i) {
    failed |= binding.checked[i] && binding.call->args[i]->type->isError();
  }
  bool reported = this->state.getDiagnostics().errorsSeen() > binding.errorsBefore;

  return failed && reported;
}

void TypeArgInferrer::walkUncheckedArguments(GenericCallBinding &binding) {
  for (size_t i = 0; i < binding.call->args.size(); ++i) {
    if (!binding.checked[i]) {
      this->checkArgumentNow(binding, i, nullptr);
    }
  }
}

bool TypeArgInferrer::instantiateCall(GenericCallBinding &binding, bool inferredArgs) {
  CallExpr *call = binding.call;
  const FuncSymbol *generic = binding.generic;
  this->state.getGenericStore().enterInstanceLoc(binding.loc);
  std::vector<const Type *> params;
  for (const Type *param : generic->params) {
    params.push_back(this->typeParams->substituteValue(param, binding.typeParams, binding.bound));
  }
  for (size_t i : binding.literals) {
    const Type *pattern = TypeArgInferrer::parameterFor(binding, i);
    const Type *hint =
        this->typeParams->substituteValue(pattern, binding.typeParams, binding.bound);
    this->checkArgumentNow(binding, i, hint);
  }
  const Type *result = binding.result;
  call->type = result != nullptr
                   ? this->typeParams->substitute(result, binding.typeParams, binding.bound)
                   : Type::error();
  this->state.getGenericStore().leaveInstanceLoc();

  size_t minArgs = generic->minArgs;
  bool hasRest = generic->hasRest;
  std::vector<const Type *> flat;
  if (ArgumentLists::flattenArguments(params, minArgs, hasRest, flat)) {
    params = std::move(flat);
  }
  std::vector<bool> *checked = inferredArgs ? &binding.checked : nullptr;
  this->argumentChecker->checkArguments(call, params, minArgs, hasRest, binding.loc, checked);

  return result != nullptr;
}

bool TypeArgInferrer::inferFromArguments(FuncSymbol *generic, llvm::ArrayRef<Expr *> args,
                                         std::vector<const Type *> &bound) {
  llvm::ArrayRef<const Type *> genericTypeParams = generic->typeParams;
  Inference inference(genericTypeParams, this->state.getAliasStore(),
                      [this](const Type *t) { return this->iterableChecker->elementTypeOf(t); });
  inference.setAsyncOf(
      [this](const Type *t) { return this->iterableChecker->asyncIterableType(t); });
  size_t fixed = generic->params.size() - (generic->hasRest ? 1 : 0);
  for (size_t i = 0; i < args.size(); ++i) {
    const Type *pattern = nullptr;
    if (i < fixed) {
      pattern = generic->params[i];
    } else if (generic->hasRest && generic->params.back()->isArray()) {
      pattern = generic->params.back()->inner();
    }
    if (pattern && args[i]->type) {
      inference.from(pattern, args[i]->type);
    }
  }
  bound.assign(genericTypeParams.size(), nullptr);
  for (size_t k = 0; k < genericTypeParams.size(); ++k) {
    const Type *type = nullptr;
    for (const Type *c : inference.getCandidates(k)) {
      type = type ? TypeRelations::unify(type, c) : c;
      if (!type) {
        return false;
      }
    }
    if (type && inference.hasSeenUndefined(k)) {
      type = Type::optional(type);
    } else if (!type && inference.hasSeenUndefined(k)) {
      type = Type::voidTy();
    }
    bound[k] = type;
  }
  for (size_t k = 0; k < genericTypeParams.size(); ++k) {
    if (!bound[k] && k < generic->typeDefaults.size() && generic->typeDefaults[k]) {
      bound[k] = this->typeParams->substitute(generic->typeDefaults[k], genericTypeParams, bound);
    }
    if (!bound[k] || GenericCheckHelpers::mentionsAnyOf(bound[k], genericTypeParams) ||
        !this->typeParams->satisfies(bound[k], genericTypeParams[k], genericTypeParams, bound)) {
      return false;
    }
  }

  return true;
}

}
