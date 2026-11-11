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
  this->dictChecker = &all.getDictChecker();
  this->exprChecker = &all.getExprChecker();
  this->flowChecker = &all.getFlowChecker();
  this->typeRelations = &all.getTypeRelations();
  this->iteratorMethods = &all.getIteratorMethods();
  this->generatorChecker = &all.getGeneratorChecker();
}

void DictNewChecker::markPairs(Expr *arg) {
  auto *call = dyn_cast<CallExpr>(arg);
  auto *member = call ? dyn_cast<MemberExpr>(call->callee) : nullptr;
  auto *fe = call && call->args.size() == 1 ? dyn_cast<FunctionExpr>(call->args[0]) : nullptr;
  const ArrayMethodInfo *method =
      member != nullptr ? ArrayMethodTable::find(member->name) : nullptr;
  bool maps = method != nullptr && method->method == ArrayMethod::MAP;
  if (!maps || !fe || fe->fn->returnType || !fe->fn->body) {
    return;
  }
  std::vector<const ReturnStmt *> returns = DictNewChecker::branchReturns(fe->fn->body);
  bool plain = llvm::all_of(returns, DictNewChecker::returnsPair);
  if (!plain || returns.empty()) {
    return;
  }
  for (const ReturnStmt *ret : returns) {
    this->state.getExpressionMarks().markPairLiteral(ret->value);
  }
}

std::vector<const ReturnStmt *> DictNewChecker::branchReturns(const Stmt *body) {
  std::vector<const ReturnStmt *> returns;
  std::vector<const Stmt *> stack = {body};
  while (!stack.empty()) {
    const Stmt *next = stack.back();
    stack.pop_back();
    if (auto *ret = dyn_cast_or_null<ReturnStmt>(next)) {
      returns.push_back(ret);
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

bool DictNewChecker::returnsPair(const ReturnStmt *ret) {
  auto *pair = dyn_cast_or_null<ArrayLitExpr>(ret->value);

  return pair != nullptr && pair->elements.size() == 2 && !pair->elements[0].spread &&
         !pair->elements[1].spread;
}

DictNewChecker::ResultPartsJoined DictNewChecker::partsJoin(bool keys, const Type *common,
                                                            const Type *type) {
  const Type *next = common ? TypeRelations::unify(common, type) : type;
  if (!next && keys) {
    std::string message = ErrorTextPairParts::differ(true, common->name(), type->name());

    return err(DictNewCheckerPartsJoinError{
        .code = DictNewCheckerPartsJoinErrorCode::KEYS_DIFFER,
        .message = message,
    });
  }
  if (!next) {
    std::string message = ErrorTextPairParts::differ(false, common->name(), type->name());

    return err(DictNewCheckerPartsJoinError{
        .code = DictNewCheckerPartsJoinErrorCode::VALUES_DIFFER,
        .message = message,
    });
  }

  return ok(next);
}

DictNewChecker::ResultPairTypesOk DictNewChecker::pairsKnown(const Type *key, const Type *value) {
  if (key->isUndefined() || value->isUndefined() || key->isVoid() || value->isVoid()) {
    std::string message = ErrorTextPairsKnown::undefined();

    return err(DictNewCheckerPairsKnownError{
        .code = DictNewCheckerPairsKnownErrorCode::TYPES_UNDEFINED,
        .message = message,
    });
  }

  return ok(key);
}

DictNewChecker::ResultSourceShape DictNewChecker::sourceShape(const Type *type, const Type *given,
                                                              const Type *wanted, bool pairs,
                                                              llvm::StringRef what) {
  if (!given) {
    std::string message = ErrorTextDictSource::notIter(what, type->name(), type->isRecord());

    return err(DictNewCheckerSourceShapeError{
        .code = DictNewCheckerSourceShapeErrorCode::NOT_ITERABLE,
        .message = message,
    });
  }
  bool pair = given->isTuple() && given->fields().size() == 2;
  bool pairFits =
      pair &&
      (!wanted ||
       (TypeRelations::isImplicitlyConvertible(given->fields()[0].type, wanted->fields()[0].type) &&
        TypeRelations::isImplicitlyConvertible(given->fields()[1].type, wanted->fields()[1].type)));
  if (pairs && !pairFits) {
    llvm::StringRef wantedName = wanted ? wanted->name() : llvm::StringRef();
    std::string message =
        ErrorTextDictSource::notPairs(what, wantedName, given->name(), given->isArray());

    return err(DictNewCheckerSourceShapeError{
        .code = DictNewCheckerSourceShapeErrorCode::NOT_PAIRS,
        .message = message,
    });
  }
  if (!pairs && wanted && !TypeRelations::isImplicitlyConvertible(given, wanted)) {
    std::string message = ErrorTextDictSource::values(what, wanted->name(), given->name());

    return err(DictNewCheckerSourceShapeError{
        .code = DictNewCheckerSourceShapeErrorCode::WRONG_VALUES,
        .message = message,
    });
  }

  return ok(given);
}

DictNewChecker::ResultOneArgument DictNewChecker::argumentCount(const NewExpr *expr, bool isMap) {
  if (expr->args.size() > 1) {
    std::string message = ErrorTextDictArgs::oneArg(expr->name, isMap);

    return err(DictNewCheckerArgumentCountError{
        .code = DictNewCheckerArgumentCountErrorCode::TOO_MANY_ARGUMENTS,
        .message = message,
    });
  }

  return ok(expr);
}

DictNewChecker::ResultTypesStated DictNewChecker::typesGiven(const NewExpr *expr, const Type *type,
                                                             bool isMap, bool undefinedArg) {
  if (!type && undefinedArg) {
    std::string message = ErrorTextDictTypes::undefinedArg(expr->name, isMap);

    return err(DictNewCheckerTypesGivenError{
        .code = DictNewCheckerTypesGivenErrorCode::UNDEFINED_NEEDS_TYPES,
        .message = message,
    });
  }
  if (!type && isMap) {
    std::string message = ErrorTextDictTypes::needsMap();

    return err(DictNewCheckerTypesGivenError{
        .code = DictNewCheckerTypesGivenErrorCode::MAP_NEEDS_TYPES,
        .message = message,
    });
  }
  if (!type) {
    std::string message = ErrorTextDictTypes::needsSet();

    return err(DictNewCheckerTypesGivenError{
        .code = DictNewCheckerTypesGivenErrorCode::SET_NEEDS_TYPES,
        .message = message,
    });
  }

  return ok(type);
}

DictNewChecker::ResultPairsShaped DictNewChecker::pairShape(const Type *element) {
  if (!element->isTuple() || element->fields().size() != 2) {
    std::string message = ErrorTextDictPairs::pairs(element->name());

    return err(DictNewCheckerPairShapeError{
        .code = DictNewCheckerPairShapeErrorCode::NOT_PAIRS,
        .message = message,
    });
  }

  return ok(element);
}

DictNewChecker::ResultDefinedType DictNewChecker::elementsKnown(const NewExpr *expr,
                                                                const Type *key,
                                                                const Type *element, bool isMap) {
  if (key->isUndefined() || (isMap && element->fields()[1].type->isUndefined())) {
    std::string message = ErrorTextDictElements::undefined(expr->name);

    return err(DictNewCheckerElementsKnownError{
        .code = DictNewCheckerElementsKnownErrorCode::TYPES_UNDEFINED,
        .message = message,
    });
  }

  return ok(element);
}

bool DictNewChecker::inferPairs(Expr *arg, const Type *&key, const Type *&value) {
  this->markPairs(arg);
  auto *lit = dyn_cast<ArrayLitExpr>(arg);
  if (!lit || lit->elements.empty()) {
    return false;
  }
  for (const ArrayLitExpr::Element &e : lit->elements) {
    auto *pair = dyn_cast<ArrayLitExpr>(e.expr);
    if (e.spread || !pair || pair->elements.size() != 2 || pair->elements[0].spread ||
        pair->elements[1].spread) {
      return false;
    }
  }
  key = value = nullptr;
  bool typed = true;
  for (const ArrayLitExpr::Element &e : lit->elements) {
    auto *pair = cast<ArrayLitExpr>(e.expr);
    for (size_t i = 0; i < 2; ++i) {
      Expr *&part = pair->elements[i].expr;
      const Type *&common = i == 0 ? key : value;
      part = this->exprChecker->checkExpr(
          part, common && DecimalChecker::isLiteralTree(part) ? common : nullptr);
      const Type *t = part->type;
      if (t->isError()) {
        typed = false;
        continue;
      }
      ResultPartsJoined joined = DictNewChecker::partsJoin(i == 0, common, t);
      if (joined.isErr()) {
        this->state.getDiagnostics().error(part->loc(), joined.error().message);
        typed = false;
        continue;
      }
      common = joined.value();
    }
  }
  if (!typed || !key || !value) {
    key = value = Type::error();
    return true;
  }
  ResultPairTypesOk known = DictNewChecker::pairsKnown(key, value);
  if (known.isErr()) {
    this->state.getDiagnostics().error(arg->loc(), known.error().message);
    key = value = Type::error();
    return true;
  }
  const Type *pairType = Type::tuple({key, value});
  for (const ArrayLitExpr::Element &e : lit->elements) {
    auto *pair = cast<ArrayLitExpr>(e.expr);
    pair->elements[0].expr = this->typeRelations->coerce(pair->elements[0].expr, key);
    pair->elements[1].expr = this->typeRelations->coerce(pair->elements[1].expr, value);
    pair->type = pairType;
  }
  lit->type = Type::array(pairType);

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
  const Type *given = GeneratorChecker::iteratedType(type);
  if (given && type->isIterator()) {
    GeneratorChecker::ResultNextNeedsNoValue nothing =
        this->generatorChecker->checkSendsNothing(type, what);
    if (nothing.isErr()) {
      this->state.getDiagnostics().error(arg->loc(), nothing.error().message);
    }
    this->flowChecker->calledFunction();
  }
  ResultSourceShape shaped = DictNewChecker::sourceShape(type, given, wanted, pairs, what);
  if (shaped.isErr()) {
    this->state.getDiagnostics().error(arg->loc(), shaped.error().message);
    return arg;
  }
  element = given;

  return arg;
}

Expr *DictNewChecker::checkDictNew(NewExpr *expr, const Type *expected) {
  std::optional<GlobalName> global = GlobalNameTable::find(expr->name);
  llvm::SMLoc loc = expr->loc();
  if (global == GlobalName::WEAK_MAP || global == GlobalName::WEAK_SET) {
    return this->reportWeakDict(expr);
  }
  bool isMap = global == GlobalName::MAP;
  std::optional<const Type *> stated = this->statedDictType(expr, expected, isMap);
  if (!stated.has_value()) {
    return this->failDictNew(expr);
  }
  const Type *type = *stated;
  ResultOneArgument single = DictNewChecker::argumentCount(expr, isMap);
  if (single.isErr()) {
    this->state.getDiagnostics().error(expr->args[1]->loc(), single.error().message);
    return this->failDictNew(expr);
  }
  std::vector<Expr *> args;
  if (expr->args.empty() || isa<UndefinedExpr>(expr->args[0])) {
    ResultTypesStated given = DictNewChecker::typesGiven(expr, type, isMap, !expr->args.empty());
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
  DictChecker::ResultKeyTypeIsAllowed keyAllowed = this->dictChecker->checkKeyType(type->keyType());
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
  DictChecker::ResultDictTypeResolved weak = this->dictChecker->resolveDictType(ref);
  if (weak.isErr()) {
    this->state.getDiagnostics().error(expr->loc(), weak.error().message);
  }

  return this->failDictNew(expr);
}

std::optional<const Type *> DictNewChecker::statedDictType(NewExpr *expr, const Type *expected,
                                                           bool isMap) {
  if (expr->typeArgs.empty()) {
    return DictNewChecker::wantedDict(expected, isMap);
  }
  TypeRef ref;
  ref.form = GenericTypeRef{};
  ref.name = expr->name;
  ref.loc = expr->loc();
  ref.args = expr->typeArgs;
  DictChecker::ResultDictTypeResolved given = this->dictChecker->resolveDictType(ref);
  if (given.isErr()) {
    bool onKey = DictChecker::keyArgument(given.error().code);
    llvm::SMLoc at = onKey ? ref.args[0].loc : ref.loc;
    this->state.getDiagnostics().error(at, given.error().message);
    return std::nullopt;
  }
  if (given.value()->isError()) {
    return std::nullopt;
  }

  return given.value();
}

const Type *DictNewChecker::wantedDict(const Type *expected, bool isMap) {
  const Type *target = expected;
  if (target != nullptr && target->nonOptional()->isUnion()) {
    target = DictNewChecker::onlyDictMember(target->nonOptional(), isMap);
  }
  const Type *wanted = target != nullptr ? target->nonOptional() : nullptr;
  bool fits =
      wanted != nullptr && wanted->isDict() && wanted->isMap() == isMap && !wanted->isRecord();

  return fits ? wanted : nullptr;
}

const Type *DictNewChecker::onlyDictMember(const Type *expected, bool isMap) {
  const Type *only = nullptr;
  int count = 0;
  for (const Type *m : expected->members()) {
    if (m->isDict() && !m->isRecord() && m->isMap() == isMap) {
      only = m;
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
  DictChecker::ResultKeyTypeIsAllowed allowed = this->dictChecker->checkKeyType(key);
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
  std::string what = ErrorTextDictSource::newName(expr->name);
  Expr *checked = this->checkDictSource(arg, isMap, element, what);
  if (element->isError()) {
    return std::nullopt;
  }
  if (type != nullptr) {
    return DictSource{
        .type = type,
        .arg = checked,
    };
  }
  std::optional<const Type *> made = this->sourceElementDict(expr, checked, element, isMap);
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
  ResultPairsShaped shaped = DictNewChecker::pairShape(element);
  if (isMap && shaped.isErr()) {
    this->state.getDiagnostics().error(arg->loc(), shaped.error().message);
    return std::nullopt;
  }
  const Type *k = isMap ? element->fields()[0].type : element;
  DictChecker::ResultKeyTypeIsAllowed allowed = this->dictChecker->checkKeyType(k);
  if (allowed.isErr()) {
    this->state.getDiagnostics().error(arg->loc(), allowed.error().message);
    return std::nullopt;
  }
  ResultDefinedType defined = DictNewChecker::elementsKnown(expr, k, element, isMap);
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
