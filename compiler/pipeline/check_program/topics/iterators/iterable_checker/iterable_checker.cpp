#include "compiler/pipeline/check_program/topics/iterators/iterable_checker/iterable_checker.h"
#include "compiler/load/lexer/lexer.h"
#include "compiler/load/parser/parser.h"
#include "compiler/pipeline/check_program/declare/class_declarer/class_declarer.h"
#include "compiler/pipeline/check_program/declare/type_declarer/type_declarer.h"
#include "compiler/pipeline/check_program/flow/flow_checker/flow_checker.h"
#include "compiler/pipeline/check_program/generics/generic_instantiator/generic_instantiator.h"
#include "compiler/pipeline/check_program/infer/return_inferrer/return_inferrer.h"
#include "compiler/pipeline/check_program/topics/classes/class_bases/class_bases.h"
#include "compiler/pipeline/check_program/topics/classes/method_call_checker/method_call_checker.h"
#include "compiler/pipeline/check_program/topics/functions/function_checker/function_checker.h"
#include "compiler/pipeline/check_program/topics/intl/intl_checker/intl_checker.h"
#include "compiler/pipeline/check_program/topics/iterators/generator_checker/generator_checker.h"
#include "compiler/pipeline/check_program/types/type_relations/type_relations.h"
#include "compiler/pipeline/check_program/types/type_resolver/type_resolver.h"
#include "compiler/pipeline/check_program/whole_program/flows/flows.h"
#include "llvm/Support/MemoryBuffer.h"

namespace tz {

namespace {

constexpr llvm::StringLiteral kIteratorMethod = "[Symbol.iterator]";

constexpr llvm::StringLiteral kAsyncIteratorMethod = "[Symbol.asyncIterator]";

constexpr llvm::StringLiteral kIterateSource = R"(function* iterate<I, V>(it: I): Generator<V> {
  while (true) {
    const r = it.next()
    if (r.done) return
    yield r.value
  }
}
)";

}

IterableChecker::IterableChecker(CheckState &owner)
    : state(owner) {}

FuncSymbol *IterableChecker::iterateFunction() {
  if (FuncSymbol *known = this->state.getExpressionMarks().getIterateSymbol()) {
    return known;
  }
  std::unique_ptr<llvm::MemoryBuffer> buffer =
      llvm::MemoryBuffer::getMemBuffer(kIterateSource, "<tz>");
  llvm::StringRef text = buffer->getBuffer();
  this->state.getDiagnostics().sourceMgr().AddNewSourceBuffer(std::move(buffer), llvm::SMLoc());
  Lexer lexer(text, this->state.getDiagnostics());
  Parser parser(lexer, this->state.getAst(), this->state.getDiagnostics());
  FunctionDecl *fn = parser.parseFunctionCopy();
  this->state.getScopeStack().enterOnlyTypes(this->state.getScopeStack().getTopTypes());
  FunctionChecker(this->state).declareFunction(fn, nullptr, false);
  this->state.getScopeStack().leaveOnlyTypes();
  this->state.getExpressionMarks().rememberIterateSymbol(fn->sym);
  this->state.getGenericStore().rememberExactParams(fn->sym, {true, true});

  return fn->sym;
}

const Type *IterableChecker::iteratorResultType(const Type *value, const Type *returned) {
  if (!returned || returned->isVoid()) {
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
  if (ClassDecl *cls = ClassDeclarer::classOf(type)) {
    ClassDeclarer(this->state).completeClass(cls);
    for (FunctionDecl *fn : ClassDeclarer::methodsNamed(cls, "next", false)) {
      if (fn->sym && fn->sym->minArgs == 0 && fn->typeParams.empty()) {
        next = ReturnInferrer(this->state).functionType(fn->sym);
      }
    }
  }
  if (const Field *field = type->field("next"); !next && field && field->type->isFunction()) {
    next = field->type;
  }
  if (!next || next->minArgs() != 0 || next->ret()->isError()) {
    return nullptr;
  }
  const Type *result = next->ret();
  llvm::ArrayRef<const Type *> members =
      result->isUnion() ? result->members() : llvm::ArrayRef<const Type *>(result);
  std::vector<const Type *> values;
  for (const Type *m : members) {
    const Field *done = m->isObject() ? m->field("done") : nullptr;
    const Field *value = m->isObject() ? m->field("value") : nullptr;
    if (!done || !value ||
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
  const Type *value = Type::unionOf(values);

  return value->isUndefined() || value->isVoid() ? nullptr : value;
}

Expr *IterableChecker::iteratorOf(Expr *it) {
  const Type *type = it->type;
  if (type->isIterator() || type->isError()) {
    return it;
  }
  const Type *value = this->nextValueType(type);
  if (!value) {
    return nullptr;
  }
  FuncSymbol *iterate = this->iterateFunction();
  llvm::SMLoc loc = it->loc();
  auto *callee = this->state.getAst().make<NameExpr>(loc, iterate->name);
  auto *call = this->state.getAst().make<CallExpr>(loc, callee, std::vector<Expr *>{it});
  call->target = FunctionCall{.func = iterate};
  if (!this->state.getFunctionStack().isInsideTrial()) {
    if (FuncSymbol *instance = GenericInstantiator(this->state)
                                   .instantiate(iterate, {type->nonOptional(), value}, loc)) {
      call->target = FunctionCall{.func = instance};
    }
  }
  callee->binding = NamedFunction{.func = call->func()};
  call->type = Type::iterator(value);
  callee->type = Type::function({type->nonOptional()}, 1, false, call->type);
  if (type->isOptional()) {
    call->args[0] = TypeRelations(this->state).convert(it, type->nonOptional());
  }
  FlowChecker(this->state).calledFunction();

  return call;
}

Expr *IterableChecker::userIterator(Expr *expr) {
  const Type *type = expr->type;
  ClassDecl *cls = type ? ClassDeclarer::classOf(type) : nullptr;
  if (!cls) {
    return nullptr;
  }
  ClassDeclarer(this->state).completeClass(cls);
  if (ClassDeclarer::methodsNamed(cls, kIteratorMethod, false).empty() &&
      !ClassBases::findField(cls, kIteratorMethod)) {
    return nullptr;
  }
  auto *member = this->state.getAst().make<MemberExpr>(expr->loc(), expr, kIteratorMethod);
  auto *call = this->state.getAst().make<CallExpr>(expr->loc(), member, std::vector<Expr *>{});
  Expr *result = MethodCallChecker(this->state).checkMethodCall(call, member);
  if (result->type->isError() || result->type->isIterator()) {
    return result;
  }
  if (Expr *it = this->iteratorOf(result)) {
    return it;
  }
  ResultIteratorGiven given = IterableChecker::iteratorGiven(type, result);
  if (given.isErr()) {
    this->state.getDiagnostics().error(expr->loc(), given.error().message);
  }
  result->type = Type::error();

  return result;
}

const Type *IterableChecker::asyncIterableType(const Type *type) {
  ClassDecl *cls = type ? ClassDeclarer::classOf(type) : nullptr;
  if (!cls) {
    return nullptr;
  }
  ClassDeclarer(this->state).completeClass(cls);
  std::vector<FunctionDecl *> methods =
      ClassDeclarer::methodsNamed(cls, kAsyncIteratorMethod, false);
  if (methods.size() != 1 || !methods[0]->sym) {
    return nullptr;
  }
  const Type *result = ReturnInferrer(this->state).returnType(methods[0]->sym);

  return result && result->isAsyncIterator() ? result : nullptr;
}

Expr *IterableChecker::asyncUserIterator(Expr *expr) {
  const Type *type = expr->type;
  ClassDecl *cls = type ? ClassDeclarer::classOf(type) : nullptr;
  if (!cls) {
    return nullptr;
  }
  ClassDeclarer(this->state).completeClass(cls);
  if (ClassDeclarer::methodsNamed(cls, kAsyncIteratorMethod, false).empty() &&
      !ClassBases::findField(cls, kAsyncIteratorMethod)) {
    return nullptr;
  }
  auto *member = this->state.getAst().make<MemberExpr>(expr->loc(), expr, kAsyncIteratorMethod);
  auto *call = this->state.getAst().make<CallExpr>(expr->loc(), member, std::vector<Expr *>{});
  Expr *result = MethodCallChecker(this->state).checkMethodCall(call, member);
  if (result->type->isError() || result->type->isAsyncIterator()) {
    return result;
  }
  ResultAsyncIterator given = IterableChecker::asyncIteratorGiven(type, result);
  if (given.isErr()) {
    this->state.getDiagnostics().error(expr->loc(), given.error().message);
  }
  result->type = Type::error();

  return result;
}

const Type *IterableChecker::elementTypeOf(const Type *type) {
  if (!type || type->isError()) {
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
    return IntlChecker::segmentDataType();
  }
  if (const Type *element = GeneratorChecker::iteratedType(type)) {
    return element;
  }
  ClassDecl *cls = ClassDeclarer::classOf(type);
  if (!cls) {
    return nullptr;
  }
  ClassDeclarer(this->state).completeClass(cls);
  std::vector<FunctionDecl *> methods = ClassDeclarer::methodsNamed(cls, kIteratorMethod, false);
  if (methods.size() != 1 || !methods[0]->sym) {
    return nullptr;
  }
  const Type *result = ReturnInferrer(this->state).returnType(methods[0]->sym);
  if (!result || result->isError()) {
    return nullptr;
  }

  return result->isIterator() ? result->yielded() : this->nextValueType(result);
}

bool IterableChecker::isIterableRef(const TypeRef &ref) const {
  return ref.kind() == TypeRefKind::GENERIC_REF && ref.name == "Iterable" &&
         !TypeDeclarer(this->state).lookupType("Iterable");
}

IterableChecker::ResultIteratorGiven IterableChecker::iteratorGiven(const Type *type,
                                                                    const Expr *result) {
  if (!result->type->isError() && !result->type->isIterator()) {
    std::string message = ErrorTextIteratorGiven::notIterator(type->name(), result->type->name());

    return err(IterableCheckerIteratorGivenError{
        .code = IterableCheckerIteratorGivenErrorCode::NOT_AN_ITERATOR,
        .message = message,
    });
  }

  return ok(result);
}

IterableChecker::ResultAsyncIterator IterableChecker::asyncIteratorGiven(const Type *type,
                                                                         const Expr *result) {
  if (!result->type->isError() && !result->type->isAsyncIterator()) {
    std::string message = ErrorTextAsyncGiven::notAsync(type->name(), result->type->name());

    return err(IterableCheckerAsyncIteratorGivenError{
        .code = IterableCheckerAsyncIteratorGivenErrorCode::NOT_AN_ASYNC_ITERATOR,
        .message = message,
    });
  }

  return ok(result);
}

IterableChecker::ResultIterableParam
IterableChecker::iterableParam(FunctionDecl *fn, FuncSymbol *sym, const TypeRef &ref) {
  if (!this->isIterableRef(ref)) {
    return err(IterableCheckerIterableParamError{
        .code = IterableCheckerIterableParamErrorCode::NOT_ITERABLE,
        .message = "",
    });
  }
  if (ref.args.size() != 1) {
    std::string message = ErrorTextIterableParam::typeArgs();

    return err(IterableCheckerIterableParamError{
        .code = IterableCheckerIterableParamErrorCode::WRONG_ARGUMENT_COUNT,
        .message = message,
    });
  }
  const Type *element = TypeResolver(this->state).resolveType(ref.args[0], false);
  if (element->isError()) {
    return ok(element);
  }
  if (fn->isConstructor()) {
    std::string message = ErrorTextIterableParam::inCtor(element->name());

    return err(IterableCheckerIterableParamError{
        .code = IterableCheckerIterableParamErrorCode::CONSTRUCTOR_ITERABLE,
        .message = message,
    });
  }
  if (fn->var && !fn->typeParams.empty()) {
    return ok(Type::error());
  }
  if (fn->isClosure() || fn->var) {
    std::string message = ErrorTextIterableParam::notTop(element->name());

    return err(IterableCheckerIterableParamError{
        .code = IterableCheckerIterableParamErrorCode::CLOSURE_ITERABLE,
        .message = message,
    });
  }

  Type *param = Type::param("Iterable<" + element->name().str() + ">");
  param->setIterated(element);
  TypeParamDecl decl;
  decl.name = this->state.getAst().hiddenName();
  decl.loc = ref.loc;
  fn->typeParams.push_back(decl);
  fn->typeArgs.push_back(param);
  sym->typeParams.push_back(param);
  sym->typeDefaults.push_back(nullptr);

  return ok(param);
}

}
