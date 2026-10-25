#include "compiler/check/iterators/iterable_checker/iterable_checker.h"
#include "compiler/check/classes/class_bases/class_bases.h"
#include "compiler/check/classes/method_call_checker/method_call_checker.h"
#include "compiler/check/flow/flow_checker/flow_checker.h"
#include "compiler/check/functions/function_checker/function_checker.h"
#include "compiler/check/generics/generic_instantiator/generic_instantiator.h"
#include "compiler/check/iterators/generator_checker/generator_checker.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/check/types/type_resolver/type_resolver.h"
#include "compiler/declare/class_declarer/class_declarer.h"
#include "compiler/declare/type_declarer/type_declarer.h"
#include "compiler/infer/return_inferrer/return_inferrer.h"
#include "compiler/library/intl/intl_checker/intl_checker.h"
#include "compiler/load/lexer/lexer.h"
#include "compiler/load/parser/parser.h"
#include "compiler/whole_program/flows/flows.h"
#include "llvm/Support/MemoryBuffer.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

constexpr llvm::StringLiteral kIteratorMethod = "[Symbol.iterator]";

constexpr llvm::StringLiteral kAsyncIteratorMethod = "[Symbol.asyncIterator]";

// The generator that iterates an iterator written by hand. A copy is made
// for each type of iterator, with the type of its values; its body is only
// ever checked with them.
constexpr llvm::StringLiteral kIterateSource = R"(function* iterate<I, V>(it: I): Generator<V> {
  while (true) {
    const r = it.next()
    if (r.done) return
    yield r.value
  }
}
)";

} // namespace

IterableChecker::IterableChecker(Checker &checker) : checker_(checker) {}

FuncSymbol *IterableChecker::iterateFunction() {
  if (iterate_) {
    return iterate_;
  }
  // In a buffer of its own, which messages can show.
  std::unique_ptr<llvm::MemoryBuffer> buffer =
      llvm::MemoryBuffer::getMemBuffer(kIterateSource, "<tz>");
  llvm::StringRef text = buffer->getBuffer();
  checker_.diags_.sourceMgr().AddNewSourceBuffer(std::move(buffer), llvm::SMLoc());
  Lexer lexer(text, checker_.diags_);
  Parser parser(lexer, checker_.ctx_, checker_.diags_);
  FunctionDecl *fn = parser.parseFunctionCopy();
  // No program can name it: it is declared in no scope.
  std::vector<llvm::StringMap<Stmt *>> outer = checker_.typeScopes_;
  checker_.typeScopes_.resize(1);
  checker_.functionChecker().declareFunction(fn, nullptr, /*scoped=*/false);
  checker_.typeScopes_ = std::move(outer);
  iterate_ = fn->sym;
  // Each copy is for the very type of the iterator, whose next() it calls.
  checker_.exactParams_[iterate_] = {true, true};

  return iterate_;
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
  // The function next(), which needs no arguments: a method, or a function
  // in a field.
  const Type *next = nullptr;
  if (ClassDecl *cls = ClassDeclarer::classOf(type)) {
    checker_.classDeclarer().completeClass(cls);
    for (FunctionDecl *fn : ClassDeclarer::methodsNamed(cls, "next", /*isStatic=*/false)) {
      if (fn->sym && fn->sym->minArgs == 0 && fn->typeParams.empty()) {
        next = checker_.returnInferrer().functionType(fn->sym);
      }
    }
  }
  if (const Field *field = type->field("next"); !next && field && field->type->isFunction()) {
    next = field->type;
  }
  if (!next || next->minArgs() != 0 || next->ret()->isError()) {
    return nullptr;
  }
  // What it gives: `{ done, value }`, or a union of those, of which the
  // value is that of the members that may not be done.
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
      continue; // what it gives when it is done
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
  const Type *value = nextValueType(type);
  if (!value) {
    return nullptr;
  }
  // `iterate(it)`: the copy of the generator for this iterator.
  FuncSymbol *iterate = iterateFunction();
  llvm::SMLoc loc = it->loc();
  auto *callee = checker_.ctx_.make<NameExpr>(loc, iterate->name);
  auto *call = checker_.ctx_.make<CallExpr>(loc, callee, std::vector<Expr *>{it});
  call->func = iterate;
  if (!checker_.trial_) {
    if (FuncSymbol *instance = checker_.genericInstantiator().instantiate(
            iterate, {type->nonOptional(), value}, loc)) {
      call->func = instance;
    }
  }
  callee->func = call->func;
  call->type = Type::iterator(value);
  callee->type = Type::function({type->nonOptional()}, 1, false, call->type);
  if (type->isOptional()) {
    call->args[0] = checker_.typeRelations().convert(it, type->nonOptional());
  }
  checker_.flowChecker().calledFunction(); // next() runs

  return call;
}

Expr *IterableChecker::userIterator(Expr *expr) {
  const Type *type = expr->type;
  ClassDecl *cls = type ? ClassDeclarer::classOf(type) : nullptr;
  if (!cls) {
    return nullptr;
  }
  checker_.classDeclarer().completeClass(cls);
  if (ClassDeclarer::methodsNamed(cls, kIteratorMethod, /*isStatic=*/false).empty() &&
      !ClassBases::findField(cls, kIteratorMethod)) {
    return nullptr;
  }
  // `x[Symbol.iterator]()`, called on the object, which is checked.
  auto *member = checker_.ctx_.make<MemberExpr>(expr->loc(), expr, kIteratorMethod);
  auto *call = checker_.ctx_.make<CallExpr>(expr->loc(), member, std::vector<Expr *>{});
  Expr *result = checker_.methodCallChecker().checkMethodCall(call, member);
  if (result->type->isError() || result->type->isIterator()) {
    return result;
  }
  if (Expr *it = iteratorOf(result)) {
    return it;
  }
  checker_.diags_.error(expr->loc(),
                        "'[Symbol.iterator]()' of '" + type->name() + "' gives '" +
                            result->type->name() +
                            "', which is not an iterator: it needs a method 'next()' that gives "
                            "'{ done, value }' (IteratorResult<T>); or make it a generator, "
                            "'*[Symbol.iterator]() { yield ... }'");
  result->type = Type::error();

  return result;
}

const Type *IterableChecker::asyncIterableType(const Type *type) {
  ClassDecl *cls = type ? ClassDeclarer::classOf(type) : nullptr;
  if (!cls) {
    return nullptr;
  }
  checker_.classDeclarer().completeClass(cls);
  std::vector<FunctionDecl *> methods =
      ClassDeclarer::methodsNamed(cls, kAsyncIteratorMethod, /*isStatic=*/false);
  if (methods.size() != 1 || !methods[0]->sym) {
    return nullptr;
  }
  const Type *result = checker_.returnInferrer().returnType(methods[0]->sym);

  return result && result->isAsyncIterator() ? result : nullptr;
}

Expr *IterableChecker::asyncUserIterator(Expr *expr) {
  const Type *type = expr->type;
  ClassDecl *cls = type ? ClassDeclarer::classOf(type) : nullptr;
  if (!cls) {
    return nullptr;
  }
  checker_.classDeclarer().completeClass(cls);
  if (ClassDeclarer::methodsNamed(cls, kAsyncIteratorMethod, /*isStatic=*/false).empty() &&
      !ClassBases::findField(cls, kAsyncIteratorMethod)) {
    return nullptr;
  }
  // `x[Symbol.asyncIterator]()`, called on the object, which is checked.
  auto *member = checker_.ctx_.make<MemberExpr>(expr->loc(), expr, kAsyncIteratorMethod);
  auto *call = checker_.ctx_.make<CallExpr>(expr->loc(), member, std::vector<Expr *>{});
  Expr *result = checker_.methodCallChecker().checkMethodCall(call, member);
  if (result->type->isError() || result->type->isAsyncIterator()) {
    return result;
  }
  checker_.diags_.error(expr->loc(),
                        "'[Symbol.asyncIterator]()' of '" + type->name() + "' gives '" +
                            result->type->name() +
                            "', which is not an async iterator; make it an async generator, "
                            "'async *[Symbol.asyncIterator]() { yield ... }'");
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
  // What Intl.Segmenter's segment() gives: its segments.
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
  checker_.classDeclarer().completeClass(cls);
  std::vector<FunctionDecl *> methods =
      ClassDeclarer::methodsNamed(cls, kIteratorMethod, /*isStatic=*/false);
  if (methods.size() != 1 || !methods[0]->sym) {
    return nullptr;
  }
  const Type *result = checker_.returnInferrer().returnType(methods[0]->sym);
  if (!result || result->isError()) {
    return nullptr;
  }

  return result->isIterator() ? result->yielded() : nextValueType(result);
}

bool IterableChecker::isIterableRef(const TypeRef &ref) const {
  return ref.kind == TypeRef::Kind::Generic && ref.name == "Iterable" &&
         !checker_.typeDeclarer().lookupType("Iterable");
}

const Type *IterableChecker::iterableParam(FunctionDecl *fn, FuncSymbol *sym, const TypeRef &ref) {
  if (!isIterableRef(ref)) {
    return nullptr;
  }
  if (ref.args.size() != 1) {
    checker_.diags_.error(
        ref.loc,
        "'Iterable' takes one type argument, the type of its values, e.g. 'Iterable<int>'");
    return Type::error();
  }
  const Type *element = checker_.typeResolver().resolveType(ref.args[0], /*allowVoid=*/false);
  if (element->isError()) {
    return element;
  }
  if (fn->isConstructor) {
    checker_.diags_.error(ref.loc,
                          "a constructor cannot take an 'Iterable<" + element->name() +
                              ">' yet: it would make a copy of the constructor for each kind of "
                              "iterable; take an array, e.g. '" +
                              element->name() +
                              "[]', and pass "
                              "'[...xs]'");
    return Type::error();
  }
  if (fn->var && !fn->typeParams.empty()) { // reported generic already
    return Type::error();
  }
  if (fn->isClosure() || fn->var) {
    checker_.diags_.error(ref.loc,
                          "a parameter 'Iterable<" + element->name() +
                              ">' makes the function generic, and a generic function can be "
                              "declared only at the top level for now; take an array, e.g. '" +
                              element->name() + "[]'");
    return Type::error();
  }
  // A type parameter of its own, which the function's copies make the type
  // of the argument.
  Type *param = Type::param("Iterable<" + element->name().str() + ">");
  param->setIterated(element);
  TypeParamDecl decl;
  decl.name = checker_.ctx_.hiddenName();
  decl.loc = ref.loc;
  fn->typeParams.push_back(decl);
  fn->typeArgs.push_back(param);
  sym->typeParams.push_back(param);
  sym->typeDefaults.push_back(nullptr);

  return param;
}

} // namespace tz
