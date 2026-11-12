#include "compiler/semantic/bodies/iterators/iterable_checker/iterable_checker.h"
#include "compiler/program/library/builtin_names/type_name_table/type_name_table.h"
#include "compiler/semantic/bodies/classes/class_bases/class_bases.h"
#include "compiler/semantic/bodies/classes/method_call_checker/method_call_checker.h"
#include "compiler/semantic/bodies/functions/function_checker/function_checker.h"
#include "compiler/semantic/bodies/iterators/generator_checker/generator_checker.h"
#include "compiler/semantic/declare/class_declarer/class_declarer.h"
#include "compiler/semantic/declare/type_declarer/type_declarer.h"
#include "compiler/semantic/flow/flow_checker/flow_checker.h"
#include "compiler/semantic/generics/generic_instantiator/generic_instantiator.h"
#include "compiler/semantic/infer/return_inferrer/return_inferrer.h"
#include "compiler/semantic/library/intl/intl_checker/intl_checker.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "compiler/semantic/types/type_resolver/type_resolver.h"

namespace tz {

namespace {

constexpr llvm::StringLiteral kIteratorMethod = "[Symbol.iterator]";

constexpr llvm::StringLiteral kAsyncIteratorMethod = "[Symbol.asyncIterator]";

}

IterableChecker::IterableChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->classBases = &all.classBases;
  this->flowChecker = &all.flowChecker;
  this->intlChecker = &all.intlChecker;
  this->typeDeclarer = &all.typeDeclarer;
  this->typeResolver = &all.typeResolver;
  this->classDeclarer = &all.classDeclarer;
  this->typeRelations = &all.typeRelations;
  this->returnInferrer = &all.returnInferrer;
  this->functionChecker = &all.functionChecker;
  this->generatorChecker = &all.generatorChecker;
  this->methodCallChecker = &all.methodCallChecker;
  this->genericInstantiator = &all.genericInstantiator;
}

FuncSymbol *IterableChecker::iterateFunction() {
  auto *known = this->state.getExpressionMarks().getIterateSymbol();
  if (known != nullptr) {
    return known;
  }
  auto items = this->state.getLibraryDeclarations().getBuiltinItems(BuiltinText::ITERATE);
  auto *functionDecl = llvm::cast<FunctionDecl>(items.front());
  this->state.getScopeStack().enterOnlyTypes(this->state.getScopeStack().getTopTypes());
  this->functionChecker->declareSignature(functionDecl);
  this->state.getScopeStack().leaveOnlyTypes();
  this->state.getExpressionMarks().rememberIterateSymbol(functionDecl->sym);
  this->state.getGenericStore().rememberExactParams(functionDecl->sym, {true, true});

  return functionDecl->sym;
}

const Type *IterableChecker::iteratorResultType(const Type *value, const Type *returned) const {
  if (returned == nullptr || returned->isVoid()) {
    returned = Type::undefinedTy();
  }

  return Type::unionOf(
      {Type::object({{"done", Type::boolLiteral(false), false}, {"value", value, false}}),
       Type::object({{"done", Type::boolLiteral(true), false}, {"value", returned, false}})});
}

const Type *IterableChecker::nextValueType(const Type *type) {
  type = type->nonOptional();
  if (!type->isObject() || type->isTuple()) {
    return nullptr;
  }
  const Type *next = nullptr;
  auto *classDecl = this->classDeclarer->classOf(type);
  if (classDecl != nullptr) {
    this->classDeclarer->completeClass(classDecl);
    for (FunctionDecl *functionDecl : this->classDeclarer->methodsNamed(classDecl, "next")) {
      if (functionDecl->sym != nullptr && functionDecl->sym->minArgs == 0 &&
          functionDecl->typeParams.empty()) {
        next = this->returnInferrer->functionType(functionDecl->sym);
      }
    }
  }
  const auto *field = type->field("next");
  if (next == nullptr && field != nullptr && field->type->isFunction()) {
    next = field->type;
  }
  if (next == nullptr || next->minArgs() != 0 || next->ret()->isError()) {
    return nullptr;
  }
  const Type *result = next->ret();
  llvm::ArrayRef<const Type *> members =
      result->isUnion() ? result->members() : llvm::ArrayRef<const Type *>(result);
  auto values = std::vector<const Type *>();
  for (const Type *member : members) {
    const Field *done = member->isObject() ? member->field("done") : nullptr;
    const Field *value = member->isObject() ? member->field("value") : nullptr;
    if (done == nullptr || value == nullptr ||
        !(done->type->isBool() ||
          (done->type->isLiteral() && done->type->literalBase()->isBool()))) {
      return nullptr;
    }
    if (done->type->isLiteral() && done->type->literalNumber() != 0) {
      continue;
    }
    values.push_back(value->type);
  }
  if (values.empty()) {
    return nullptr;
  }
  const auto *value = Type::unionOf(values);

  return value->isUndefined() || value->isVoid() ? nullptr : value;
}

Expr *IterableChecker::iteratorOf(Expr *it) {
  const Type *type = it->type;
  if (type->isIterator() || type->isError()) {
    return it;
  }
  const auto *value = this->nextValueType(type);
  if (value == nullptr) {
    return nullptr;
  }
  auto *iterate = this->iterateFunction();
  llvm::SMLoc loc = it->loc();
  auto *callee = this->state.getAst().make<NameExpr>(loc, iterate->name);
  auto *call = this->state.getAst().make<CallExpr>(loc, callee, std::vector<Expr *>{it});
  call->target = FunctionCall{.func = iterate};
  if (!this->state.getFunctionStack().isInsideTrial()) {
    auto *instance =
        this->genericInstantiator->instantiate(iterate, {type->nonOptional(), value}, loc);
    if (instance != nullptr) {
      call->target = FunctionCall{.func = instance};
    }
  }
  callee->binding = NamedFunction{.func = call->func()};
  call->type = Type::iterator(value);
  callee->type = Type::function({type->nonOptional()}, 1, false, call->type);
  if (type->isOptional()) {
    call->args[0] = this->typeRelations->convert(it, type->nonOptional());
  }
  this->flowChecker->calledFunction();

  return call;
}

Expr *IterableChecker::userIterator(Expr *expr) {
  const Type *type = expr->type;
  ClassDecl *classDecl = type != nullptr ? this->classDeclarer->classOf(type) : nullptr;
  if (classDecl == nullptr) {
    return nullptr;
  }
  this->classDeclarer->completeClass(classDecl);
  if (this->classDeclarer->methodsNamed(classDecl, kIteratorMethod).empty() &&
      this->classBases->findField(classDecl, kIteratorMethod) == nullptr) {
    return nullptr;
  }
  auto *member = this->state.getAst().make<MemberExpr>(expr->loc(), expr, kIteratorMethod);
  auto *call = this->state.getAst().make<CallExpr>(expr->loc(), member, std::vector<Expr *>{});
  auto *result = this->methodCallChecker->checkMethodCall(call, member);
  if (result->type->isError() || result->type->isIterator()) {
    return result;
  }
  auto *it = this->iteratorOf(result);
  if (it != nullptr) {
    return it;
  }
  auto given = this->iteratorGiven(type, result);
  if (given.isErr()) {
    this->state.getDiagnostics().error(expr->loc(), given.error().message);
  }
  result->type = Type::error();

  return result;
}

const Type *IterableChecker::asyncIterableType(const Type *type) {
  ClassDecl *classDecl = type != nullptr ? this->classDeclarer->classOf(type) : nullptr;
  if (classDecl == nullptr) {
    return nullptr;
  }
  this->classDeclarer->completeClass(classDecl);
  auto methods = this->classDeclarer->methodsNamed(classDecl, kAsyncIteratorMethod);
  if (methods.size() != 1 || methods[0]->sym == nullptr) {
    return nullptr;
  }
  const auto *result = methods[0]->sym->getReturnType();

  return result != nullptr && result->isAsyncIterator() ? result : nullptr;
}

Expr *IterableChecker::asyncUserIterator(Expr *expr) {
  const Type *type = expr->type;
  ClassDecl *classDecl = type != nullptr ? this->classDeclarer->classOf(type) : nullptr;
  if (classDecl == nullptr) {
    return nullptr;
  }
  this->classDeclarer->completeClass(classDecl);
  if (this->classDeclarer->methodsNamed(classDecl, kAsyncIteratorMethod).empty() &&
      this->classBases->findField(classDecl, kAsyncIteratorMethod) == nullptr) {
    return nullptr;
  }
  auto *member = this->state.getAst().make<MemberExpr>(expr->loc(), expr, kAsyncIteratorMethod);
  auto *call = this->state.getAst().make<CallExpr>(expr->loc(), member, std::vector<Expr *>{});
  auto *result = this->methodCallChecker->checkMethodCall(call, member);
  if (result->type->isError() || result->type->isAsyncIterator()) {
    return result;
  }
  auto given = this->asyncIteratorGiven(type, result);
  if (given.isErr()) {
    this->state.getDiagnostics().error(expr->loc(), given.error().message);
  }
  result->type = Type::error();

  return result;
}

const Type *IterableChecker::elementTypeOf(const Type *type) {
  if (type == nullptr || type->isError()) {
    return nullptr;
  }
  if (type->isParam()) {
    return type->iterated();
  }
  if (type->isMap()) {
    return Type::tuple({type->keyType(), type->valueType()});
  }
  if (type->isSet()) {
    return type->keyType();
  }
  if (type->intlKind() == IntlKind::Segments) {
    return this->intlChecker->segmentDataType();
  }
  const auto *element = this->generatorChecker->iteratedType(type);
  if (element != nullptr) {
    return element;
  }
  auto *classDecl = this->classDeclarer->classOf(type);
  if (classDecl == nullptr) {
    return nullptr;
  }
  this->classDeclarer->completeClass(classDecl);
  auto methods = this->classDeclarer->methodsNamed(classDecl, kIteratorMethod);
  if (methods.size() != 1 || methods[0]->sym == nullptr) {
    return nullptr;
  }
  const auto *result = methods[0]->sym->getReturnType();
  if (result == nullptr || result->isError()) {
    return nullptr;
  }

  return result->isIterator() ? result->yielded() : this->nextValueType(result);
}

bool IterableChecker::isIterableRef(const TypeRef &ref) const {
  return ref.kind() == TypeRefKind::GENERIC_REF &&
         TypeNameTable::isName(ref.name, TypeName::ITERABLE) &&
         this->typeDeclarer->lookupType(ref.name) == nullptr;
}

IterableChecker::ResultIteratorGiven IterableChecker::iteratorGiven(const Type *type,
                                                                    const Expr *result) const {
  if (!result->type->isError() && !result->type->isIterator()) {
    auto message = ErrorTextIteratorGiven::notIterator(type->name(), result->type->name());

    return err(IterableCheckerIteratorGivenError{
        .code = IterableCheckerIteratorGivenErrorCode::NOT_AN_ITERATOR,
        .message = message,
    });
  }

  return ok(result);
}

IterableChecker::ResultAsyncIterator IterableChecker::asyncIteratorGiven(const Type *type,
                                                                         const Expr *result) const {
  if (!result->type->isError() && !result->type->isAsyncIterator()) {
    auto message = ErrorTextAsyncGiven::notAsync(type->name(), result->type->name());

    return err(IterableCheckerAsyncIteratorGivenError{
        .code = IterableCheckerAsyncIteratorGivenErrorCode::NOT_AN_ASYNC_ITERATOR,
        .message = message,
    });
  }

  return ok(result);
}

IterableChecker::ResultIterableParam IterableChecker::iterableParam(FunctionDecl *functionDecl,
                                                                    FuncSymbol *function,
                                                                    const TypeRef &ref) {
  if (!this->isIterableRef(ref)) {
    return err(IterableCheckerIterableParamError{
        .code = IterableCheckerIterableParamErrorCode::NOT_ITERABLE,
        .message = "",
    });
  }
  if (ref.args.size() != 1) {
    auto message = ErrorTextIterableParam::typeArgs();

    return err(IterableCheckerIterableParamError{
        .code = IterableCheckerIterableParamErrorCode::WRONG_ARGUMENT_COUNT,
        .message = message,
    });
  }
  const auto *element = this->typeResolver->resolveType(ref.args[0]);
  if (element->isError()) {
    return ok(element);
  }
  if (functionDecl->isConstructor()) {
    auto message = ErrorTextIterableParam::inCtor(element->name());

    return err(IterableCheckerIterableParamError{
        .code = IterableCheckerIterableParamErrorCode::CONSTRUCTOR_ITERABLE,
        .message = message,
    });
  }
  if (functionDecl->var != nullptr && !functionDecl->typeParams.empty()) {
    return ok(Type::error());
  }
  if (functionDecl->isClosure() || functionDecl->var != nullptr) {
    auto message = ErrorTextIterableParam::notTop(element->name());

    return err(IterableCheckerIterableParamError{
        .code = IterableCheckerIterableParamErrorCode::CLOSURE_ITERABLE,
        .message = message,
    });
  }

  auto *param = Type::param("Iterable<" + element->name().str() + ">");
  param->setIterated(element);
  TypeParamDecl typeParam;
  typeParam.name = this->state.getAst().hiddenName();
  typeParam.loc = ref.loc;
  functionDecl->typeParams.push_back(typeParam);
  functionDecl->typeArgs.push_back(param);
  function->typeParams.push_back(param);
  function->typeDefaults.push_back(nullptr);

  return ok(param);
}

}
