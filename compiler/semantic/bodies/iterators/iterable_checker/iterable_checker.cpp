#include "compiler/semantic/bodies/iterators/iterable_checker/iterable_checker.h"
#include "compiler/program/library/builtin_names/type_name_table/type_name_table.h"
#include "compiler/program/library/iterator/iterator_method_table/iterator_method_table.h"
#include "compiler/program/library/iterator/iterator_result_field_table/iterator_result_field_table.h"
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
  auto topTypes = this->state.getScopeStack().getTopTypes();
  this->state.getScopeStack().enterOnlyTypes(topTypes);
  this->functionChecker->declareSignature(functionDecl);
  this->state.getScopeStack().leaveOnlyTypes();
  this->state.getExpressionMarks().rememberIterateSymbol(functionDecl->sym);
  this->state.getGenericStore().rememberExactParams(functionDecl->sym, {true, true});

  return functionDecl->sym;
}

const Type *IterableChecker::iteratorResultType(const Type *value, const Type *returned) const {
  bool isNothingReturned = returned == nullptr || returned->isVoid();
  const Type *ending = isNothingReturned ? Type::undefinedTy() : returned;
  const auto *going = Type::object(
      {{IteratorResultFieldTable::getName(IteratorResultField::DONE).str(),
        Type::boolLiteral(false), false},
       {IteratorResultFieldTable::getName(IteratorResultField::VALUE).str(), value, false}});
  const auto *finished = Type::object(
      {{IteratorResultFieldTable::getName(IteratorResultField::DONE).str(), Type::boolLiteral(true),
        false},
       {IteratorResultFieldTable::getName(IteratorResultField::VALUE).str(), ending, false}});

  return Type::unionOf({going, finished});
}

const Type *IterableChecker::nextValueType(const Type *type) {
  const Type *object = type->nonOptional();
  if (!object->isObject() || object->isTuple()) {
    return nullptr;
  }
  const auto *next = this->nextFunctionType(object);
  if (next == nullptr || next->minArgs() != 0 || next->ret()->isError()) {
    return nullptr;
  }

  return this->nextResultValue(next->ret());
}

const Type *IterableChecker::nextFunctionType(const Type *object) {
  const auto *method = this->nextMethodType(object);
  if (method != nullptr) {
    return method;
  }
  const auto *field = object->field(IteratorMethodTable::getName(IteratorMethod::NEXT));
  if (field != nullptr && field->type->isFunction()) {
    return field->type;
  }

  return nullptr;
}

const Type *IterableChecker::nextMethodType(const Type *object) {
  auto *classDecl = this->classDeclarer->classOf(object);
  if (classDecl == nullptr) {
    return nullptr;
  }
  this->classDeclarer->completeClass(classDecl);
  auto methods = this->classDeclarer->methodsNamed(
      classDecl, IteratorMethodTable::getName(IteratorMethod::NEXT));
  auto methodTypes = std::vector<const Type *>();
  for (FunctionDecl *functionDecl : methods) {
    if (this->isPlainNext(functionDecl)) {
      const auto *methodType = this->returnInferrer->functionType(functionDecl->sym);
      methodTypes.push_back(methodType);
    }
  }

  return methodTypes.empty() ? nullptr : methodTypes.back();
}

bool IterableChecker::isPlainNext(const FunctionDecl *functionDecl) const {
  return functionDecl->sym != nullptr && functionDecl->sym->minArgs == 0 &&
         functionDecl->typeParams.empty();
}

const Type *IterableChecker::nextResultValue(const Type *result) const {
  llvm::ArrayRef<const Type *> members =
      result->isUnion() ? result->members() : llvm::ArrayRef<const Type *>(result);
  bool isResultShaped =
      llvm::all_of(members, std::bind_front(&IterableChecker::isIteratorResultShape, this));
  if (!isResultShaped) {
    return nullptr;
  }
  auto values = std::vector<const Type *>();
  for (const Type *member : members) {
    if (!this->isFinishedResult(member)) {
      values.push_back(
          member->field(IteratorResultFieldTable::getName(IteratorResultField::VALUE))->type);
    }
  }
  if (values.empty()) {
    return nullptr;
  }
  const auto *value = Type::unionOf(values);

  return value->isUndefined() || value->isVoid() ? nullptr : value;
}

bool IterableChecker::isIteratorResultShape(const Type *member) const {
  if (!member->isObject()) {
    return false;
  }
  const Field *done = member->field(IteratorResultFieldTable::getName(IteratorResultField::DONE));
  const Field *value = member->field(IteratorResultFieldTable::getName(IteratorResultField::VALUE));
  if (done == nullptr || value == nullptr) {
    return false;
  }
  bool isBoolLiteral = done->type->isLiteral() && done->type->literalBase()->isBool();

  return done->type->isBool() || isBoolLiteral;
}

bool IterableChecker::isFinishedResult(const Type *member) const {
  const Field *done = member->field(IteratorResultFieldTable::getName(IteratorResultField::DONE));

  return done->type->isLiteral() && done->type->literalNumber() != 0;
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
  auto *target = this->iterateTarget(iterate, type, value, loc);
  call->target = FunctionCall{.func = target};
  callee->binding = NamedFunction{.func = call->func()};
  call->type = Type::iterator(value);
  callee->type = Type::function({type->nonOptional()}, 1, false, call->type);
  if (type->isOptional()) {
    call->args[0] = this->typeRelations->convert(it, type->nonOptional());
  }
  this->flowChecker->calledFunction();

  return call;
}

FuncSymbol *IterableChecker::iterateTarget(FuncSymbol *iterate, const Type *type, const Type *value,
                                           llvm::SMLoc loc) {
  if (this->state.getFunctionStack().isInsideTrial()) {
    return iterate;
  }
  auto *instance =
      this->genericInstantiator->instantiate(iterate, {type->nonOptional(), value}, loc);

  return instance != nullptr ? instance : iterate;
}

Expr *IterableChecker::userIterator(Expr *expr) {
  const Type *type = expr->type;
  auto *result = this->callSymbolMethod(expr, kIteratorMethod);
  if (result == nullptr || result->type->isError() || result->type->isIterator()) {
    return result;
  }
  auto *iterator = this->iteratorOf(result);
  if (iterator != nullptr) {
    return iterator;
  }
  auto given = this->checkIteratorGiven(type, result);
  if (given.isErr()) {
    this->state.getDiagnostics().error(expr->loc(), given.error().message);
  }
  result->type = Type::error();

  return result;
}

Expr *IterableChecker::asyncUserIterator(Expr *expr) {
  const Type *type = expr->type;
  auto *result = this->callSymbolMethod(expr, kAsyncIteratorMethod);
  if (result == nullptr || result->type->isError() || result->type->isAsyncIterator()) {
    return result;
  }
  auto given = this->checkAsyncIteratorGiven(type, result);
  if (given.isErr()) {
    this->state.getDiagnostics().error(expr->loc(), given.error().message);
  }
  result->type = Type::error();

  return result;
}

Expr *IterableChecker::callSymbolMethod(Expr *expr, llvm::StringRef name) {
  const Type *type = expr->type;
  ClassDecl *classDecl = type != nullptr ? this->classDeclarer->classOf(type) : nullptr;
  if (classDecl == nullptr) {
    return nullptr;
  }
  this->classDeclarer->completeClass(classDecl);
  auto methods = this->classDeclarer->methodsNamed(classDecl, name);
  bool hasMethod = !methods.empty() || this->classBases->findField(classDecl, name) != nullptr;
  if (!hasMethod) {
    return nullptr;
  }
  auto *member = this->state.getAst().make<MemberExpr>(expr->loc(), expr, name);
  auto *call = this->state.getAst().make<CallExpr>(expr->loc(), member, std::vector<Expr *>{});

  return this->methodCallChecker->checkMethodCall(call, member);
}

const Type *IterableChecker::asyncIterableType(const Type *type) {
  const auto *result = this->soleMethodReturn(type, kAsyncIteratorMethod);

  return result != nullptr && result->isAsyncIterator() ? result : nullptr;
}

const Type *IterableChecker::soleMethodReturn(const Type *type, llvm::StringRef name) {
  ClassDecl *classDecl = type != nullptr ? this->classDeclarer->classOf(type) : nullptr;
  if (classDecl == nullptr) {
    return nullptr;
  }
  this->classDeclarer->completeClass(classDecl);
  auto methods = this->classDeclarer->methodsNamed(classDecl, name);
  if (methods.size() != 1 || methods[0]->sym == nullptr) {
    return nullptr;
  }

  return methods[0]->sym->getReturnType();
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
  const auto *result = this->soleMethodReturn(type, kIteratorMethod);
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

IterableChecker::ResultIteratorGiven IterableChecker::checkIteratorGiven(const Type *type,
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

IterableChecker::ResultAsyncIterator
IterableChecker::checkAsyncIteratorGiven(const Type *type, const Expr *result) const {
  if (!result->type->isError() && !result->type->isAsyncIterator()) {
    auto message = ErrorTextAsyncGiven::notAsync(type->name(), result->type->name());

    return err(IterableCheckerAsyncIteratorGivenError{
        .code = IterableCheckerAsyncIteratorGivenErrorCode::NOT_AN_ASYNC_ITERATOR,
        .message = message,
    });
  }

  return ok(result);
}

IterableChecker::ResultIterableParam IterableChecker::checkIterableParam(FunctionDecl *functionDecl,
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
  auto *param = this->addIterableTypeParam(functionDecl, function, element, ref);

  return ok(param);
}

const Type *IterableChecker::addIterableTypeParam(FunctionDecl *functionDecl, FuncSymbol *function,
                                                  const Type *element, const TypeRef &ref) {
  auto iterable = TypeNameTable::getName(TypeName::ITERABLE);
  auto *param = Type::param(iterable.str() + "<" + element->name().str() + ">");
  param->setIterated(element);
  auto typeParam = TypeParamDecl{
      .name = this->state.getAst().hiddenName(),
      .loc = ref.loc,
  };
  functionDecl->typeParams.push_back(typeParam);
  functionDecl->typeArgs.push_back(param);
  function->typeParams.push_back(param);
  function->typeDefaults.push_back(nullptr);

  return param;
}

}
