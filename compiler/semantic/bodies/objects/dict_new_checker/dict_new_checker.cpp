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
  if (!isMapping || functionExpr == nullptr || functionExpr->fn->returnType ||
      !functionExpr->fn->body) {
    return;
  }
  auto returns = this->branchReturns(functionExpr->fn->body);
  bool isPlain = llvm::all_of(returns, DictNewChecker::returnsPair);
  if (!isPlain || returns.empty()) {
    return;
  }
  for (const ReturnStmt *returnStmt : returns) {
    this->state.getExpressionMarks().markPairLiteral(returnStmt->value);
  }
}

std::vector<const ReturnStmt *> DictNewChecker::branchReturns(const Stmt *body) const {
  auto returns = std::vector<const ReturnStmt *>();
  std::vector<const Stmt *> stack = {body};
  while (!stack.empty()) {
    const Stmt *next = stack.back();
    stack.pop_back();
    auto *returnStmt = dyn_cast_or_null<ReturnStmt>(next);
    if (returnStmt != nullptr) {
      returns.push_back(returnStmt);
    } else if (auto *block = dyn_cast_or_null<BlockStmt>(next)) {
      for (const Stmt *inner : llvm::reverse(block->body)) {
        stack.push_back(inner);
      }
    } else if (auto *branch = dyn_cast_or_null<IfStmt>(next)) {
      stack.push_back(branch->elseStmt);
      stack.push_back(branch->thenStmt);
    }
  }

  return returns;
}

bool DictNewChecker::returnsPair(const ReturnStmt *returnStmt) {
  auto *pair = dyn_cast_or_null<ArrayLitExpr>(returnStmt->value);

  return pair != nullptr && pair->elements.size() == 2 && !pair->elements[0].spread &&
         !pair->elements[1].spread;
}

DictNewChecker::ResultPartsJoined DictNewChecker::partsJoin(bool keys, const Type *common,
                                                            const Type *type) const {
  const Type *next = common != nullptr ? this->typeRelations->unify(common, type) : type;
  if (next == nullptr && keys) {
    auto message = ErrorTextPairParts::differ(true, common->name(), type->name());

    return err(DictNewCheckerPartsJoinError{
        .code = DictNewCheckerPartsJoinErrorCode::KEYS_DIFFER,
        .message = message,
    });
  }
  if (next == nullptr) {
    auto message = ErrorTextPairParts::differ(false, common->name(), type->name());

    return err(DictNewCheckerPartsJoinError{
        .code = DictNewCheckerPartsJoinErrorCode::VALUES_DIFFER,
        .message = message,
    });
  }

  return ok(next);
}

DictNewChecker::ResultPairTypesOk DictNewChecker::pairsKnown(const Type *key,
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

DictNewChecker::ResultSourceShape DictNewChecker::sourceShape(const Type *type, const Type *given,
                                                              const Type *wanted, bool pairs,
                                                              llvm::StringRef what) const {
  if (given == nullptr) {
    auto message = ErrorTextDictSource::notIter(what, type->name(), type->isRecord());

    return err(DictNewCheckerSourceShapeError{
        .code = DictNewCheckerSourceShapeErrorCode::NOT_ITERABLE,
        .message = message,
    });
  }
  bool isPair = given->isTuple() && given->fields().size() == 2;
  bool isPairFits =
      isPair &&
      (wanted == nullptr ||
       (TypeRelations::isImplicitlyConvertible(given->fields()[0].type, wanted->fields()[0].type) &&
        TypeRelations::isImplicitlyConvertible(given->fields()[1].type, wanted->fields()[1].type)));
  if (pairs && !isPairFits) {
    llvm::StringRef wantedName = wanted != nullptr ? wanted->name() : llvm::StringRef();
    auto message = ErrorTextDictSource::notPairs(what, wantedName, given->name(), given->isArray());

    return err(DictNewCheckerSourceShapeError{
        .code = DictNewCheckerSourceShapeErrorCode::NOT_PAIRS,
        .message = message,
    });
  }
  if (!pairs && wanted != nullptr && !TypeRelations::isImplicitlyConvertible(given, wanted)) {
    auto message = ErrorTextDictSource::values(what, wanted->name(), given->name());

    return err(DictNewCheckerSourceShapeError{
        .code = DictNewCheckerSourceShapeErrorCode::WRONG_VALUES,
        .message = message,
    });
  }

  return ok(given);
}

DictNewChecker::ResultOneArgument DictNewChecker::argumentCount(const NewExpr *expr,
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

DictNewChecker::ResultTypesStated DictNewChecker::typesGiven(const NewExpr *expr, const Type *type,
                                                             bool isMap, bool undefinedArg) const {
  if (type == nullptr && undefinedArg) {
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

DictNewChecker::ResultPairsShaped DictNewChecker::pairShape(const Type *element) const {
  if (!element->isTuple() || element->fields().size() != 2) {
    auto message = ErrorTextDictPairs::pairs(element->name());

    return err(DictNewCheckerPairShapeError{
        .code = DictNewCheckerPairShapeErrorCode::NOT_PAIRS,
        .message = message,
    });
  }

  return ok(element);
}

DictNewChecker::ResultDefinedType DictNewChecker::elementsKnown(const NewExpr *expr,
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

bool DictNewChecker::inferPairs(Expr *arg, const Type *&key, const Type *&value) {
  this->markPairs(arg);
  auto *literal = dyn_cast<ArrayLitExpr>(arg);
  if (literal == nullptr || literal->elements.empty()) {
    return false;
  }
  for (const ArrayLitExpr::Element &element : literal->elements) {
    auto *pair = dyn_cast<ArrayLitExpr>(element.expr);
    if (element.spread || pair == nullptr || pair->elements.size() != 2 ||
        pair->elements[0].spread || pair->elements[1].spread) {
      return false;
    }
  }
  key = value = nullptr;
  bool isTyped = true;
  for (const ArrayLitExpr::Element &element : literal->elements) {
    auto *pair = cast<ArrayLitExpr>(element.expr);
    for (size_t index = 0; index < 2; ++index) {
      Expr *&part = pair->elements[index].expr;
      const Type *&common = index == 0 ? key : value;
      part = this->exprChecker->checkExpr(
          part, common && this->decimalChecker->isLiteralTree(part) ? common : nullptr);
      const Type *type = part->type;
      if (type->isError()) {
        isTyped = false;
        continue;
      }
      auto joined = this->partsJoin(index == 0, common, type);
      if (joined.isErr()) {
        this->state.getDiagnostics().error(part->loc(), joined.error().message);
        isTyped = false;
        continue;
      }
      common = joined.value();
    }
  }
  if (!isTyped || !key || !value) {
    key = value = Type::error();
    return true;
  }
  auto known = this->pairsKnown(key, value);
  if (known.isErr()) {
    this->state.getDiagnostics().error(arg->loc(), known.error().message);
    key = value = Type::error();
    return true;
  }
  const auto *pairType = Type::tuple({key, value});
  for (const ArrayLitExpr::Element &element : literal->elements) {
    auto *pair = cast<ArrayLitExpr>(element.expr);
    pair->elements[0].expr = this->typeRelations->coerce(pair->elements[0].expr, key);
    pair->elements[1].expr = this->typeRelations->coerce(pair->elements[1].expr, value);
    pair->type = pairType;
  }
  literal->type = Type::array(pairType);

  return true;
}

Expr *DictNewChecker::checkDictSource(Expr *arg, bool pairs, const Type *&element,
                                      llvm::StringRef what) {
  this->iteratorMethods->markFused(arg);
  arg = this->dictChecker->iterableOf(
      this->exprChecker->checkExpr(arg, element ? Type::array(element) : nullptr));
  const Type *type = arg->type;
  const Type *wanted = element;
  element = Type::error();
  if (type->isError()) {
    return arg;
  }
  const auto *given = this->generatorChecker->iteratedType(type);
  if (given != nullptr && type->isIterator()) {
    auto nothing = this->generatorChecker->checkSendsNothing(type, what);
    if (nothing.isErr()) {
      this->state.getDiagnostics().error(arg->loc(), nothing.error().message);
    }
    this->flowChecker->calledFunction();
  }
  auto shaped = this->sourceShape(type, given, wanted, pairs, what);
  if (shaped.isErr()) {
    this->state.getDiagnostics().error(arg->loc(), shaped.error().message);
    return arg;
  }
  element = given;

  return arg;
}

Expr *DictNewChecker::checkDictNew(NewExpr *expr, const Type *expected) {
  auto global = GlobalNameTable::find(expr->name);
  llvm::SMLoc loc = expr->loc();
  if (global == GlobalName::WEAK_MAP || global == GlobalName::WEAK_SET) {
    return this->reportWeakDict(expr);
  }
  bool isMap = global == GlobalName::MAP;
  auto stated = this->statedDictType(expr, expected, isMap);
  if (!stated.has_value()) {
    return this->failDictNew(expr);
  }
  const Type *type = *stated;
  auto single = this->argumentCount(expr, isMap);
  if (single.isErr()) {
    this->state.getDiagnostics().error(expr->args[1]->loc(), single.error().message);
    return this->failDictNew(expr);
  }
  auto args = std::vector<Expr *>();
  if (expr->args.empty() || isa<UndefinedExpr>(expr->args[0])) {
    auto given = this->typesGiven(expr, type, isMap, !expr->args.empty());
    if (given.isErr()) {
      this->state.getDiagnostics().error(loc, given.error().message);
      return this->failDictNew(expr);
    }
  } else {
    std::optional<DictSource> source = this->sourcedDict(expr, type, isMap);
    if (!source.has_value()) {
      expr->type = Type::error();
      return expr;
    }
    type = source->type;
    args.push_back(source->arg);
  }
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
  TypeRef ref;
  ref.name = expr->name;
  ref.loc = expr->loc();
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
  TypeRef ref;
  ref.form = GenericTypeRef{};
  ref.name = expr->name;
  ref.loc = expr->loc();
  ref.args = expr->typeArgs;
  auto given = this->dictChecker->resolveDictType(ref);
  if (given.isErr()) {
    bool isOnKey = this->dictChecker->keyArgument(given.error().code);
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
  const Type *target = expected;
  if (target != nullptr && target->nonOptional()->isUnion()) {
    target = this->onlyDictMember(target->nonOptional(), isMap);
  }
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
  const Type *key = nullptr;
  const Type *value = nullptr;
  if (type == nullptr && isMap && this->inferPairs(arg, key, value)) {
    return this->inferredPairsDict(arg, key, value);
  }

  return this->checkedSourceDict(expr, arg, type, isMap);
}

std::optional<DictNewChecker::DictSource>
DictNewChecker::inferredPairsDict(Expr *arg, const Type *key, const Type *value) {
  if (key->isError()) {
    return std::nullopt;
  }
  auto allowed = this->dictChecker->checkKeyType(key);
  if (allowed.isErr()) {
    this->state.getDiagnostics().error(arg->loc(), allowed.error().message);
    return std::nullopt;
  }

  return DictSource{
      .type = Type::dict(Type::DictKind::Map, key, value),
      .arg = arg,
  };
}

std::optional<DictNewChecker::DictSource>
DictNewChecker::checkedSourceDict(NewExpr *expr, Expr *arg, const Type *type, bool isMap) {
  const Type *element = nullptr;
  if (type != nullptr) {
    element = isMap ? Type::tuple({type->keyType(), type->valueType()}) : type->keyType();
  }
  auto what = ErrorTextDictSource::newName(expr->name);
  auto *checked = this->checkDictSource(arg, isMap, element, what);
  if (element->isError()) {
    return std::nullopt;
  }
  if (type != nullptr) {
    return DictSource{
        .type = type,
        .arg = checked,
    };
  }
  auto made = this->sourceElementDict(expr, checked, element, isMap);
  if (!made.has_value()) {
    return std::nullopt;
  }

  return DictSource{
      .type = *made,
      .arg = checked,
  };
}

std::optional<const Type *> DictNewChecker::sourceElementDict(const NewExpr *expr, const Expr *arg,
                                                              const Type *element, bool isMap) {
  auto shaped = this->pairShape(element);
  if (isMap && shaped.isErr()) {
    this->state.getDiagnostics().error(arg->loc(), shaped.error().message);
    return std::nullopt;
  }
  const Type *k = isMap ? element->fields()[0].type : element;
  auto allowed = this->dictChecker->checkKeyType(k);
  if (allowed.isErr()) {
    this->state.getDiagnostics().error(arg->loc(), allowed.error().message);
    return std::nullopt;
  }
  auto defined = this->elementsKnown(expr, k, element, isMap);
  if (defined.isErr()) {
    this->state.getDiagnostics().error(arg->loc(), defined.error().message);
    return std::nullopt;
  }
  if (isMap) {
    return Type::dict(Type::DictKind::Map, k, element->fields()[1].type);
  }

  return Type::dict(Type::DictKind::Set, k, Type::voidTy());
}

}
