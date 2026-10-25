#include "compiler/check/types/destination/destination.h"
#include "compiler/check/iterators/iterable_checker/iterable_checker.h"
#include "compiler/check/scopes/scopes.h"
#include "compiler/check/types/destination_check_helpers/destination_check_helpers.h"
#include "compiler/check/types/destination_scanner/destination_scanner.h"
#include "compiler/check/types/literal_types/literal_types.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/check/types/type_resolver/type_resolver.h"
#include "compiler/whole_program/flows/flows.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

// The values the returns of a function body give, outside the functions
// nested in it.
static void forEachReturn(const Stmt *stmt, llvm::function_ref<void(const Expr *)> each) {
  if (!stmt) {
    return;
  }
  switch (stmt->kind()) {
  case Stmt::Kind::Return:
    if (const Expr *value = cast<ReturnStmt>(stmt)->value) {
      each(value);
    }
    break;
  case Stmt::Kind::Block:
    for (const Stmt *s : cast<BlockStmt>(stmt)->body) {
      forEachReturn(s, each);
    }
    break;
  case Stmt::Kind::If:
    forEachReturn(cast<IfStmt>(stmt)->thenStmt, each);
    forEachReturn(cast<IfStmt>(stmt)->elseStmt, each);
    break;
  case Stmt::Kind::While:
    forEachReturn(cast<WhileStmt>(stmt)->body, each);
    break;
  case Stmt::Kind::DoWhile:
    forEachReturn(cast<DoWhileStmt>(stmt)->body, each);
    break;
  case Stmt::Kind::For:
    forEachReturn(cast<ForStmt>(stmt)->body, each);
    break;
  case Stmt::Kind::ForOf:
    forEachReturn(cast<ForOfStmt>(stmt)->body, each);
    break;
  case Stmt::Kind::Switch:
    for (const SwitchCase &c : cast<SwitchStmt>(stmt)->cases) {
      for (const Stmt *s : c.body) {
        forEachReturn(s, each);
      }
    }
    break;
  case Stmt::Kind::Labeled:
    forEachReturn(cast<LabeledStmt>(stmt)->body, each);
    break;
  case Stmt::Kind::Try: {
    auto *s = cast<TryStmt>(stmt);
    forEachReturn(s->block, each);
    forEachReturn(s->handler, each);
    forEachReturn(s->finalizer, each);
    break;
  }
  default:
    break;
  }
}

// `ok(v)` or `err(e)` as written, before it is checked.
static bool isResultLiteral(const CallExpr *call) {
  auto *name = dyn_cast<NameExpr>(call->callee);

  return name && (name->name == "ok" || name->name == "err") && call->args.size() <= 1 &&
         !call->hasSpread() && !call->optional;
}

// A type that fits where the literal goes, without the read-only fields of
// the place: the object itself may still be changed.
static const Type *writable(const Type *type) {
  const Type *object = type->nonOptional();
  if (!object->isObject() ||
      !llvm::any_of(object->fields(), [](const Field &f) { return f.readonly; })) {
    return object;
  }
  if (object->isTuple()) {
    std::vector<const Type *> elements;
    for (const Field &f : object->fields()) {
      elements.push_back(f.type);
    }
    return Type::tuple(elements);
  }
  std::vector<Field> fields(object->fields().begin(), object->fields().end());
  for (Field &f : fields) {
    f.readonly = false;
  }

  return Type::object(std::move(fields));
}

Destination::Destination(Checker &checker) : checker_(checker) {}

bool Destination::literalShaped(const Expr *expr) {
  // An array literal too: `[1, 2]` that goes to a `double[]` holds doubles,
  // and `["Ann", 30]` that goes to `[string, int]` is a tuple.
  if (isa<ObjectLitExpr>(expr) || isa<ArrayLitExpr>(expr)) {
    return true;
  }
  if (auto *cond = dyn_cast<ConditionalExpr>(expr)) {
    return literalShaped(cond->thenExpr) || literalShaped(cond->elseExpr);
  }
  if (auto *call = dyn_cast<CallExpr>(expr)) {
    // `ok(1)`: a Result whose other half, and the type of its own, come from
    // where it goes.
    if (isResultLiteral(call)) {
      return true;
    }
    // `xs.map(x => ({ x, y: 0 }))`
    auto *member = dyn_cast<MemberExpr>(call->callee);
    if (!member || (member->name != "map" && member->name != "flatMap") || call->args.size() != 1) {
      return false;
    }
    auto *fe = dyn_cast<FunctionExpr>(call->args[0]);
    bool returnsLiteral = false;
    if (fe) {
      forEachReturn(fe->fn->body,
                    [&](const Expr *value) { returnsLiteral |= literalShaped(value); });
    }
    return returnsLiteral;
  }

  return false;
}

bool Destination::literalFits(const Expr *expr, const Type *type) const {
  type = type->nonOptional();
  // Into a union: as the member it is written for (see literalMember).
  if (type->isUnion()) {
    if (auto *lit = dyn_cast<ObjectLitExpr>(expr)) {
      if (const Type *member = checker_.literalTypes().literalMember(lit, type)) {
        return literalFits(expr, member);
      }
    }
    if (const Type *array = type->slotPart(UnionSlot::Array); array && isa<ArrayLitExpr>(expr)) {
      return literalFits(expr, array);
    }
    return false;
  }
  if (auto *lit = dyn_cast<ObjectLitExpr>(expr)) {
    if (!type->isObject() || type->isTuple() || type->isIncomplete()) {
      return false;
    }
    std::vector<llvm::StringRef> names;
    for (const ObjectLitExpr::Property &prop : lit->props) {
      if (prop.spread) {
        // The fields of the object spread, if its type is known.
        auto *name = dyn_cast<NameExpr>(prop.value);
        const VarSymbol *var = name ? checker_.scopes().lookup(name->name).var : nullptr;
        if (!var || !var->type || !var->type->isObject() || var->type->isTuple()) {
          return false;
        }
        for (const Field &f : var->type->fields()) {
          if (!llvm::is_contained(prop.omit, f.name)) {
            names.push_back(f.name);
          }
        }
        continue;
      }
      if (prop.key) { // known only once checked
        return false;
      }
      llvm::StringRef name = prop.name;
      const Field *field = type->field(name);
      if (!field || (literalShaped(prop.value) && !literalFits(prop.value, field->type))) {
        return false;
      }
      names.push_back(name);
    }
    for (llvm::StringRef name : names) {
      if (!type->field(name)) {
        return false;
      }
    }
    return llvm::all_of(type->fields(), [&](const Field &f) {
      return f.type->isOptional() || llvm::is_contained(names, f.name);
    });
  }
  if (auto *lit = dyn_cast<ArrayLitExpr>(expr)) {
    if (type->isTuple()) {
      if (lit->elements.size() != type->fields().size()) {
        return false;
      }
      for (size_t i = 0; i < lit->elements.size(); ++i) {
        const ArrayLitExpr::Element &e = lit->elements[i];
        if (e.spread || (literalShaped(e.expr) && !literalFits(e.expr, type->fields()[i].type))) {
          return false;
        }
      }
      return true;
    }
    if (!type->isArray()) {
      return false;
    }
    return llvm::all_of(lit->elements, [&](const ArrayLitExpr::Element &e) {
      return e.spread || !literalShaped(e.expr) || literalFits(e.expr, type->inner());
    });
  }
  if (auto *cond = dyn_cast<ConditionalExpr>(expr)) {
    return (!literalShaped(cond->thenExpr) || literalFits(cond->thenExpr, type)) &&
           (!literalShaped(cond->elseExpr) || literalFits(cond->elseExpr, type));
  }
  if (auto *call = dyn_cast<CallExpr>(expr)) {
    if (isResultLiteral(call)) {
      if (!type->isResult()) {
        return false;
      }
      bool isOk = cast<NameExpr>(call->callee)->name == "ok";
      const Type *half = isOk ? type->okType() : type->errType();
      return call->args.empty() || !literalShaped(call->args[0]) ||
             literalFits(call->args[0], half);
    }
    if (!type->isArray()) {
      return false;
    }
    auto *fe = cast<FunctionExpr>(call->args[0]);
    const Type *element = cast<MemberExpr>(call->callee)->name == "map" ? type->inner() : type;
    bool fits = true;
    forEachReturn(fe->fn->body, [&](const Expr *value) {
      fits &= !literalShaped(value) || literalFits(value, element);
    });
    return fits;
  }

  return true;
}

const Type *Destination::literalDestination(const VarDeclarator &d,
                                            llvm::ArrayRef<VarDeclarator> later) {
  Checker::DestinationScan scan;
  scan.name = d.name;
  scan.returns = checker_.currentFunction_ ? checker_.currentFunction_->ret : nullptr;
  scan.locals.emplace_back();
  bool muted = checker_.diags_.setMuted(true); // types are resolved again where they are written
  bool visible = true;
  for (const VarDeclarator &next : later) {
    if (next.init) {
      checker_.destinationScanner().scanDestinations(
          next.init,
          next.typeAnn ? checker_.typeResolver().resolveType(*next.typeAnn, false) : nullptr, scan);
    }
    if (next.name == d.name) {
      visible = false;
    }
    scan.declare(next.name, next.typeAnn ? checker_.typeResolver().resolveType(*next.typeAnn, false)
                                         : nullptr);
  }
  std::vector<const Stmt *> following = std::move(checker_.following_);
  checker_.following_.clear();
  for (const Stmt *stmt : following) {
    if (!visible && !isa<FunctionDecl>(stmt)) {
      continue;
    }
    if (auto *decl = dyn_cast<VarDeclStmt>(stmt);
        decl &&
        llvm::any_of(decl->decls, [&](const VarDeclarator &v) { return v.name == d.name; })) {
      visible = false;
    }
    checker_.destinationScanner().scanDestinations(stmt, scan);
  }
  checker_.diags_.setMuted(muted);

  // The places the literal fits, and one type that suits them all. A
  // parameter of a generic function takes whatever comes, so it says nothing
  // about the literal.
  std::vector<std::pair<const Type *, llvm::SMLoc>> places;
  for (auto [type, loc] : scan.found) {
    if (!type->isError() && !type->isVoid() && !mentionsParam(type) && literalFits(d.init, type)) {
      places.push_back({type, loc});
    }
  }
  const Type *chosen = nullptr;
  for (auto [type, loc] : places) {
    const Type *candidate = writable(type);
    if (llvm::all_of(places, [&](const std::pair<const Type *, llvm::SMLoc> &other) {
          return TypeRelations::isImplicitlyConvertible(candidate, other.first);
        })) {
      chosen = candidate;
      break;
    }
  }
  // Results meet as the returns of a function do: `let r = ok(1)` that is
  // given `err("x")` later is a `Result<int, string>`.
  if (!chosen && llvm::all_of(places, [](const std::pair<const Type *, llvm::SMLoc> &place) {
        return place.first->isResult();
      })) {
    for (auto [type, loc] : places) {
      chosen = chosen ? TypeRelations::unify(chosen, type) : type;
      if (!chosen || chosen->isError()) {
        chosen = nullptr;
        break;
      }
    }
  }
  if (!chosen && !places.empty()) {
    checker_.destinationConflict_ = std::move(places);
  }

  return chosen;
}

const Type *Destination::declaredFunctionType(const FunctionDecl *fn) {
  std::vector<const Type *> params;
  for (const Param &param : fn->params) {
    // `Iterable<T>` makes it generic: no one type.
    if (!param.type || checker_.iterableChecker().isIterableRef(*param.type)) {
      return nullptr;
    }
    const Type *type = checker_.typeResolver().resolveType(*param.type, /*allowVoid=*/false);
    params.push_back(param.optional || param.defaultValue ? Type::optional(type) : type);
  }
  bool hasRest = !fn->params.empty() && fn->params.back().rest;
  const Type *type = Type::function(params, 0, hasRest, Type::voidTy());

  return type->isFunction() ? type : nullptr;
}

} // namespace tz
