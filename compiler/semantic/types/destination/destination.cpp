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
  this->typeRelations = &all.typeRelations;
  this->iterableChecker = &all.iterableChecker;
  this->destinationScanner = &all.destinationScanner;
}

std::vector<const Expr *> Destination::findReturnValues(const Stmt *body) const {
  auto values = std::vector<const Expr *>();
  auto nodes = PlacedNodes::collect({body}, {});
  for (const PlacedStmt &placed : nodes.getStmts()) {
    auto *returnStmt = dyn_cast<ReturnStmt>(placed.stmt);
    bool isOwnReturn = returnStmt != nullptr && !placed.place.isInsideFunction();
    if (isOwnReturn && returnStmt->value != nullptr) {
      values.push_back(returnStmt->value);
    }
  }

  return values;
}

const Type *Destination::writable(const Type *type) const {
  const Type *object = type->nonOptional();
  if (!object->isObject() || !this->hasReadonlyField(object)) {
    return object;
  }
  if (object->isTuple()) {
    return this->writableTuple(object);
  }
  auto fields = std::vector<Field>(object->fields().begin(), object->fields().end());
  for (Field &field : fields) {
    field.readonly = false;
  }

  return Type::object(std::move(fields));
}

bool Destination::hasReadonlyField(const Type *object) const {
  for (const Field &field : object->fields()) {
    if (field.readonly) {
      return true;
    }
  }

  return false;
}

const Type *Destination::writableTuple(const Type *tuple) const {
  auto elements = std::vector<const Type *>();
  for (const Field &field : tuple->fields()) {
    elements.push_back(field.type);
  }

  return Type::tuple(elements);
}

bool Destination::isLiteralShaped(const Expr *expr) const {
  auto stack = std::vector<const Expr *>{expr};
  while (!stack.empty()) {
    const Expr *next = stack.back();
    stack.pop_back();
    if (this->isLiteralShape(next)) {
      return true;
    }
    auto choices = this->shapeChoices(next);
    stack.insert(stack.end(), choices.rbegin(), choices.rend());
  }

  return false;
}

bool Destination::isLiteralShape(const Expr *expr) const {
  if (isa<ObjectLitExpr>(expr) || isa<ArrayLitExpr>(expr)) {
    return true;
  }
  auto *call = dyn_cast<CallExpr>(expr);

  return call != nullptr && this->destinationScanner->isResultLiteral(call);
}

std::vector<const Expr *> Destination::shapeChoices(const Expr *expr) const {
  auto *conditional = dyn_cast<ConditionalExpr>(expr);
  auto *call = dyn_cast<CallExpr>(expr);

  if (conditional != nullptr) {
    return {conditional->thenExpr, conditional->elseExpr};
  }
  if (call != nullptr) {
    return this->mappedReturns(call);
  }

  return {};
}

std::vector<const Expr *> Destination::mappedReturns(const CallExpr *call) const {
  auto *member = dyn_cast<MemberExpr>(call->callee);
  if (member == nullptr || call->args.size() != 1) {
    return {};
  }
  const auto *method = ArrayMethodTable::find(member->name);
  bool isMapping = method != nullptr &&
                   (method->method == ArrayMethod::MAP || method->method == ArrayMethod::FLAT_MAP);
  if (!isMapping) {
    return {};
  }
  auto *functionExpr = dyn_cast<FunctionExpr>(call->args[0]);
  if (functionExpr == nullptr) {
    return {};
  }

  return this->findReturnValues(functionExpr->fn->body);
}

bool Destination::isLiteralFitting(const Expr *expr, const Type *type) const {
  auto first = LiteralGoal{.expr = expr, .type = type};
  auto goals = std::vector<LiteralGoal>{first};
  while (!goals.empty()) {
    LiteralGoal goal = goals.back();
    goals.pop_back();
    auto needs = this->literalNeeds(goal.expr, goal.type);
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
  auto *objectLiteral = dyn_cast<ObjectLitExpr>(expr);
  auto *arrayLiteral = dyn_cast<ArrayLitExpr>(expr);
  auto *conditional = dyn_cast<ConditionalExpr>(expr);
  auto *call = dyn_cast<CallExpr>(expr);

  if (type->isUnion()) {
    return this->unionNeeds(expr, type);
  }
  if (objectLiteral != nullptr) {
    return this->objectNeeds(objectLiteral, type);
  }
  if (arrayLiteral != nullptr) {
    return this->arrayNeeds(arrayLiteral, type);
  }
  if (conditional != nullptr) {
    return this->conditionalNeeds(conditional, type);
  }
  if (call != nullptr) {
    return this->callNeeds(call, type);
  }

  return std::vector<LiteralGoal>();
}

std::optional<std::vector<LiteralGoal>> Destination::unionNeeds(const Expr *expr,
                                                                const Type *type) const {
  const Type *member = this->unionMemberFor(expr, type);
  if (member == nullptr) {
    return std::nullopt;
  }
  auto goal = LiteralGoal{.expr = expr, .type = member};

  return std::vector<LiteralGoal>{goal};
}

const Type *Destination::unionMemberFor(const Expr *expr, const Type *type) const {
  auto *objectLiteral = dyn_cast<ObjectLitExpr>(expr);
  if (objectLiteral != nullptr) {
    return this->literalTypes->literalMember(objectLiteral, type);
  }
  if (isa<ArrayLitExpr>(expr)) {
    return type->slotPart(UnionSlot::Array);
  }

  return nullptr;
}

std::vector<LiteralGoal> Destination::conditionalNeeds(const ConditionalExpr *conditional,
                                                       const Type *type) const {
  auto needs = std::vector<LiteralGoal>();
  for (const Expr *branch : {conditional->thenExpr, conditional->elseExpr}) {
    if (this->isLiteralShaped(branch)) {
      needs.push_back(LiteralGoal{.expr = branch, .type = type});
    }
  }

  return needs;
}

std::optional<std::vector<LiteralGoal>> Destination::objectNeeds(const ObjectLitExpr *literal,
                                                                 const Type *type) const {
  if (!type->isObject() || type->isTuple() || type->isIncomplete()) {
    return std::nullopt;
  }
  auto names = this->writtenNames(literal);
  if (!names.has_value() || !this->isCoveringFields(type, *names)) {
    return std::nullopt;
  }
  auto needs = std::vector<LiteralGoal>();
  for (const ObjectLitExpr::Property &property : literal->props) {
    if (!property.spread() && this->isLiteralShaped(property.value)) {
      const Field *field = type->field(property.name);
      needs.push_back(LiteralGoal{.expr = property.value, .type = field->type});
    }
  }

  return needs;
}

std::optional<std::vector<llvm::StringRef>>
Destination::writtenNames(const ObjectLitExpr *literal) const {
  auto names = std::vector<llvm::StringRef>();
  for (const ObjectLitExpr::Property &property : literal->props) {
    auto added = this->propertyNames(property);
    if (!added.has_value()) {
      return std::nullopt;
    }
    names.insert(names.end(), added->begin(), added->end());
  }

  return names;
}

std::optional<std::vector<llvm::StringRef>>
Destination::propertyNames(const ObjectLitExpr::Property &property) const {
  if (property.spread()) {
    return this->spreadNames(property);
  }
  if (property.key() != nullptr) {
    return std::nullopt;
  }

  return std::vector<llvm::StringRef>{property.name};
}

std::optional<std::vector<llvm::StringRef>>
Destination::spreadNames(const ObjectLitExpr::Property &property) const {
  const VarSymbol *variable = this->namedVariable(property.value);
  bool isPlainObject = variable != nullptr && variable->type != nullptr &&
                       variable->type->isObject() && !variable->type->isTuple();
  if (!isPlainObject) {
    return std::nullopt;
  }
  auto names = std::vector<llvm::StringRef>();
  for (const Field &field : variable->type->fields()) {
    if (!llvm::is_contained(property.omit(), field.name)) {
      names.push_back(field.name);
    }
  }

  return names;
}

const VarSymbol *Destination::namedVariable(const Expr *expr) const {
  auto *name = dyn_cast<NameExpr>(expr);
  if (name == nullptr) {
    return nullptr;
  }
  auto entry = this->scopes->lookup(name->name);

  return entry.var();
}

bool Destination::isCoveringFields(const Type *type, llvm::ArrayRef<llvm::StringRef> names) const {
  for (llvm::StringRef name : names) {
    if (type->field(name) == nullptr) {
      return false;
    }
  }
  for (const Field &field : type->fields()) {
    if (!field.type->isOptional() && !llvm::is_contained(names, field.name)) {
      return false;
    }
  }

  return true;
}

std::optional<std::vector<LiteralGoal>> Destination::arrayNeeds(const ArrayLitExpr *literal,
                                                                const Type *type) const {
  if (type->isTuple()) {
    return this->tupleNeeds(literal, type);
  }
  if (!type->isArray()) {
    return std::nullopt;
  }
  auto needs = std::vector<LiteralGoal>();
  for (const ArrayLitExpr::Element &element : literal->elements) {
    if (!element.spread && this->isLiteralShaped(element.expr)) {
      needs.push_back(LiteralGoal{.expr = element.expr, .type = type->inner()});
    }
  }

  return needs;
}

std::optional<std::vector<LiteralGoal>> Destination::tupleNeeds(const ArrayLitExpr *literal,
                                                                const Type *tuple) const {
  if (literal->elements.size() != tuple->fields().size()) {
    return std::nullopt;
  }
  auto needs = std::vector<LiteralGoal>();
  for (size_t index = 0; index < literal->elements.size(); ++index) {
    const ArrayLitExpr::Element &element = literal->elements[index];
    if (element.spread) {
      return std::nullopt;
    }
    if (this->isLiteralShaped(element.expr)) {
      const Type *elementType = tuple->fields()[index].type;
      needs.push_back(LiteralGoal{.expr = element.expr, .type = elementType});
    }
  }

  return needs;
}

std::optional<std::vector<LiteralGoal>> Destination::callNeeds(const CallExpr *call,
                                                               const Type *type) const {
  if (this->destinationScanner->isResultLiteral(call)) {
    return this->resultNeeds(call, type);
  }
  if (!type->isArray()) {
    return std::nullopt;
  }
  auto *functionExpr = cast<FunctionExpr>(call->args[0]);
  auto *member = cast<MemberExpr>(call->callee);
  const auto *method = ArrayMethodTable::find(member->name);
  bool isMappingEach = method != nullptr && method->method == ArrayMethod::MAP;
  const Type *element = isMappingEach ? type->inner() : type;
  auto needs = std::vector<LiteralGoal>();
  for (const Expr *value : this->findReturnValues(functionExpr->fn->body)) {
    if (this->isLiteralShaped(value)) {
      needs.push_back(LiteralGoal{.expr = value, .type = element});
    }
  }

  return needs;
}

std::optional<std::vector<LiteralGoal>> Destination::resultNeeds(const CallExpr *call,
                                                                 const Type *type) const {
  if (!type->isResult()) {
    return std::nullopt;
  }
  auto *maker = cast<NameExpr>(call->callee);
  bool isOk = GlobalNameTable::find(maker->name) == GlobalName::OK;
  const Type *half = isOk ? type->okType() : type->errType();
  auto needs = std::vector<LiteralGoal>();
  if (!call->args.empty() && this->isLiteralShaped(call->args[0])) {
    needs.push_back(LiteralGoal{.expr = call->args[0], .type = half});
  }

  return needs;
}

const Type *Destination::literalDestination(const VarDeclarator &declarator,
                                            llvm::ArrayRef<VarDeclarator> later) {
  auto scan = DestinationScan(declarator.name, this->knownReturnType());
  scan.enterScope();
  this->state.getDiagnostics().pushList(DiagnosticRoute::DISCARD);
  this->scanLaterDeclarators(later, scan);
  bool isVisible = !this->isRedeclaredIn(later, declarator.name);
  this->scanFollowing(declarator.name, isVisible, scan);
  this->state.getDiagnostics().popList();
  auto places = this->fittingPlaces(declarator.init, scan);
  const auto *chosen = this->chooseDestination(places);
  if (chosen == nullptr && !places.empty()) {
    this->state.getTypeContext().rememberDestinationConflict(std::move(places));
  }

  return chosen;
}

const Type *Destination::knownReturnType() const {
  auto *current = this->state.getFunctionStack().getCurrentFunction();
  if (current == nullptr) {
    return nullptr;
  }

  return current->getKnownReturnType();
}

void Destination::scanLaterDeclarators(llvm::ArrayRef<VarDeclarator> later, DestinationScan &scan) {
  for (const VarDeclarator &next : later) {
    const auto *declared = this->annotatedType(next);
    if (next.init != nullptr) {
      this->destinationScanner->scanDestinations(next.init, declared, scan);
    }
    scan.declare(next.name, declared);
  }
}

const Type *Destination::annotatedType(const VarDeclarator &declarator) {
  if (!declarator.typeAnn.has_value()) {
    return nullptr;
  }

  return this->typeResolver->resolveType(*declarator.typeAnn);
}

bool Destination::isRedeclaredIn(llvm::ArrayRef<VarDeclarator> decls, llvm::StringRef name) const {
  for (const VarDeclarator &declarator : decls) {
    if (declarator.name == name) {
      return true;
    }
  }

  return false;
}

void Destination::scanFollowing(llvm::StringRef name, bool isVisibleAtStart,
                                DestinationScan &scan) {
  bool isVisible = isVisibleAtStart;
  auto following = this->state.getBlockPosition().takeFollowing();
  for (const Stmt *stmt : following) {
    if (isVisible || isa<FunctionDecl>(stmt)) {
      this->destinationScanner->scanDestinations(stmt, scan);
    }
    auto *varDecl = dyn_cast<VarDeclStmt>(stmt);
    if (varDecl != nullptr && this->isRedeclaredIn(varDecl->decls, name)) {
      isVisible = false;
    }
  }
}

std::vector<Destination::DestinationPlace> Destination::fittingPlaces(const Expr *init,
                                                                      const DestinationScan &scan) {
  auto places = std::vector<DestinationPlace>();
  for (auto [type, loc] : scan.getFound()) {
    bool isUsable = !type->isError() && !type->isVoid() && !TypeQueries::mentionsParam(type);
    if (isUsable && this->isLiteralFitting(init, type)) {
      places.push_back({type, loc});
    }
  }

  return places;
}

const Type *Destination::chooseDestination(llvm::ArrayRef<DestinationPlace> places) const {
  const auto *chosen = this->commonPlace(places);
  if (chosen != nullptr || !this->isAllResults(places)) {
    return chosen;
  }

  return this->unifiedResults(places);
}

const Type *Destination::commonPlace(llvm::ArrayRef<DestinationPlace> places) const {
  for (auto [type, loc] : places) {
    const auto *candidate = this->writable(type);
    if (this->isAcceptingAll(candidate, places)) {
      return candidate;
    }
  }

  return nullptr;
}

bool Destination::isAcceptingAll(const Type *candidate,
                                 llvm::ArrayRef<DestinationPlace> places) const {
  for (const DestinationPlace &other : places) {
    if (!TypeRelations::isImplicitlyConvertible(candidate, other.first)) {
      return false;
    }
  }

  return true;
}

bool Destination::isAllResults(llvm::ArrayRef<DestinationPlace> places) const {
  for (const DestinationPlace &place : places) {
    if (!place.first->isResult()) {
      return false;
    }
  }

  return true;
}

const Type *Destination::unifiedResults(llvm::ArrayRef<DestinationPlace> places) const {
  const Type *chosen = nullptr;
  for (auto [type, loc] : places) {
    chosen = chosen != nullptr ? this->typeRelations->unify(chosen, type) : type;
    if (chosen == nullptr || chosen->isError()) {
      return nullptr;
    }
  }

  return chosen;
}

const Type *Destination::declaredFunctionType(const FunctionDecl *functionDecl) {
  auto params = std::vector<const Type *>();
  for (const Param &param : functionDecl->params) {
    if (!param.type.has_value() || this->iterableChecker->isIterableRef(*param.type)) {
      return nullptr;
    }
    const auto *type = this->typeResolver->resolveType(*param.type);
    bool isOptional = param.optional() || param.defaultValue() != nullptr;
    params.push_back(isOptional ? Type::optional(type) : type);
  }
  bool hasRest = !functionDecl->params.empty() && functionDecl->params.back().rest();
  const auto *type = Type::function(params, 0, hasRest, Type::voidTy());
  if (!type->isFunction()) {
    return nullptr;
  }

  return type;
}

}
