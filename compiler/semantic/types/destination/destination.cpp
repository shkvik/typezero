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
  this->scopes = &all.scopes;
  this->literalTypes = &all.literalTypes;
  this->typeResolver = &all.typeResolver;
  this->iterableChecker = &all.iterableChecker;
  this->destinationScanner = &all.destinationScanner;
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
  std::vector<const Expr *> stack = {expr};
  while (!stack.empty()) {
    const Expr *next = stack.back();
    stack.pop_back();
    if (Destination::isLiteralShape(next)) {
      return true;
    }
    std::vector<const Expr *> choices = Destination::shapeChoices(next);
    stack.insert(stack.end(), choices.rbegin(), choices.rend());
  }

  return false;
}

bool Destination::isLiteralShape(const Expr *expr) {
  if (isa<ObjectLitExpr>(expr) || isa<ArrayLitExpr>(expr)) {
    return true;
  }
  auto *call = dyn_cast<CallExpr>(expr);

  return call != nullptr && DestinationScanner::isResultLiteral(call);
}

std::vector<const Expr *> Destination::shapeChoices(const Expr *expr) {
  if (auto *cond = dyn_cast<ConditionalExpr>(expr)) {
    return {cond->thenExpr, cond->elseExpr};
  }
  auto *call = dyn_cast<CallExpr>(expr);
  auto *member = call != nullptr ? dyn_cast<MemberExpr>(call->callee) : nullptr;
  if (member == nullptr || call->args.size() != 1) {
    return {};
  }
  const ArrayMethodInfo *method = ArrayMethodTable::find(member->name);
  bool isMapping = method != nullptr &&
                   (method->method == ArrayMethod::MAP || method->method == ArrayMethod::FLAT_MAP);
  auto *fe = isMapping ? dyn_cast<FunctionExpr>(call->args[0]) : nullptr;
  if (fe == nullptr) {
    return {};
  }

  return Destination::findReturnValues(fe->fn->body);
}

bool Destination::literalFits(const Expr *expr, const Type *type) const {
  std::vector<LiteralGoal> goals = {LiteralGoal{expr, type}};
  while (!goals.empty()) {
    LiteralGoal goal = goals.back();
    goals.pop_back();
    std::optional<std::vector<LiteralGoal>> needs = this->literalNeeds(goal.expr, goal.type);
    if (!needs.has_value()) {
      return false;
    }
    goals.insert(goals.end(), needs->rbegin(), needs->rend());
  }

  return true;
}

std::optional<std::vector<LiteralGoal>> Destination::literalNeeds(const Expr *expr,
                                                                  const Type *wanted) const {
  const Type *type = wanted->nonOptional();
  if (type->isUnion()) {
    return this->unionNeeds(expr, type);
  }
  if (auto *lit = dyn_cast<ObjectLitExpr>(expr)) {
    return this->objectNeeds(lit, type);
  }
  if (auto *lit = dyn_cast<ArrayLitExpr>(expr)) {
    return Destination::arrayNeeds(lit, type);
  }
  if (auto *cond = dyn_cast<ConditionalExpr>(expr)) {
    std::vector<LiteralGoal> needs;
    for (const Expr *branch : {cond->thenExpr, cond->elseExpr}) {
      if (Destination::literalShaped(branch)) {
        needs.push_back(LiteralGoal{branch, type});
      }
    }
    return needs;
  }
  if (auto *call = dyn_cast<CallExpr>(expr)) {
    return Destination::callNeeds(call, type);
  }

  return std::vector<LiteralGoal>();
}

std::optional<std::vector<LiteralGoal>> Destination::unionNeeds(const Expr *expr,
                                                                const Type *type) const {
  if (auto *lit = dyn_cast<ObjectLitExpr>(expr)) {
    if (const Type *member = this->literalTypes->literalMember(lit, type)) {
      return std::vector<LiteralGoal>{LiteralGoal{expr, member}};
    }
  }
  if (const Type *array = type->slotPart(UnionSlot::Array); array && isa<ArrayLitExpr>(expr)) {
    return std::vector<LiteralGoal>{LiteralGoal{expr, array}};
  }

  return std::nullopt;
}

std::optional<std::vector<LiteralGoal>> Destination::objectNeeds(const ObjectLitExpr *lit,
                                                                 const Type *type) const {
  if (!type->isObject() || type->isTuple() || type->isIncomplete()) {
    return std::nullopt;
  }
  std::vector<LiteralGoal> needs;
  std::vector<llvm::StringRef> names;
  for (const ObjectLitExpr::Property &prop : lit->props) {
    if (prop.spread()) {
      auto *name = dyn_cast<NameExpr>(prop.value);
      const VarSymbol *var = name ? this->scopes->lookup(name->name).var() : nullptr;
      if (!var || !var->type || !var->type->isObject() || var->type->isTuple()) {
        return std::nullopt;
      }
      for (const Field &f : var->type->fields()) {
        if (!llvm::is_contained(prop.omit(), f.name)) {
          names.push_back(f.name);
        }
      }
      continue;
    }
    if (prop.key()) {
      return std::nullopt;
    }
    const Field *field = type->field(prop.name);
    if (!field) {
      return std::nullopt;
    }
    if (Destination::literalShaped(prop.value)) {
      needs.push_back(LiteralGoal{prop.value, field->type});
    }
    names.push_back(prop.name);
  }
  for (llvm::StringRef name : names) {
    if (!type->field(name)) {
      return std::nullopt;
    }
  }
  bool isComplete = llvm::all_of(type->fields(), [&](const Field &f) {
    return f.type->isOptional() || llvm::is_contained(names, f.name);
  });
  if (!isComplete) {
    return std::nullopt;
  }

  return needs;
}

std::optional<std::vector<LiteralGoal>> Destination::arrayNeeds(const ArrayLitExpr *lit,
                                                                const Type *type) {
  std::vector<LiteralGoal> needs;
  if (type->isTuple()) {
    if (lit->elements.size() != type->fields().size()) {
      return std::nullopt;
    }
    for (size_t i = 0; i < lit->elements.size(); ++i) {
      const ArrayLitExpr::Element &e = lit->elements[i];
      if (e.spread) {
        return std::nullopt;
      }
      if (Destination::literalShaped(e.expr)) {
        needs.push_back(LiteralGoal{e.expr, type->fields()[i].type});
      }
    }
    return needs;
  }
  if (!type->isArray()) {
    return std::nullopt;
  }
  for (const ArrayLitExpr::Element &e : lit->elements) {
    if (!e.spread && Destination::literalShaped(e.expr)) {
      needs.push_back(LiteralGoal{e.expr, type->inner()});
    }
  }

  return needs;
}

std::optional<std::vector<LiteralGoal>> Destination::callNeeds(const CallExpr *call,
                                                               const Type *type) {
  std::vector<LiteralGoal> needs;
  if (DestinationScanner::isResultLiteral(call)) {
    if (!type->isResult()) {
      return std::nullopt;
    }
    auto *maker = cast<NameExpr>(call->callee);
    bool isOk = GlobalNameTable::find(maker->name) == GlobalName::OK;
    const Type *half = isOk ? type->okType() : type->errType();
    if (!call->args.empty() && Destination::literalShaped(call->args[0])) {
      needs.push_back(LiteralGoal{call->args[0], half});
    }
    return needs;
  }
  if (!type->isArray()) {
    return std::nullopt;
  }
  auto *fe = cast<FunctionExpr>(call->args[0]);
  auto *member = cast<MemberExpr>(call->callee);
  const ArrayMethodInfo *method = ArrayMethodTable::find(member->name);
  bool isMappingEach = method != nullptr && method->method == ArrayMethod::MAP;
  const Type *element = isMappingEach ? type->inner() : type;
  for (const Expr *value : Destination::findReturnValues(fe->fn->body)) {
    if (Destination::literalShaped(value)) {
      needs.push_back(LiteralGoal{value, element});
    }
  }

  return needs;
}

const Type *Destination::literalDestination(const VarDeclarator &d,
                                            llvm::ArrayRef<VarDeclarator> later) {
  FuncSymbol *current = this->state.getFunctionStack().getCurrentFunction();
  const Type *returned = current != nullptr ? current->getKnownReturnType() : nullptr;
  DestinationScan scan(d.name, returned);
  scan.enterScope();
  this->state.getDiagnostics().pushList(DiagnosticRoute::DISCARD);
  this->scanLaterDeclarators(later, scan);
  bool isVisible = !Destination::isRedeclaredIn(later, d.name);
  this->scanFollowing(d.name, isVisible, scan);
  this->state.getDiagnostics().popList();
  std::vector<DestinationPlace> places = this->fittingPlaces(d.init, scan);
  const Type *chosen = Destination::chooseDestination(places);
  if (chosen == nullptr && !places.empty()) {
    this->state.getTypeContext().rememberDestinationConflict(std::move(places));
  }

  return chosen;
}

void Destination::scanLaterDeclarators(llvm::ArrayRef<VarDeclarator> later, DestinationScan &scan) {
  for (const VarDeclarator &next : later) {
    if (next.init != nullptr) {
      const Type *wanted = this->annotatedType(next);
      this->destinationScanner->scanDestinations(next.init, wanted, scan);
    }
    const Type *declared = this->annotatedType(next);
    scan.declare(next.name, declared);
  }
}

const Type *Destination::annotatedType(const VarDeclarator &decl) {
  if (!decl.typeAnn.has_value()) {
    return nullptr;
  }

  return this->typeResolver->resolveType(*decl.typeAnn, false);
}

bool Destination::isRedeclaredIn(llvm::ArrayRef<VarDeclarator> decls, llvm::StringRef name) {
  for (const VarDeclarator &v : decls) {
    if (v.name == name) {
      return true;
    }
  }

  return false;
}

void Destination::scanFollowing(llvm::StringRef name, bool visible, DestinationScan &scan) {
  std::vector<const Stmt *> following = this->state.getBlockPosition().takeFollowing();
  for (const Stmt *stmt : following) {
    if (!visible && !isa<FunctionDecl>(stmt)) {
      continue;
    }
    auto *decl = dyn_cast<VarDeclStmt>(stmt);
    if (decl != nullptr && Destination::isRedeclaredIn(decl->decls, name)) {
      visible = false;
    }
    this->destinationScanner->scanDestinations(stmt, scan);
  }
}

std::vector<Destination::DestinationPlace> Destination::fittingPlaces(const Expr *init,
                                                                      const DestinationScan &scan) {
  std::vector<DestinationPlace> places;
  for (auto [type, loc] : scan.getFound()) {
    bool isFitting = !type->isError() && !type->isVoid() && !TypeQueries::mentionsParam(type) &&
                     this->literalFits(init, type);
    if (isFitting) {
      places.push_back({type, loc});
    }
  }

  return places;
}

const Type *Destination::chooseDestination(llvm::ArrayRef<DestinationPlace> places) {
  const Type *chosen = Destination::commonPlace(places);
  if (chosen != nullptr || !Destination::allResults(places)) {
    return chosen;
  }

  return Destination::unifiedResults(places);
}

const Type *Destination::commonPlace(llvm::ArrayRef<DestinationPlace> places) {
  for (auto [type, loc] : places) {
    const Type *candidate = Destination::writable(type);
    if (Destination::acceptsAll(candidate, places)) {
      return candidate;
    }
  }

  return nullptr;
}

bool Destination::acceptsAll(const Type *candidate, llvm::ArrayRef<DestinationPlace> places) {
  for (const DestinationPlace &other : places) {
    if (!TypeRelations::isImplicitlyConvertible(candidate, other.first)) {
      return false;
    }
  }

  return true;
}

bool Destination::allResults(llvm::ArrayRef<DestinationPlace> places) {
  for (const DestinationPlace &place : places) {
    if (!place.first->isResult()) {
      return false;
    }
  }

  return true;
}

const Type *Destination::unifiedResults(llvm::ArrayRef<DestinationPlace> places) {
  const Type *chosen = nullptr;
  for (auto [type, loc] : places) {
    chosen = chosen != nullptr ? TypeRelations::unify(chosen, type) : type;
    if (chosen == nullptr || chosen->isError()) {
      return nullptr;
    }
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
