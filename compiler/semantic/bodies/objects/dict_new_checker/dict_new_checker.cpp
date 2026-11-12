#include "compiler/semantic/bodies/objects/dict_new_checker/dict_new_checker.h"
#include "compiler/program/library/array/array_method_table/array_method_table.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/iterators/generator_checker/generator_checker.h"
#include "compiler/semantic/bodies/iterators/iterator_methods/iterator_methods.h"
#include "compiler/semantic/bodies/objects/dict_checker/dict_checker.h"
#include "compiler/semantic/flow/flow_checker/flow_checker.h"
#include "compiler/semantic/library/decimal/decimal_checker/decimal_checker.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

DictNewChecker::DictNewChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->dictChecker = &all.dictChecker;
  this->exprChecker = &all.exprChecker;
  this->flowChecker = &all.flowChecker;
  this->typeRelations = &all.typeRelations;
  this->decimalChecker = &all.decimalChecker;
  this->iteratorMethods = &all.iteratorMethods;
  this->generatorChecker = &all.generatorChecker;
}

void DictNewChecker::markPairs(Expr *arg) {
  auto *call = dyn_cast<CallExpr>(arg);
  auto *member = call != nullptr ? dyn_cast<MemberExpr>(call->callee) : nullptr;
  auto *functionExpr =
      call != nullptr && call->args.size() == 1 ? dyn_cast<FunctionExpr>(call->args[0]) : nullptr;
  const ArrayMethodInfo *method =
      member != nullptr ? ArrayMethodTable::find(member->name) : nullptr;
  bool isMapping = method != nullptr && method->method == ArrayMethod::MAP;
  bool isInferredCallback = functionExpr != nullptr && !functionExpr->fn->returnType.has_value() &&
                            functionExpr->fn->body != nullptr;
  if (!isMapping || !isInferredCallback) {
    return;
  }
  auto returns = this->branchReturns(functionExpr->fn->body);
  bool isPlain = llvm::all_of(returns, std::bind_front(&DictNewChecker::isPairReturn, this));
  if (!isPlain || returns.empty()) {
    return;
  }
  for (const ReturnStmt *returnStmt : returns) {
    this->state.getExpressionMarks().markPairLiteral(returnStmt->value);
  }
}

std::vector<const ReturnStmt *> DictNewChecker::branchReturns(const Stmt *body) const {
  auto returns = std::vector<const ReturnStmt *>();
  auto stack = std::vector<const Stmt *>{body};
  while (!stack.empty()) {
    const Stmt *next = stack.back();
    stack.pop_back();
    auto *returnStmt = dyn_cast_or_null<ReturnStmt>(next);
    auto *block = dyn_cast_or_null<BlockStmt>(next);
    auto *branch = dyn_cast_or_null<IfStmt>(next);
    if (returnStmt != nullptr) {
      returns.push_back(returnStmt);
    }
    if (block != nullptr) {
      this->pushBlockStatements(block, stack);
    }
    if (branch != nullptr) {
      stack.push_back(branch->elseStmt);
      stack.push_back(branch->thenStmt);
    }
  }

  return returns;
}

void DictNewChecker::pushBlockStatements(const BlockStmt *block,
                                         std::vector<const Stmt *> &stack) const {
  for (const Stmt *inner : llvm::reverse(block->body)) {
    stack.push_back(inner);
  }
}

bool DictNewChecker::isPairReturn(const ReturnStmt *returnStmt) const {
  auto *pair = dyn_cast_or_null<ArrayLitExpr>(returnStmt->value);

  return pair != nullptr && pair->elements.size() == 2 && !pair->elements[0].spread &&
         !pair->elements[1].spread;
}

DictNewChecker::ResultPartsJoined DictNewChecker::checkPartsJoin(PairPart part, const Type *common,
                                                                 const Type *type) const {
  const Type *next = common != nullptr ? this->typeRelations->unify(common, type) : type;
  if (next == nullptr && part == PairPart::KEY) {
    auto message = ErrorTextPairParts::keysDiffer(common->name(), type->name());

    return err(DictNewCheckerPartsJoinError{
        .code = DictNewCheckerPartsJoinErrorCode::KEYS_DIFFER,
        .message = message,
    });
  }
  if (next == nullptr) {
    auto message = ErrorTextPairParts::valuesDiffer(common->name(), type->name());

    return err(DictNewCheckerPartsJoinError{
        .code = DictNewCheckerPartsJoinErrorCode::VALUES_DIFFER,
        .message = message,
    });
  }

  return ok(next);
}

DictNewChecker::ResultPairTypesOk DictNewChecker::checkPairsKnown(const Type *key,
                                                                  const Type *value) const {
  if (key->isUndefined() || value->isUndefined() || key->isVoid() || value->isVoid()) {
    auto message = ErrorTextPairsKnown::undefined();

    return err(DictNewCheckerPairsKnownError{
        .code = DictNewCheckerPairsKnownErrorCode::TYPES_UNDEFINED,
        .message = message,
    });
  }

  return ok(key);
}

DictNewChecker::ResultSourceShape
DictNewChecker::checkSourceShape(const Type *type, const Type *given, const Type *wanted,
                                 SourceItems items, llvm::StringRef what) const {
  if (given == nullptr) {
    auto message = ErrorTextDictSource::notIter(what, type->name(), type->isRecord());

    return err(DictNewCheckerSourceShapeError{
        .code = DictNewCheckerSourceShapeErrorCode::NOT_ITERABLE,
        .message = message,
    });
  }
  if (items == SourceItems::PAIRS && !this->isFittingPair(given, wanted)) {
    llvm::StringRef wantedName = wanted != nullptr ? wanted->name() : llvm::StringRef();
    auto message = ErrorTextDictSource::notPairs(what, wantedName, given->name(), given->isArray());

    return err(DictNewCheckerSourceShapeError{
        .code = DictNewCheckerSourceShapeErrorCode::NOT_PAIRS,
        .message = message,
    });
  }
  bool isWrongValue = items == SourceItems::VALUES && wanted != nullptr &&
                      !TypeRelations::isImplicitlyConvertible(given, wanted);
  if (isWrongValue) {
    auto message = ErrorTextDictSource::values(what, wanted->name(), given->name());

    return err(DictNewCheckerSourceShapeError{
        .code = DictNewCheckerSourceShapeErrorCode::WRONG_VALUES,
        .message = message,
    });
  }

  return ok(given);
}

bool DictNewChecker::isFittingPair(const Type *given, const Type *wanted) const {
  bool isPair = given->isTuple() && given->fields().size() == 2;
  if (!isPair || wanted == nullptr) {
    return isPair;
  }
  bool isKeyFitting =
      TypeRelations::isImplicitlyConvertible(given->fields()[0].type, wanted->fields()[0].type);
  bool isValueFitting =
      TypeRelations::isImplicitlyConvertible(given->fields()[1].type, wanted->fields()[1].type);

  return isKeyFitting && isValueFitting;
}

DictNewChecker::ResultOneArgument DictNewChecker::checkArgumentCount(const NewExpr *expr,
                                                                     bool isMap) const {
  if (expr->args.size() > 1) {
    auto message = ErrorTextDictArgs::oneArg(expr->name, isMap);

    return err(DictNewCheckerArgumentCountError{
        .code = DictNewCheckerArgumentCountErrorCode::TOO_MANY_ARGUMENTS,
        .message = message,
    });
  }

  return ok(expr);
}

DictNewChecker::ResultTypesStated DictNewChecker::checkTypesGiven(const NewExpr *expr,
                                                                  const Type *type, bool isMap,
                                                                  bool isUndefinedArg) const {
  if (type == nullptr && isUndefinedArg) {
    auto message = ErrorTextDictTypes::undefinedArg(expr->name, isMap);

    return err(DictNewCheckerTypesGivenError{
        .code = DictNewCheckerTypesGivenErrorCode::UNDEFINED_NEEDS_TYPES,
        .message = message,
    });
  }
  if (type == nullptr && isMap) {
    auto message = ErrorTextDictTypes::needsMap();

    return err(DictNewCheckerTypesGivenError{
        .code = DictNewCheckerTypesGivenErrorCode::MAP_NEEDS_TYPES,
        .message = message,
    });
  }
  if (type == nullptr) {
    auto message = ErrorTextDictTypes::needsSet();

    return err(DictNewCheckerTypesGivenError{
        .code = DictNewCheckerTypesGivenErrorCode::SET_NEEDS_TYPES,
        .message = message,
    });
  }

  return ok(type);
}

DictNewChecker::ResultPairsShaped DictNewChecker::checkPairShape(const Type *element) const {
  if (!element->isTuple() || element->fields().size() != 2) {
    auto message = ErrorTextDictPairs::pairs(element->name());

    return err(DictNewCheckerPairShapeError{
        .code = DictNewCheckerPairShapeErrorCode::NOT_PAIRS,
        .message = message,
    });
  }

  return ok(element);
}

DictNewChecker::ResultDefinedType DictNewChecker::checkElementsKnown(const NewExpr *expr,
                                                                     const Type *key,
                                                                     const Type *element,
                                                                     bool isMap) const {
  if (key->isUndefined() || (isMap && element->fields()[1].type->isUndefined())) {
    auto message = ErrorTextDictElements::undefined(expr->name);

    return err(DictNewCheckerElementsKnownError{
        .code = DictNewCheckerElementsKnownErrorCode::TYPES_UNDEFINED,
        .message = message,
    });
  }

  return ok(element);
}

std::optional<DictNewChecker::InferredPairs> DictNewChecker::inferPairs(Expr *arg) {
  this->markPairs(arg);
  auto *literal = dyn_cast<ArrayLitExpr>(arg);
  if (literal == nullptr || literal->elements.empty()) {
    return std::nullopt;
  }
  bool isPairList =
      llvm::all_of(literal->elements, std::bind_front(&DictNewChecker::isPairElement, this));
  if (!isPairList) {
    return std::nullopt;
  }
  auto join = PairJoin{
      .key = nullptr,
      .value = nullptr,
      .isTyped = true,
  };
  for (const ArrayLitExpr::Element &element : literal->elements) {
    auto *pair = cast<ArrayLitExpr>(element.expr);
    join = this->joinPart(join, pair->elements[0].expr, PairPart::KEY);
    join = this->joinPart(join, pair->elements[1].expr, PairPart::VALUE);
  }
  auto failed = InferredPairs{
      .key = Type::error(),
      .value = Type::error(),
  };
  if (!join.isTyped || join.key == nullptr || join.value == nullptr) {
    return failed;
  }
  auto known = this->checkPairsKnown(join.key, join.value);
  if (known.isErr()) {
    this->state.getDiagnostics().error(arg->loc(), known.error().message);
    return failed;
  }
  this->typePairs(literal, join.key, join.value);

  return InferredPairs{
      .key = join.key,
      .value = join.value,
  };
}

bool DictNewChecker::isPairElement(const ArrayLitExpr::Element &element) const {
  auto *pair = dyn_cast<ArrayLitExpr>(element.expr);

  return !element.spread && pair != nullptr && pair->elements.size() == 2 &&
         !pair->elements[0].spread && !pair->elements[1].spread;
}

DictNewChecker::PairJoin DictNewChecker::joinPart(const PairJoin &join, Expr *&part,
                                                  PairPart which) {
  const Type *common = which == PairPart::KEY ? join.key : join.value;
  const Type *hint =
      common != nullptr && this->decimalChecker->isLiteralTree(part) ? common : nullptr;
  part = this->exprChecker->checkExpr(part, hint);
  const Type *type = part->type;
  auto failed = PairJoin{
      .key = join.key,
      .value = join.value,
      .isTyped = false,
  };
  if (type->isError()) {
    return failed;
  }
  auto joined = this->checkPartsJoin(which, common, type);
  if (joined.isErr()) {
    this->state.getDiagnostics().error(part->loc(), joined.error().message);
    return failed;
  }
  if (which == PairPart::KEY) {
    return PairJoin{
        .key = joined.value(),
        .value = join.value,
        .isTyped = join.isTyped,
    };
  }

  return PairJoin{
      .key = join.key,
      .value = joined.value(),
      .isTyped = join.isTyped,
  };
}

void DictNewChecker::typePairs(ArrayLitExpr *literal, const Type *key, const Type *value) {
  const auto *pairType = Type::tuple({key, value});
  for (const ArrayLitExpr::Element &element : literal->elements) {
    auto *pair = cast<ArrayLitExpr>(element.expr);
    pair->elements[0].expr = this->typeRelations->coerce(pair->elements[0].expr, key);
    pair->elements[1].expr = this->typeRelations->coerce(pair->elements[1].expr, value);
    pair->type = pairType;
  }
  literal->type = Type::array(pairType);
}

DictNewChecker::SourceElement DictNewChecker::checkDictSource(Expr *arg, SourceItems items,
                                                              const Type *wanted,
                                                              llvm::StringRef what) {
  this->iteratorMethods->markFused(arg);
  const Type *hint = wanted != nullptr ? Type::array(wanted) : nullptr;
  auto *checked = this->exprChecker->checkExpr(arg, hint);
  auto *iterable = this->dictChecker->iterableOf(checked);
  const Type *type = iterable->type;
  auto failed = SourceElement{
      .arg = iterable,
      .element = Type::error(),
  };
  if (type->isError()) {
    return failed;
  }
  const auto *given = this->generatorChecker->iteratedType(type);
  if (given != nullptr && type->isIterator()) {
    this->walkSourceSends(iterable, type, what);
  }
  auto shaped = this->checkSourceShape(type, given, wanted, items, what);
  if (shaped.isErr()) {
    this->state.getDiagnostics().error(iterable->loc(), shaped.error().message);
    return failed;
  }

  return SourceElement{
      .arg = iterable,
      .element = given,
  };
}

void DictNewChecker::walkSourceSends(const Expr *arg, const Type *type, llvm::StringRef what) {
  auto nothing = this->generatorChecker->checkSendsNothing(type, what);
  if (nothing.isErr()) {
    this->state.getDiagnostics().error(arg->loc(), nothing.error().message);
  }
  this->flowChecker->calledFunction();
}

Expr *DictNewChecker::checkDictNew(NewExpr *expr, const Type *expected) {
  auto global = GlobalNameTable::find(expr->name);
  if (global == GlobalName::WEAK_MAP || global == GlobalName::WEAK_SET) {
    return this->reportWeakDict(expr);
  }
  bool isMap = global == GlobalName::MAP;
  auto stated = this->statedDictType(expr, expected, isMap);
  if (!stated.has_value()) {
    return this->failDictNew(expr);
  }
  auto single = this->checkArgumentCount(expr, isMap);
  if (single.isErr()) {
    this->state.getDiagnostics().error(expr->args[1]->loc(), single.error().message);
    return this->failDictNew(expr);
  }
  bool isEmpty = expr->args.empty() || isa<UndefinedExpr>(expr->args[0]);
  if (isEmpty) {
    return this->checkEmptyDictNew(expr, *stated, isMap);
  }
  auto source = this->sourcedDict(expr, *stated, isMap);
  if (!source.has_value()) {
    expr->type = Type::error();
    return expr;
  }

  return this->finishDictNew(expr, source->type, {source->arg});
}

Expr *DictNewChecker::checkEmptyDictNew(NewExpr *expr, const Type *type, bool isMap) {
  auto given = this->checkTypesGiven(expr, type, isMap, !expr->args.empty());
  if (given.isErr()) {
    this->state.getDiagnostics().error(expr->loc(), given.error().message);
    return this->failDictNew(expr);
  }

  return this->finishDictNew(expr, type, {});
}

Expr *DictNewChecker::finishDictNew(NewExpr *expr, const Type *type, std::vector<Expr *> args) {
  llvm::SMLoc loc = expr->loc();
  auto keyAllowed = this->dictChecker->checkKeyType(type->keyType());
  if (keyAllowed.isErr()) {
    this->state.getDiagnostics().error(loc, keyAllowed.error().message);
    return this->failDictNew(expr);
  }
  auto *callee = this->state.getAst().make<NameExpr>(loc, expr->name);
  auto *call = this->state.getAst().make<CallExpr>(loc, callee, std::move(args));
  call->callee->type = type;
  call->target = BuiltinCall{.builtin = Builtin::DictNew};
  call->type = type;

  return call;
}

Expr *DictNewChecker::failDictNew(NewExpr *expr) {
  for (Expr *&arg : expr->args) {
    arg = this->exprChecker->checkExpr(arg);
  }
  expr->type = Type::error();

  return expr;
}

Expr *DictNewChecker::reportWeakDict(NewExpr *expr) {
  auto ref = TypeRef{
      .name = expr->name,
      .loc = expr->loc(),
  };
  auto weak = this->dictChecker->resolveDictType(ref);
  if (weak.isErr()) {
    this->state.getDiagnostics().error(expr->loc(), weak.error().message);
  }

  return this->failDictNew(expr);
}

std::optional<const Type *> DictNewChecker::statedDictType(NewExpr *expr, const Type *expected,
                                                           bool isMap) {
  if (expr->typeArgs.empty()) {
    return this->wantedDict(expected, isMap);
  }
  auto ref = TypeRef{
      .form = GenericTypeRef{},
      .name = expr->name,
      .loc = expr->loc(),
      .args = expr->typeArgs,
  };
  auto given = this->dictChecker->resolveDictType(ref);
  if (given.isErr()) {
    bool isOnKey = this->dictChecker->isKeyArgument(given.error().code);
    llvm::SMLoc at = isOnKey ? ref.args[0].loc : ref.loc;
    this->state.getDiagnostics().error(at, given.error().message);
    return std::nullopt;
  }
  if (given.value()->isError()) {
    return std::nullopt;
  }

  return given.value();
}

const Type *DictNewChecker::wantedDict(const Type *expected, bool isMap) const {
  bool isUnion = expected != nullptr && expected->nonOptional()->isUnion();
  const Type *target = isUnion ? this->onlyDictMember(expected->nonOptional(), isMap) : expected;
  const Type *wanted = target != nullptr ? target->nonOptional() : nullptr;
  bool isFitting =
      wanted != nullptr && wanted->isDict() && wanted->isMap() == isMap && !wanted->isRecord();

  return isFitting ? wanted : nullptr;
}

const Type *DictNewChecker::onlyDictMember(const Type *expected, bool isMap) const {
  const Type *only = nullptr;
  int count = 0;
  for (const Type *member : expected->members()) {
    if (member->isDict() && !member->isRecord() && member->isMap() == isMap) {
      only = member;
      ++count;
    }
  }

  return count == 1 ? only : nullptr;
}

std::optional<DictNewChecker::DictSource>
DictNewChecker::sourcedDict(NewExpr *expr, const Type *type, bool isMap) {
  Expr *arg = expr->args[0];
  auto inferred = type == nullptr && isMap ? this->inferPairs(arg) : std::nullopt;
  if (inferred.has_value()) {
    return this->inferredPairsDict(arg, *inferred);
  }

  return this->checkedSourceDict(expr, arg, type, isMap);
}

std::optional<DictNewChecker::DictSource>
DictNewChecker::inferredPairsDict(Expr *arg, const InferredPairs &pairs) {
  if (pairs.key->isError()) {
    return std::nullopt;
  }
  auto allowed = this->dictChecker->checkKeyType(pairs.key);
  if (allowed.isErr()) {
    this->state.getDiagnostics().error(arg->loc(), allowed.error().message);
    return std::nullopt;
  }

  return DictSource{
      .type = Type::dict(Type::DictKind::Map, pairs.key, pairs.value),
      .arg = arg,
  };
}

std::optional<DictNewChecker::DictSource>
DictNewChecker::checkedSourceDict(NewExpr *expr, Expr *arg, const Type *type, bool isMap) {
  const auto *wanted = this->wantedElement(type, isMap);
  auto what = ErrorTextDictSource::newName(expr->name);
  auto items = isMap ? SourceItems::PAIRS : SourceItems::VALUES;
  auto source = this->checkDictSource(arg, items, wanted, what);
  if (source.element->isError()) {
    return std::nullopt;
  }
  if (type != nullptr) {
    return DictSource{
        .type = type,
        .arg = source.arg,
    };
  }
  auto made = this->sourceElementDict(expr, source.arg, source.element, isMap);
  if (!made.has_value()) {
    return std::nullopt;
  }

  return DictSource{
      .type = *made,
      .arg = source.arg,
  };
}

const Type *DictNewChecker::wantedElement(const Type *type, bool isMap) const {
  if (type == nullptr) {
    return nullptr;
  }
  if (isMap) {
    return Type::tuple({type->keyType(), type->valueType()});
  }

  return type->keyType();
}

std::optional<const Type *> DictNewChecker::sourceElementDict(const NewExpr *expr, const Expr *arg,
                                                              const Type *element, bool isMap) {
  auto shaped = this->checkPairShape(element);
  if (isMap && shaped.isErr()) {
    this->state.getDiagnostics().error(arg->loc(), shaped.error().message);
    return std::nullopt;
  }
  const Type *key = isMap ? element->fields()[0].type : element;
  auto allowed = this->dictChecker->checkKeyType(key);
  if (allowed.isErr()) {
    this->state.getDiagnostics().error(arg->loc(), allowed.error().message);
    return std::nullopt;
  }
  auto defined = this->checkElementsKnown(expr, key, element, isMap);
  if (defined.isErr()) {
    this->state.getDiagnostics().error(arg->loc(), defined.error().message);
    return std::nullopt;
  }
  if (isMap) {
    return Type::dict(Type::DictKind::Map, key, element->fields()[1].type);
  }

  return Type::dict(Type::DictKind::Set, key, Type::voidTy());
}

}
