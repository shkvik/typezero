#include "compiler/semantic/code/iterators/iterable_sources/iterable_sources.h"
#include "compiler/program/library/dict/dict_method_table/dict_method_table.h"
#include "compiler/program/library/iterator/iterator_method_table/iterator_method_table.h"

using llvm::cast;

namespace tz {

namespace {

constexpr llvm::StringLiteral kIteratorMethod = "[Symbol.iterator]";
constexpr llvm::StringLiteral kAsyncIteratorMethod = "[Symbol.asyncIterator]";

}

IterableSources::IterableSources(SemanticState &owner)
    : state(owner) {}

void IterableSources::destructuredIteratorSlot(const DestructuredIteratorSlot &task) {
  Expr *init = *task.slot;
  const Type *iterator = init->type;
  auto destructuring = ErrorTextSendsNothing::destructuring();
  auto nothing = this->state.code.iteration.checkSendsNothing(iterator, destructuring);
  if (nothing.isErr()) {
    this->state.diagnostics.error(init->loc(), nothing.error().message);
  }
  Expr *source = task.declarator->patternRest() ? init : this->takeCall(init, *task.declarator);
  auto *array = this->state.ast.make<ArrayLitExpr>(init->loc());
  array->elements.push_back(ArrayLitExpr::Element{.expr = source, .spread = true});
  array->type = Type::array(iterator->yielded());
  *task.slot = array;

  return this->state.walk.tasks.pushTask(CalledFunction{});
}

void IterableSources::iterableOfSlot(const IterableOfSlot &task) {
  this->state.walk.tasks.pushTask(AfterIterableUser{.slot = task.slot});

  return this->state.walk.tasks.pushTask(UserIterator{.expr = *task.slot});
}

void IterableSources::afterIterableUser(const AfterIterableUser &task) {
  Expr *iterator = this->state.walk.tasks.takeChecked();
  if (iterator != nullptr) {
    *task.slot = iterator;
    return;
  }
  Expr *expr = *task.slot;
  const Type *type = expr->type;
  if (type != nullptr && type->isParam() && type->iterated() != nullptr) {
    const auto *iterated = Type::iterator(type->iterated());
    return this->state.walk.tasks.pushTask(ConvertSlot{.slot = task.slot, .to = iterated});
  }
  if (type != nullptr && type->isListParam()) {
    const auto *element = Type::indexedAccess(type, Type::intTy());
    const auto *listed = Type::iterator(element);
    return this->state.walk.tasks.pushTask(ConvertSlot{.slot = task.slot, .to = listed});
  }
  if (type != nullptr && type->intlKind() == IntlKind::Segments) {
    return this->state.walk.tasks.pushTask(SegmentsArraySlot{.slot = task.slot});
  }
  if (type == nullptr || (!type->isMap() && !type->isSet())) {
    return;
  }
  *task.slot = this->collectionValues(expr);
}

Expr *IterableSources::collectionValues(Expr *expr) {
  const Type *type = expr->type;
  if (type->isMap()) {
    const auto *entry = Type::tuple({type->keyType(), type->valueType()});
    const auto *entries = Type::iterator(entry);
    auto entriesName = DictMethodTable::getName(DictMethod::ENTRIES);
    return this->dictCall(expr->loc(), expr, entriesName, Builtin::MapEntries, entries);
  }
  const auto *values = Type::iterator(type->keyType());
  auto valuesName = DictMethodTable::getName(DictMethod::VALUES);

  return this->dictCall(expr->loc(), expr, valuesName, Builtin::SetValues, values);
}

Expr *IterableSources::dictCall(llvm::SMLoc loc, Expr *receiver, llvm::StringRef name,
                                Builtin builtin, const Type *type) {
  auto *member = this->state.ast.make<MemberExpr>(loc, receiver, name);
  member->type = type;
  auto *call = this->state.ast.make<CallExpr>(loc, member, std::vector<Expr *>{});
  call->target = BuiltinCall{.builtin = builtin};
  call->type = type;

  return call;
}

void IterableSources::findIterator(const FindIterator &task) {
  this->state.walk.tasks.pushTask(AfterBuiltinIterator{.expr = task.expr});

  return this->state.walk.tasks.pushTask(IteratorOf{.expr = task.expr});
}

void IterableSources::afterBuiltinIterator(const AfterBuiltinIterator &task) {
  Expr *builtin = this->state.walk.tasks.takeChecked();
  if (builtin != nullptr) {
    return this->state.walk.tasks.pushChecked(builtin);
  }

  return this->state.walk.tasks.pushTask(UserIterator{.expr = task.expr});
}

void IterableSources::userIterator(const UserIterator &task) {
  auto *call = this->symbolMethodCall(task.expr, kIteratorMethod);
  if (call == nullptr) {
    return this->state.walk.tasks.pushChecked(nullptr);
  }
  this->state.walk.tasks.pushTask(AfterSymbolIterator{.expr = task.expr});

  return this->state.walk.tasks.pushTask(CheckMethodCall{
      .call = call,
      .member = cast<MemberExpr>(call->callee),
      .expected = nullptr,
  });
}

void IterableSources::afterSymbolIterator(const AfterSymbolIterator &task) {
  Expr *result = this->state.walk.tasks.takeChecked();
  if (result->type->isError() || result->type->isIterator()) {
    return this->state.walk.tasks.pushChecked(result);
  }
  this->state.walk.tasks.pushTask(AfterResultIterator{.expr = task.expr, .result = result});

  return this->state.walk.tasks.pushTask(IteratorOf{.expr = result});
}

void IterableSources::afterResultIterator(const AfterResultIterator &task) {
  Expr *iterator = this->state.walk.tasks.takeChecked();
  if (iterator != nullptr) {
    return this->state.walk.tasks.pushChecked(iterator);
  }
  auto given = this->checkIteratorGiven(task.expr->type, task.result);
  if (given.isErr()) {
    this->state.diagnostics.error(task.expr->loc(), given.error().message);
  }
  task.result->type = Type::error();

  return this->state.walk.tasks.pushChecked(task.result);
}

void IterableSources::asyncUserIterator(const AsyncUserIterator &task) {
  auto *call = this->symbolMethodCall(task.expr, kAsyncIteratorMethod);
  if (call == nullptr) {
    return this->state.walk.tasks.pushChecked(nullptr);
  }
  this->state.walk.tasks.pushTask(AfterAsyncSymbolIterator{.expr = task.expr});

  return this->state.walk.tasks.pushTask(CheckMethodCall{
      .call = call,
      .member = cast<MemberExpr>(call->callee),
      .expected = nullptr,
  });
}

void IterableSources::afterAsyncSymbolIterator(const AfterAsyncSymbolIterator &task) {
  Expr *result = this->state.walk.tasks.takeChecked();
  if (result->type->isError() || result->type->isAsyncIterator()) {
    return this->state.walk.tasks.pushChecked(result);
  }
  auto given = this->checkAsyncIteratorGiven(task.expr->type, result);
  if (given.isErr()) {
    this->state.diagnostics.error(task.expr->loc(), given.error().message);
  }
  result->type = Type::error();

  return this->state.walk.tasks.pushChecked(result);
}

CallExpr *IterableSources::symbolMethodCall(Expr *expr, llvm::StringRef name) {
  ClassDecl *classDecl = this->classOf(expr->type);
  if (classDecl == nullptr) {
    return nullptr;
  }
  auto methods = classDecl->findMethodsNamed(name);
  bool hasMethod = !methods.empty() || classDecl->findField(name) != nullptr;
  if (!hasMethod) {
    return nullptr;
  }
  auto *member = this->state.ast.make<MemberExpr>(expr->loc(), expr, name);

  return this->state.ast.make<CallExpr>(expr->loc(), member, std::vector<Expr *>{});
}

void IterableSources::iteratorOf(const IteratorOf &task) {
  const Type *type = task.expr->type;
  if (type->isIterator() || type->isError()) {
    return this->state.walk.tasks.pushChecked(task.expr);
  }
  this->state.walk.tasks.pushTask(AfterIteratorValue{.expr = task.expr});

  return this->state.walk.tasks.pushTask(NextValueOf{.type = type});
}

void IterableSources::afterIteratorValue(const AfterIteratorValue &task) {
  const Type *value = this->state.walk.tasks.takeAnswer();
  if (value == nullptr) {
    return this->state.walk.tasks.pushChecked(nullptr);
  }
  auto *known = this->state.code.variables.getIterateSymbol();
  if (known != nullptr) {
    return this->pushIterateCall(task.expr, value, known);
  }
  this->state.walk.tasks.pushTask(BuildIterateCall{.expr = task.expr, .value = value});

  return this->startIterateSymbol();
}

void IterableSources::startIterateSymbol() {
  auto items = this->state.library.declarations.getBuiltinItems(BuiltinText::ITERATE);
  auto *functionDecl = cast<FunctionDecl>(items.front());
  auto topTypes = this->state.walk.scopes.getTopTypes();
  this->state.walk.scopes.enterOnlyTypes(topTypes);
  this->state.walk.tasks.pushTask(FinishIterateSymbol{.functionDecl = functionDecl});

  return this->state.walk.tasks.pushTask(
      StartFunctionSymbol{.functionDecl = functionDecl, .variable = nullptr, .isNamed = false});
}

void IterableSources::finishIterateSymbol(const FinishIterateSymbol &task) {
  FuncSymbol *iterate = task.functionDecl->sym;
  this->state.walk.scopes.leaveOnlyTypes();
  this->state.code.variables.rememberIterateSymbol(iterate);
  this->state.generics.parameters.rememberExactParams(iterate, {true, true});
}

void IterableSources::buildIterateCall(const BuildIterateCall &task) {
  auto *iterate = this->state.code.variables.getIterateSymbol();

  return this->pushIterateCall(task.expr, task.value, iterate);
}

void IterableSources::pushIterateCall(Expr *it, const Type *value, FuncSymbol *iterate) {
  const Type *type = it->type;
  llvm::SMLoc loc = it->loc();
  auto *callee = this->state.ast.make<NameExpr>(loc, iterate->name);
  auto *call = this->state.ast.make<CallExpr>(loc, callee, std::vector<Expr *>{it});
  call->target = FunctionCall{.func = iterate};
  call->type = Type::iterator(value);
  callee->type = Type::function({type->nonOptional()}, 1, false, call->type);
  this->state.walk.tasks.pushChecked(call);
  this->state.walk.tasks.pushTask(CalledFunction{});
  if (type->isOptional()) {
    this->state.walk.tasks.pushTask(ConvertSlot{.slot = &call->args[0], .to = type->nonOptional()});
  }
  this->state.walk.tasks.pushTask(FinishIterateCall{.call = call});
  if (this->state.walk.functions.isInsideTrial()) {
    return;
  }

  return this->state.walk.tasks.pushTask(InstantiateCallTarget{
      .call = call,
      .generic = iterate,
      .typeArgs = {type->nonOptional(), value},
      .loc = loc,
  });
}

void IterableSources::finishIterateCall(const FinishIterateCall &task) {
  auto *callee = cast<NameExpr>(task.call->callee);
  callee->binding = NamedFunction{.func = task.call->func()};
}

Expr *IterableSources::takeCall(Expr *init, const VarDeclarator &declarator) {
  auto takeName = IteratorMethodTable::getName(IteratorMethod::TAKE);
  auto *member = this->state.ast.make<MemberExpr>(init->loc(), init, takeName);
  auto *count = this->state.ast.make<IntLitExpr>(init->loc(), declarator.patternLength());
  count->type = Type::intTy();
  auto *take = this->state.ast.make<CallExpr>(init->loc(), member, std::vector<Expr *>{count});
  take->target = BuiltinCall{.builtin = Builtin::IterTake};
  take->fused = true;
  take->type = Type::iterator(init->type->yielded());

  return take;
}

IterableSources::ResultIteratorGiven IterableSources::checkIteratorGiven(const Type *type,
                                                                         const Expr *result) const {
  if (!result->type->isError() && !result->type->isIterator()) {
    auto message = ErrorTextIteratorGiven::notIterator(type->name(), result->type->name());

    return err(CodeIteratorsRegistryIteratorGivenError{
        .code = CodeIteratorsRegistryIteratorGivenErrorCode::NOT_AN_ITERATOR,
        .message = message,
    });
  }

  return ok(result);
}

IterableSources::ResultAsyncIterator
IterableSources::checkAsyncIteratorGiven(const Type *type, const Expr *result) const {
  if (!result->type->isError() && !result->type->isAsyncIterator()) {
    auto message = ErrorTextAsyncGiven::notAsync(type->name(), result->type->name());

    return err(CodeIteratorsRegistryAsyncIteratorGivenError{
        .code = CodeIteratorsRegistryAsyncIteratorGivenErrorCode::NOT_AN_ASYNC_ITERATOR,
        .message = message,
    });
  }

  return ok(result);
}

ClassDecl *IterableSources::classOf(const Type *type) const {
  if (type == nullptr) {
    return nullptr;
  }
  const auto *classDecl = type->classDecl();

  return const_cast<ClassDecl *>(classDecl);
}

}
