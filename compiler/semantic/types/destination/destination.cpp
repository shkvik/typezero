#include "compiler/semantic/types/destination/destination.h"
#include "compiler/program/library/array/array_method_table/array_method_table.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/semantic/bodies/iterators/iterable_checker/iterable_checker.h"
#include "compiler/semantic/flow/placed_nodes/placed_nodes.h"
#include "compiler/semantic/names/scopes/scopes.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/destination_scan/destination_scan.h"
#include "compiler/semantic/types/destination_scanner/destination_scanner.h"
#include "compiler/semantic/types/literal_types/literal_types.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "compiler/semantic/types/type_resolver/type_resolver.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

Destination::Destination(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->scopes = &all.getScopes();
  this->literalTypes = &all.getLiteralTypes();
  this->typeResolver = &all.getTypeResolver();
  this->iterableChecker = &all.getIterableChecker();
  this->destinationScanner = &all.getDestinationScanner();
}

std::vector<const Expr *> Destination::findReturnValues(const Stmt *body) {
  std::vector<const Expr *> values;
  PlacedNodes nodes = PlacedNodes::collect({body}, {});
  for (const PlacedStmt &placed : nodes.getStmts()) {
    if (placed.place.isInsideFunction()) {
      continue;
    }
    auto *result = dyn_cast<ReturnStmt>(placed.stmt);
    if (result != nullptr && result->value != nullptr) {
      values.push_back(result->value);
    }
  }

  return values;
}

const Type *Destination::writable(const Type *type) {
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

bool Destination::literalShaped(const Expr *expr) {
  if (isa<ObjectLitExpr>(expr) || isa<ArrayLitExpr>(expr)) {
    return true;
  }
  if (auto *cond = dyn_cast<ConditionalExpr>(expr)) {
    return Destination::literalShaped(cond->thenExpr) || Destination::literalShaped(cond->elseExpr);
  }
  if (auto *call = dyn_cast<CallExpr>(expr)) {
    if (DestinationScanner::isResultLiteral(call)) {
      return true;
    }
    auto *member = dyn_cast<MemberExpr>(call->callee);
    if (member == nullptr || call->args.size() != 1) {
      return false;
    }
    const ArrayMethodInfo *method = ArrayMethodTable::find(member->name);
    bool maps = method != nullptr &&
                (method->method == ArrayMethod::MAP || method->method == ArrayMethod::FLAT_MAP);
    if (!maps) {
      return false;
    }
    auto *fe = dyn_cast<FunctionExpr>(call->args[0]);
    if (fe == nullptr) {
      return false;
    }
    for (const Expr *value : Destination::findReturnValues(fe->fn->body)) {
      if (Destination::literalShaped(value)) {
        return true;
      }
    }

    return false;
  }

  return false;
}

bool Destination::literalFits(const Expr *expr, const Type *type) const {
  type = type->nonOptional();
  if (type->isUnion()) {
    if (auto *lit = dyn_cast<ObjectLitExpr>(expr)) {
      if (const Type *member = this->literalTypes->literalMember(lit, type)) {
        return this->literalFits(expr, member);
      }
    }
    if (const Type *array = type->slotPart(UnionSlot::Array); array && isa<ArrayLitExpr>(expr)) {
      return this->literalFits(expr, array);
    }
    return false;
  }
  if (auto *lit = dyn_cast<ObjectLitExpr>(expr)) {
    if (!type->isObject() || type->isTuple() || type->isIncomplete()) {
      return false;
    }
    std::vector<llvm::StringRef> names;
    for (const ObjectLitExpr::Property &prop : lit->props) {
      if (prop.spread()) {
        auto *name = dyn_cast<NameExpr>(prop.value);
        const VarSymbol *var = name ? this->scopes->lookup(name->name).var() : nullptr;
        if (!var || !var->type || !var->type->isObject() || var->type->isTuple()) {
          return false;
        }
        for (const Field &f : var->type->fields()) {
          if (!llvm::is_contained(prop.omit(), f.name)) {
            names.push_back(f.name);
          }
        }
        continue;
      }
      if (prop.key()) {
        return false;
      }
      llvm::StringRef name = prop.name;
      const Field *field = type->field(name);
      if (!field ||
          (Destination::literalShaped(prop.value) && !this->literalFits(prop.value, field->type))) {
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
        if (e.spread || (Destination::literalShaped(e.expr) &&
                         !this->literalFits(e.expr, type->fields()[i].type))) {
          return false;
        }
      }
      return true;
    }
    if (!type->isArray()) {
      return false;
    }
    return llvm::all_of(lit->elements, [&](const ArrayLitExpr::Element &e) {
      return e.spread || !Destination::literalShaped(e.expr) ||
             this->literalFits(e.expr, type->inner());
    });
  }
  if (auto *cond = dyn_cast<ConditionalExpr>(expr)) {
    return (!Destination::literalShaped(cond->thenExpr) ||
            this->literalFits(cond->thenExpr, type)) &&
           (!Destination::literalShaped(cond->elseExpr) || this->literalFits(cond->elseExpr, type));
  }
  if (auto *call = dyn_cast<CallExpr>(expr)) {
    if (DestinationScanner::isResultLiteral(call)) {
      if (!type->isResult()) {
        return false;
      }
      auto *maker = cast<NameExpr>(call->callee);
      bool isOk = GlobalNameTable::find(maker->name) == GlobalName::OK;
      const Type *half = isOk ? type->okType() : type->errType();
      return call->args.empty() || !Destination::literalShaped(call->args[0]) ||
             this->literalFits(call->args[0], half);
    }
    if (!type->isArray()) {
      return false;
    }
    auto *fe = cast<FunctionExpr>(call->args[0]);
    auto *member = cast<MemberExpr>(call->callee);
    const ArrayMethodInfo *method = ArrayMethodTable::find(member->name);
    bool mapsEach = method != nullptr && method->method == ArrayMethod::MAP;
    const Type *element = mapsEach ? type->inner() : type;
    for (const Expr *value : Destination::findReturnValues(fe->fn->body)) {
      if (Destination::literalShaped(value) && !this->literalFits(value, element)) {
        return false;
      }
    }

    return true;
  }

  return true;
}

const Type *Destination::literalDestination(const VarDeclarator &d,
                                            llvm::ArrayRef<VarDeclarator> later) {
  FuncSymbol *current = this->state.getFunctionStack().getCurrentFunction();
  const Type *returned = current != nullptr ? current->getKnownReturnType() : nullptr;
  DestinationScan scan(d.name, returned);
  scan.enterScope();
  bool muted = this->state.getDiagnostics().setMuted(true);
  bool visible = true;
  for (const VarDeclarator &next : later) {
    if (next.init) {
      this->destinationScanner->scanDestinations(
          next.init, next.typeAnn ? this->typeResolver->resolveType(*next.typeAnn, false) : nullptr,
          scan);
    }
    if (next.name == d.name) {
      visible = false;
    }
    scan.declare(next.name,
                 next.typeAnn ? this->typeResolver->resolveType(*next.typeAnn, false) : nullptr);
  }
  std::vector<const Stmt *> following = this->state.getBlockPosition().takeFollowing();
  for (const Stmt *stmt : following) {
    if (!visible && !isa<FunctionDecl>(stmt)) {
      continue;
    }
    if (auto *decl = dyn_cast<VarDeclStmt>(stmt);
        decl &&
        llvm::any_of(decl->decls, [&](const VarDeclarator &v) { return v.name == d.name; })) {
      visible = false;
    }
    this->destinationScanner->scanDestinations(stmt, scan);
  }
  this->state.getDiagnostics().setMuted(muted);

  std::vector<std::pair<const Type *, llvm::SMLoc>> places;
  for (auto [type, loc] : scan.getFound()) {
    if (!type->isError() && !type->isVoid() && !TypeQueries::mentionsParam(type) &&
        this->literalFits(d.init, type)) {
      places.push_back({type, loc});
    }
  }
  const Type *chosen = nullptr;
  for (auto [type, loc] : places) {
    const Type *candidate = Destination::writable(type);
    if (llvm::all_of(places, [&](const std::pair<const Type *, llvm::SMLoc> &other) {
          return TypeRelations::isImplicitlyConvertible(candidate, other.first);
        })) {
      chosen = candidate;
      break;
    }
  }
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
    this->state.getTypeContext().rememberDestinationConflict(std::move(places));
  }

  return chosen;
}

const Type *Destination::declaredFunctionType(const FunctionDecl *fn) {
  std::vector<const Type *> params;
  for (const Param &param : fn->params) {
    if (!param.type || this->iterableChecker->isIterableRef(*param.type)) {
      return nullptr;
    }
    const Type *type = this->typeResolver->resolveType(*param.type, false);
    params.push_back(param.optional() || param.defaultValue() ? Type::optional(type) : type);
  }
  bool hasRest = !fn->params.empty() && fn->params.back().rest();
  const Type *type = Type::function(params, 0, hasRest, Type::voidTy());

  return type->isFunction() ? type : nullptr;
}

}
