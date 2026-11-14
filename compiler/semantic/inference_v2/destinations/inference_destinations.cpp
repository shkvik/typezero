#include "compiler/semantic/inference_v2/destinations/inference_destinations.h"
#include "compiler/program/library/array/array_method_table/array_method_table.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/semantic/flow/placed_nodes/placed_nodes.h"
#include "compiler/semantic/inference_v2/destinations/destination_lookahead/destination_lookahead.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

InferenceDestinations::InferenceDestinations(SemanticState &owner)
    : state(owner),
      literalMembers(owner) {}

void InferenceDestinations::rememberDestinations(const VarSymbol *variable,
                                                 DestinationPlaces places) {
  this->destinations[variable] = std::move(places);
}

DestinationPlaces InferenceDestinations::getDestinations(const VarSymbol *variable) const {
  auto found = this->destinations.find(variable);
  if (found == this->destinations.end()) {
    return {};
  }

  return found->second;
}

DestinationFound InferenceDestinations::literalDestination(const VarDeclarator &declarator,
                                                           llvm::ArrayRef<VarDeclarator> later,
                                                           llvm::ArrayRef<const Stmt *> following,
                                                           DestinationTypes &types) {
  auto search = DestinationSearch(declarator.name, this->knownReturnType());
  search.enterScope();
  auto lookahead = DestinationLookahead(this->state, search, types);
  this->scanLaterDeclarators(later, lookahead, search);
  bool isVisible = !this->isRedeclaredIn(later, declarator.name);
  this->scanFollowing(following, declarator.name, isVisible, lookahead);
  auto places = this->fittingPlaces(declarator.init, search);
  const auto *chosen = this->chooseDestination(places);
  bool isConflict = chosen == nullptr && !places.empty();

  return DestinationFound{
      .hint = chosen,
      .conflict = isConflict ? std::move(places) : DestinationPlaces(),
  };
}

void InferenceDestinations::scanLaterDeclarators(llvm::ArrayRef<VarDeclarator> later,
                                                 DestinationLookahead &lookahead,
                                                 DestinationSearch &search) {
  for (const VarDeclarator &next : later) {
    const auto *declared = lookahead.annotatedType(next);
    if (next.init != nullptr) {
      lookahead.scanValue(next.init, declared);
    }
    search.declare(next.name, declared);
  }
}

void InferenceDestinations::scanFollowing(llvm::ArrayRef<const Stmt *> following,
                                          llvm::StringRef name, bool isVisibleAtStart,
                                          DestinationLookahead &lookahead) {
  bool isVisible = isVisibleAtStart;
  for (const Stmt *stmt : following) {
    if (isVisible || isa<FunctionDecl>(stmt)) {
      lookahead.scanStatement(stmt);
    }
    auto *varDecl = dyn_cast<VarDeclStmt>(stmt);
    if (varDecl != nullptr && this->isRedeclaredIn(varDecl->decls, name)) {
      isVisible = false;
    }
  }
}

bool InferenceDestinations::isResultLiteral(const CallExpr *call) const {
  auto *name = dyn_cast<NameExpr>(call->callee);
  if (name == nullptr) {
    return false;
  }
  auto global = GlobalNameTable::find(name->name);
  bool isOkOrErr = global == GlobalName::OK || global == GlobalName::ERR;

  return isOkOrErr && call->args.size() <= 1 && !call->hasSpread() && !call->optional;
}

std::vector<const Expr *> InferenceDestinations::findReturnValues(const Stmt *body) const {
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

const Type *InferenceDestinations::writable(const Type *type) const {
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

bool InferenceDestinations::hasReadonlyField(const Type *object) const {
  for (const Field &field : object->fields()) {
    if (field.readonly) {
      return true;
    }
  }

  return false;
}

const Type *InferenceDestinations::writableTuple(const Type *tuple) const {
  auto elements = std::vector<const Type *>();
  for (const Field &field : tuple->fields()) {
    elements.push_back(field.type);
  }

  return Type::tuple(elements);
}

bool InferenceDestinations::isLiteralShaped(const Expr *expr) const {
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

bool InferenceDestinations::isLiteralShape(const Expr *expr) const {
  if (isa<ObjectLitExpr>(expr) || isa<ArrayLitExpr>(expr)) {
    return true;
  }
  auto *call = dyn_cast<CallExpr>(expr);

  return call != nullptr && this->isResultLiteral(call);
}

std::vector<const Expr *> InferenceDestinations::shapeChoices(const Expr *expr) const {
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

std::vector<const Expr *> InferenceDestinations::mappedReturns(const CallExpr *call) const {
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

bool InferenceDestinations::isLiteralFitting(const Expr *expr, const Type *type) const {
  auto first = ShapeGoal{.expr = expr, .type = type};
  auto goals = std::vector<ShapeGoal>{first};
  while (!goals.empty()) {
    ShapeGoal goal = goals.back();
    goals.pop_back();
    auto needs = this->literalNeeds(goal.expr, goal.type);
    if (!needs.has_value()) {
      return false;
    }
    goals.insert(goals.end(), needs->rbegin(), needs->rend());
  }

  return true;
}

InferenceDestinations::ShapeNeeds InferenceDestinations::literalNeeds(const Expr *expr,
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

  return std::vector<ShapeGoal>();
}

InferenceDestinations::ShapeNeeds InferenceDestinations::unionNeeds(const Expr *expr,
                                                                    const Type *type) const {
  const Type *member = this->unionMemberFor(expr, type);
  if (member == nullptr) {
    return std::nullopt;
  }
  auto goal = ShapeGoal{.expr = expr, .type = member};

  return std::vector<ShapeGoal>{goal};
}

const Type *InferenceDestinations::unionMemberFor(const Expr *expr, const Type *type) const {
  auto *objectLiteral = dyn_cast<ObjectLitExpr>(expr);
  if (objectLiteral != nullptr) {
    return this->literalMembers.literalMember(objectLiteral, type);
  }
  if (isa<ArrayLitExpr>(expr)) {
    return type->slotPart(UnionSlot::Array);
  }

  return nullptr;
}

std::vector<InferenceDestinations::ShapeGoal>
InferenceDestinations::conditionalNeeds(const ConditionalExpr *conditional,
                                        const Type *type) const {
  auto needs = std::vector<ShapeGoal>();
  for (const Expr *branch : {conditional->thenExpr, conditional->elseExpr}) {
    if (this->isLiteralShaped(branch)) {
      needs.push_back(ShapeGoal{.expr = branch, .type = type});
    }
  }

  return needs;
}

InferenceDestinations::ShapeNeeds InferenceDestinations::objectNeeds(const ObjectLitExpr *literal,
                                                                     const Type *type) const {
  if (!type->isObject() || type->isTuple() || type->isIncomplete()) {
    return std::nullopt;
  }
  auto names = this->writtenNames(literal);
  if (!names.has_value() || !this->isCoveringFields(type, *names)) {
    return std::nullopt;
  }
  auto needs = std::vector<ShapeGoal>();
  for (const ObjectLitExpr::Property &property : literal->props) {
    if (!property.spread() && this->isLiteralShaped(property.value)) {
      const Field *field = type->field(property.name);
      needs.push_back(ShapeGoal{.expr = property.value, .type = field->type});
    }
  }

  return needs;
}

std::optional<std::vector<llvm::StringRef>>
InferenceDestinations::writtenNames(const ObjectLitExpr *literal) const {
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
InferenceDestinations::propertyNames(const ObjectLitExpr::Property &property) const {
  if (property.spread()) {
    return this->spreadNames(property);
  }
  if (property.key() != nullptr) {
    return std::nullopt;
  }

  return std::vector<llvm::StringRef>{property.name};
}

std::optional<std::vector<llvm::StringRef>>
InferenceDestinations::spreadNames(const ObjectLitExpr::Property &property) const {
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

const VarSymbol *InferenceDestinations::namedVariable(const Expr *expr) const {
  auto *name = dyn_cast<NameExpr>(expr);
  if (name == nullptr) {
    return nullptr;
  }
  auto entry = this->state.walk.scopes.findName(name->name);

  return entry.var();
}

bool InferenceDestinations::isCoveringFields(const Type *type,
                                             llvm::ArrayRef<llvm::StringRef> names) const {
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

InferenceDestinations::ShapeNeeds InferenceDestinations::arrayNeeds(const ArrayLitExpr *literal,
                                                                    const Type *type) const {
  if (type->isTuple()) {
    return this->tupleNeeds(literal, type);
  }
  if (!type->isArray()) {
    return std::nullopt;
  }
  auto needs = std::vector<ShapeGoal>();
  for (const ArrayLitExpr::Element &element : literal->elements) {
    if (!element.spread && this->isLiteralShaped(element.expr)) {
      needs.push_back(ShapeGoal{.expr = element.expr, .type = type->inner()});
    }
  }

  return needs;
}

InferenceDestinations::ShapeNeeds InferenceDestinations::tupleNeeds(const ArrayLitExpr *literal,
                                                                    const Type *tuple) const {
  if (literal->elements.size() != tuple->fields().size()) {
    return std::nullopt;
  }
  auto needs = std::vector<ShapeGoal>();
  for (size_t index = 0; index < literal->elements.size(); ++index) {
    const ArrayLitExpr::Element &element = literal->elements[index];
    if (element.spread) {
      return std::nullopt;
    }
    if (this->isLiteralShaped(element.expr)) {
      const Type *elementType = tuple->fields()[index].type;
      needs.push_back(ShapeGoal{.expr = element.expr, .type = elementType});
    }
  }

  return needs;
}

InferenceDestinations::ShapeNeeds InferenceDestinations::callNeeds(const CallExpr *call,
                                                                   const Type *type) const {
  if (this->isResultLiteral(call)) {
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
  auto needs = std::vector<ShapeGoal>();
  for (const Expr *value : this->findReturnValues(functionExpr->fn->body)) {
    if (this->isLiteralShaped(value)) {
      needs.push_back(ShapeGoal{.expr = value, .type = element});
    }
  }

  return needs;
}

InferenceDestinations::ShapeNeeds InferenceDestinations::resultNeeds(const CallExpr *call,
                                                                     const Type *type) const {
  if (!type->isResult()) {
    return std::nullopt;
  }
  auto *maker = cast<NameExpr>(call->callee);
  bool isOk = GlobalNameTable::find(maker->name) == GlobalName::OK;
  const Type *half = isOk ? type->okType() : type->errType();
  auto needs = std::vector<ShapeGoal>();
  if (!call->args.empty() && this->isLiteralShaped(call->args[0])) {
    needs.push_back(ShapeGoal{.expr = call->args[0], .type = half});
  }

  return needs;
}

const Type *InferenceDestinations::knownReturnType() const {
  auto *current = this->state.walk.functions.getCurrentFunction();
  if (current == nullptr) {
    return nullptr;
  }

  return current->getKnownReturnType();
}

bool InferenceDestinations::isRedeclaredIn(llvm::ArrayRef<VarDeclarator> decls,
                                           llvm::StringRef name) const {
  for (const VarDeclarator &declarator : decls) {
    if (declarator.name == name) {
      return true;
    }
  }

  return false;
}

std::vector<InferenceDestinations::DestinationPlace>
InferenceDestinations::fittingPlaces(const Expr *init, const DestinationSearch &search) {
  auto places = std::vector<DestinationPlace>();
  for (auto [type, loc] : search.getFound()) {
    bool isUsable = !type->isError() && !type->isVoid() && !TypeQueries::mentionsParam(type);
    if (isUsable && this->isLiteralFitting(init, type)) {
      places.push_back({type, loc});
    }
  }

  return places;
}

const Type *
InferenceDestinations::chooseDestination(llvm::ArrayRef<DestinationPlace> places) const {
  const auto *chosen = this->commonPlace(places);
  if (chosen != nullptr || !this->isAllResults(places)) {
    return chosen;
  }

  return this->unifiedResults(places);
}

const Type *InferenceDestinations::commonPlace(llvm::ArrayRef<DestinationPlace> places) const {
  for (auto [type, loc] : places) {
    const auto *candidate = this->writable(type);
    if (this->isAcceptingAll(candidate, places)) {
      return candidate;
    }
  }

  return nullptr;
}

bool InferenceDestinations::isAcceptingAll(const Type *candidate,
                                           llvm::ArrayRef<DestinationPlace> places) const {
  for (const DestinationPlace &other : places) {
    if (!this->state.types.relations.isImplicitlyConvertible(candidate, other.first)) {
      return false;
    }
  }

  return true;
}

bool InferenceDestinations::isAllResults(llvm::ArrayRef<DestinationPlace> places) const {
  for (const DestinationPlace &place : places) {
    if (!place.first->isResult()) {
      return false;
    }
  }

  return true;
}

const Type *InferenceDestinations::unifiedResults(llvm::ArrayRef<DestinationPlace> places) const {
  const Type *chosen = nullptr;
  for (auto [type, loc] : places) {
    chosen = chosen != nullptr ? this->state.types.relations.unify(chosen, type) : type;
    if (chosen == nullptr || chosen->isError()) {
      return nullptr;
    }
  }

  return chosen;
}

}
