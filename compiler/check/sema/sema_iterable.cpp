// Collections of the program (docs/iterators.md, sections 2 and 3).
//
// A class (or an object literal) with a method `[Symbol.iterator]()` is
// iterated through it, wherever JavaScript iterates: `for...of`, spreads,
// destructuring, Array.from, `yield*`, `new Set(x)`, `new Map(x)`. The type
// says whose method it is, so the call is direct. A generator method gives
// an iterator as it is. An iterator written by hand, an object whose next()
// gives `{ done, value }` (IteratorResult), is iterated by a generator that
// calls next() until it is done: `iterate` below, a generic generator whose
// copy for the type of the iterator a consumer emits inline, as a loop that
// calls next() directly, so the `{ done, value }` objects stay out of the
// way of LLVM.
//
// A parameter `xs: Iterable<int>` makes its function generic, with a type
// parameter of its own that anything iterable with ints stands for: each
// kind of iterable gets a copy of the function, which iterates it as code
// written for it would (docs/generics.md).

#include "compiler/load/lexer/lexer.h"
#include "compiler/load/parser/parser.h"
#include "compiler/check/sema/sema.h"

#include "llvm/Support/MemoryBuffer.h"

using llvm::cast;
using llvm::dyn_cast;
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

FuncSymbol *Sema::iterateFunction() {
  if (iterate_)
    return iterate_;
  // In a buffer of its own, which messages can show.
  std::unique_ptr<llvm::MemoryBuffer> buffer = llvm::MemoryBuffer::getMemBuffer(kIterateSource, "<tz>");
  llvm::StringRef text = buffer->getBuffer();
  diags_.sourceMgr().AddNewSourceBuffer(std::move(buffer), llvm::SMLoc());
  Lexer lexer(text, diags_);
  Parser parser(lexer, ctx_, diags_);
  FunctionDecl *fn = parser.parseFunctionCopy();
  // No program can name it: it is declared in no scope.
  std::vector<llvm::StringMap<Stmt *>> outer = typeScopes_;
  typeScopes_.resize(1);
  declareFunction(fn, nullptr, /*scoped=*/false);
  typeScopes_ = std::move(outer);
  iterate_ = fn->sym;
  // Each copy is for the very type of the iterator, whose next() it calls.
  exactParams_[iterate_] = {true, true};
  return iterate_;
}

const Type *Sema::iteratorResultType(const Type *value, const Type *returned) {
  if (!returned || returned->isVoid())
    returned = Type::undefinedTy();
  return Type::unionOf({Type::object({{"done", Type::boolLiteral(false), false}, {"value", value, false}}),
                        Type::object({{"done", Type::boolLiteral(true), false}, {"value", returned, false}})});
}

const Type *Sema::nextValueType(const Type *type) {
  type = type->nonOptional();
  if (!type->isObject() || type->isTuple())
    return nullptr;
  // The function next(), which needs no arguments: a method, or a function
  // in a field.
  const Type *next = nullptr;
  if (ClassDecl *cls = classOf(type)) {
    completeClass(cls);
    for (FunctionDecl *fn : methodsNamed(cls, "next", /*isStatic=*/false))
      if (fn->sym && fn->sym->minArgs == 0 && fn->typeParams.empty())
        next = functionType(fn->sym);
  }
  if (const Field *field = type->field("next"); !next && field && field->type->isFunction())
    next = field->type;
  if (!next || next->minArgs() != 0 || next->ret()->isError())
    return nullptr;
  // What it gives: `{ done, value }`, or a union of those, of which the
  // value is that of the members that may not be done.
  const Type *result = next->ret();
  llvm::ArrayRef<const Type *> members =
      result->isUnion() ? result->members() : llvm::ArrayRef<const Type *>(result);
  std::vector<const Type *> values;
  for (const Type *m : members) {
    const Field *done = m->isObject() ? m->field("done") : nullptr;
    const Field *value = m->isObject() ? m->field("value") : nullptr;
    if (!done || !value || !(done->type->isBool() || (done->type->isLiteral() && done->type->literalBase()->isBool())))
      return nullptr;
    if (done->type->isLiteral() && done->type->literalNumber() != 0)
      continue; // what it gives when it is done
    values.push_back(value->type);
  }
  if (values.empty())
    return nullptr;
  const Type *value = Type::unionOf(values);
  return value->isUndefined() || value->isVoid() ? nullptr : value;
}

Expr *Sema::iteratorOf(Expr *it) {
  const Type *type = it->type;
  if (type->isIterator() || type->isError())
    return it;
  const Type *value = nextValueType(type);
  if (!value)
    return nullptr;
  // `iterate(it)`: the copy of the generator for this iterator.
  FuncSymbol *iterate = iterateFunction();
  llvm::SMLoc loc = it->loc();
  auto *callee = ctx_.make<NameExpr>(loc, iterate->name);
  auto *call = ctx_.make<CallExpr>(loc, callee, std::vector<Expr *>{it});
  call->func = iterate;
  if (!trial_)
    if (FuncSymbol *instance = instantiate(iterate, {type->nonOptional(), value}, loc))
      call->func = instance;
  callee->func = call->func;
  call->type = Type::iterator(value);
  callee->type = Type::function({type->nonOptional()}, 1, false, call->type);
  if (type->isOptional())
    call->args[0] = convert(it, type->nonOptional());
  calledFunction(); // next() runs
  return call;
}

Expr *Sema::userIterator(Expr *expr) {
  const Type *type = expr->type;
  ClassDecl *cls = type ? classOf(type) : nullptr;
  if (!cls)
    return nullptr;
  completeClass(cls);
  if (methodsNamed(cls, kIteratorMethod, /*isStatic=*/false).empty() && !findField(cls, kIteratorMethod))
    return nullptr;
  // `x[Symbol.iterator]()`, called on the object, which is checked.
  auto *member = ctx_.make<MemberExpr>(expr->loc(), expr, kIteratorMethod);
  auto *call = ctx_.make<CallExpr>(expr->loc(), member, std::vector<Expr *>{});
  Expr *result = checkMethodCall(call, member);
  if (result->type->isError() || result->type->isIterator())
    return result;
  if (Expr *it = iteratorOf(result))
    return it;
  diags_.error(expr->loc(), "'[Symbol.iterator]()' of '" + type->name() + "' gives '" + result->type->name() +
                                "', which is not an iterator: it needs a method 'next()' that gives "
                                "'{ done, value }' (IteratorResult<T>); or make it a generator, "
                                "'*[Symbol.iterator]() { yield ... }'");
  result->type = Type::error();
  return result;
}

const Type *Sema::asyncIterableType(const Type *type) {
  ClassDecl *cls = type ? classOf(type) : nullptr;
  if (!cls)
    return nullptr;
  completeClass(cls);
  std::vector<FunctionDecl *> methods = methodsNamed(cls, kAsyncIteratorMethod, /*isStatic=*/false);
  if (methods.size() != 1 || !methods[0]->sym)
    return nullptr;
  const Type *result = returnType(methods[0]->sym);
  return result && result->isAsyncIterator() ? result : nullptr;
}

Expr *Sema::asyncUserIterator(Expr *expr) {
  const Type *type = expr->type;
  ClassDecl *cls = type ? classOf(type) : nullptr;
  if (!cls)
    return nullptr;
  completeClass(cls);
  if (methodsNamed(cls, kAsyncIteratorMethod, /*isStatic=*/false).empty() &&
      !findField(cls, kAsyncIteratorMethod))
    return nullptr;
  // `x[Symbol.asyncIterator]()`, called on the object, which is checked.
  auto *member = ctx_.make<MemberExpr>(expr->loc(), expr, kAsyncIteratorMethod);
  auto *call = ctx_.make<CallExpr>(expr->loc(), member, std::vector<Expr *>{});
  Expr *result = checkMethodCall(call, member);
  if (result->type->isError() || result->type->isAsyncIterator())
    return result;
  diags_.error(expr->loc(), "'[Symbol.asyncIterator]()' of '" + type->name() + "' gives '" +
                                result->type->name() +
                                "', which is not an async iterator; make it an async generator, "
                                "'async *[Symbol.asyncIterator]() { yield ... }'");
  result->type = Type::error();
  return result;
}

const Type *Sema::elementTypeOf(const Type *type) {
  if (!type || type->isError())
    return nullptr;
  if (type->isParam())
    return type->iterated();
  if (type->isMap())
    return Type::tuple({type->keyType(), type->valueType()});
  if (type->isSet())
    return type->keyType();
  // What Intl.Segmenter's segment() gives: its segments.
  if (type->intlKind() == IntlKind::Segments)
    return segmentDataType();
  if (const Type *element = iteratedType(type))
    return element;
  ClassDecl *cls = classOf(type);
  if (!cls)
    return nullptr;
  completeClass(cls);
  std::vector<FunctionDecl *> methods = methodsNamed(cls, kIteratorMethod, /*isStatic=*/false);
  if (methods.size() != 1 || !methods[0]->sym)
    return nullptr;
  const Type *result = returnType(methods[0]->sym);
  if (!result || result->isError())
    return nullptr;
  return result->isIterator() ? result->yielded() : nextValueType(result);
}

bool Sema::isIterableRef(const TypeRef &ref) const {
  return ref.kind == TypeRef::Kind::Generic && ref.name == "Iterable" && !lookupType("Iterable");
}

const Type *Sema::iterableParam(FunctionDecl *fn, FuncSymbol *sym, const TypeRef &ref) {
  if (!isIterableRef(ref))
    return nullptr;
  if (ref.args.size() != 1) {
    diags_.error(ref.loc, "'Iterable' takes one type argument, the type of its values, e.g. 'Iterable<int>'");
    return Type::error();
  }
  const Type *element = resolveType(ref.args[0], /*allowVoid=*/false);
  if (element->isError())
    return element;
  if (fn->isConstructor) {
    diags_.error(ref.loc, "a constructor cannot take an 'Iterable<" + element->name() +
                              ">' yet: it would make a copy of the constructor for each kind of "
                              "iterable; take an array, e.g. '" + element->name() + "[]', and pass "
                              "'[...xs]'");
    return Type::error();
  }
  if (fn->var && !fn->typeParams.empty()) // reported generic already
    return Type::error();
  if (fn->isClosure() || fn->var) {
    diags_.error(ref.loc, "a parameter 'Iterable<" + element->name() +
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
  decl.name = ctx_.hiddenName();
  decl.loc = ref.loc;
  fn->typeParams.push_back(decl);
  fn->typeArgs.push_back(param);
  sym->typeParams.push_back(param);
  sym->typeDefaults.push_back(nullptr);
  return param;
}

} // namespace tz
