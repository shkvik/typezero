#include "compiler/semantic/inference_v2/destinations/destination_lookahead/destination_lookahead.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;

namespace tz {

DestinationLookahead::DestinationLookahead(SemanticState &owner, DestinationSearch &found,
                                           DestinationTypes &known)
    : state(owner),
      search(found),
      types(known),
      peeks(owner) {}

void DestinationLookahead::scanStatement(const Stmt *stmt) {
  return this->runScan(LookStmt{.stmt = stmt});
}

void DestinationLookahead::scanValue(const Expr *expr, const Type *sink) {
  return this->runScan(LookExpr{.expr = expr, .sink = sink});
}

void DestinationLookahead::runScan(LookStep first) {
  this->steps.push_back(std::move(first));
  while (!this->steps.empty()) {
    LookStep step = std::move(this->steps.back());
    this->steps.pop_back();
    this->applyStep(step);
  }
}

void DestinationLookahead::applyStep(const LookStep &step) {
  switch (step.kind()) {
    case LookStepKind::START_LIST:
      this->startList(*step.as<LookList>());
      break;
    case LookStepKind::LIST_ITEM:
      this->listItem(*step.as<LookItem>());
      break;
    case LookStepKind::STMT:
      this->scanStmt(step.as<LookStmt>()->stmt);
      break;
    case LookStepKind::EXPR:
      this->scanExpr(*step.as<LookExpr>());
      break;
    case LookStepKind::VAR_DECLARE:
      this->varDeclare(step.as<LookDeclarator>()->declarator);
      break;
    case LookStepKind::DECLARE_NAME:
      this->search.declare(step.as<LookName>()->name, step.as<LookName>()->type);
      break;
    case LookStepKind::LEAVE_SCOPE:
      this->search.leaveScope();
      break;
    case LookStepKind::FOR_REST:
      this->forRest(step.as<LookForRest>()->loop);
      break;
    case LookStepKind::FOR_OF_REST:
      this->forOfRest(step.as<LookForOfRest>()->loop);
      break;
    case LookStepKind::FUNCTION:
      this->scanFunction(step.as<LookFunction>()->functionDecl);
      break;
    case LookStepKind::PARAM:
      this->scanParam(step.as<LookParam>()->param);
      break;
    case LookStepKind::FUNCTION_END:
      this->functionEnd(step.as<LookFunctionEnd>()->outerReturns);
      break;
    case LookStepKind::ASSIGN_VALUE:
      this->assignValue(step.as<LookAssign>()->assign);
      break;
  }
}

void DestinationLookahead::startList(const LookList &list) {
  if (this->isDeclaringFunction(list.stmts, this->search.getName())) {
    return;
  }
  this->search.enterScope();
  for (const Stmt *stmt : list.stmts) {
    auto *functionDecl = dyn_cast<FunctionDecl>(stmt);
    if (functionDecl != nullptr) {
      const auto *type = this->declaredFunctionType(functionDecl);
      this->search.declare(functionDecl->name, type);
    }
  }
  this->steps.push_back(LookItem{.stmts = list.stmts, .index = 0});
}

bool DestinationLookahead::isDeclaringFunction(llvm::ArrayRef<Stmt *> stmts,
                                               llvm::StringRef name) const {
  for (const Stmt *stmt : stmts) {
    auto *functionDecl = dyn_cast<FunctionDecl>(stmt);
    if (functionDecl != nullptr && functionDecl->name == name) {
      return true;
    }
  }

  return false;
}

void DestinationLookahead::listItem(const LookItem &item) {
  bool isAfterDeclaration =
      item.index > 0 && this->isDeclaringName(item.stmts[item.index - 1], this->search.getName());
  if (item.index == item.stmts.size() || isAfterDeclaration) {
    this->search.leaveScope();
    return;
  }
  this->steps.push_back(LookItem{.stmts = item.stmts, .index = item.index + 1});
  this->steps.push_back(LookStmt{.stmt = item.stmts[item.index]});
}

bool DestinationLookahead::isDeclaringName(const Stmt *stmt, llvm::StringRef name) const {
  auto *varDecl = dyn_cast<VarDeclStmt>(stmt);
  if (varDecl == nullptr) {
    return false;
  }
  for (const VarDeclarator &declarator : varDecl->decls) {
    if (declarator.name == name) {
      return true;
    }
  }

  return false;
}

void DestinationLookahead::scanStmt(const Stmt *stmt) {
  if (stmt == nullptr) {
    return;
  }
  switch (stmt->kind()) {
    case Stmt::Kind::Block:
      return this->steps.push_back(LookList{.stmts = cast<BlockStmt>(stmt)->body});
    case Stmt::Kind::VarDecl:
      return this->pushDeclarators(cast<VarDeclStmt>(stmt));
    case Stmt::Kind::Function:
      return this->steps.push_back(LookFunction{.functionDecl = cast<FunctionDecl>(stmt)});
    case Stmt::Kind::Expr:
      return this->steps.push_back(LookExpr{.expr = cast<ExprStmt>(stmt)->expr, .sink = nullptr});
    case Stmt::Kind::If:
      return this->pushIfParts(cast<IfStmt>(stmt));
    case Stmt::Kind::While:
      return this->pushWhileParts(cast<WhileStmt>(stmt));
    case Stmt::Kind::DoWhile:
      return this->pushDoWhileParts(cast<DoWhileStmt>(stmt));
    case Stmt::Kind::For:
      return this->pushForParts(cast<ForStmt>(stmt));
    case Stmt::Kind::ForOf:
      return this->pushForOfParts(cast<ForOfStmt>(stmt));
    case Stmt::Kind::Switch:
      return this->scanSwitch(cast<SwitchStmt>(stmt));
    case Stmt::Kind::Labeled:
      return this->steps.push_back(LookStmt{.stmt = cast<LabeledStmt>(stmt)->body});
    case Stmt::Kind::Return:
      return this->pushReturnValue(cast<ReturnStmt>(stmt));
    case Stmt::Kind::Break:
    case Stmt::Kind::Continue:
    case Stmt::Kind::Empty:
    case Stmt::Kind::TypeAlias:
    case Stmt::Kind::Enum:
    case Stmt::Kind::Debugger:
    case Stmt::Kind::Class:
      return;
  }
}

void DestinationLookahead::pushDeclarators(const VarDeclStmt *varDecl) {
  for (const VarDeclarator &declarator : llvm::reverse(varDecl->decls)) {
    this->steps.push_back(LookDeclarator{.declarator = &declarator});
  }
}

void DestinationLookahead::pushIfParts(const IfStmt *ifStmt) {
  this->steps.push_back(LookStmt{.stmt = ifStmt->elseStmt});
  this->steps.push_back(LookStmt{.stmt = ifStmt->thenStmt});
  this->steps.push_back(LookExpr{.expr = ifStmt->cond, .sink = nullptr});
}

void DestinationLookahead::pushWhileParts(const WhileStmt *loop) {
  this->steps.push_back(LookStmt{.stmt = loop->body});
  this->steps.push_back(LookExpr{.expr = loop->cond, .sink = nullptr});
}

void DestinationLookahead::pushDoWhileParts(const DoWhileStmt *loop) {
  this->steps.push_back(LookExpr{.expr = loop->cond, .sink = nullptr});
  this->steps.push_back(LookStmt{.stmt = loop->body});
}

void DestinationLookahead::pushForParts(const ForStmt *loop) {
  this->search.enterScope();
  this->steps.push_back(LookForRest{.loop = loop});
  this->steps.push_back(LookStmt{.stmt = loop->init});
}

void DestinationLookahead::pushForOfParts(const ForOfStmt *loop) {
  this->steps.push_back(LookForOfRest{.loop = loop});
  this->steps.push_back(LookExpr{.expr = loop->iterable, .sink = nullptr});
}

void DestinationLookahead::pushReturnValue(const ReturnStmt *returnStmt) {
  if (returnStmt->value == nullptr) {
    return;
  }
  const Type *returns = this->search.getReturns();
  bool isTyped = returns != nullptr && !returns->isVoid();
  const Type *sink = isTyped ? returns : nullptr;

  return this->steps.push_back(LookExpr{.expr = returnStmt->value, .sink = sink});
}

void DestinationLookahead::scanSwitch(const SwitchStmt *switchStmt) {
  std::vector<Stmt *> &bodies = this->caseBodies.emplace_back();
  for (const SwitchCase &switchCase : switchStmt->cases) {
    bodies.insert(bodies.end(), switchCase.body.begin(), switchCase.body.end());
  }
  this->steps.push_back(LookList{.stmts = bodies});
  for (const SwitchCase &switchCase : llvm::reverse(switchStmt->cases)) {
    this->steps.push_back(LookExpr{.expr = switchCase.test, .sink = nullptr});
  }
  this->steps.push_back(LookExpr{.expr = switchStmt->discriminant, .sink = nullptr});
}

void DestinationLookahead::varDeclare(const VarDeclarator *declarator) {
  const Type *type = this->annotatedType(*declarator);
  this->steps.push_back(LookName{.name = declarator->name, .type = type});
  if (declarator->init != nullptr) {
    this->steps.push_back(LookExpr{.expr = declarator->init, .sink = type});
  }
}

void DestinationLookahead::forRest(const ForStmt *loop) {
  this->steps.push_back(LookScopeEnd{});
  if (!this->search.isDeclaredInnermost(this->search.getName())) {
    this->steps.push_back(LookStmt{.stmt = loop->body});
    this->steps.push_back(LookExpr{.expr = loop->step, .sink = nullptr});
    this->steps.push_back(LookExpr{.expr = loop->cond, .sink = nullptr});
  }
}

void DestinationLookahead::forOfRest(const ForOfStmt *loop) {
  if (loop->binding.name != this->search.getName()) {
    this->search.enterScope();
    this->search.declare(loop->binding.name, nullptr);
    this->steps.push_back(LookScopeEnd{});
    this->steps.push_back(LookStmt{.stmt = loop->body});
  }
}

void DestinationLookahead::scanFunction(const FunctionDecl *functionDecl) {
  bool isHidden = this->hasParamNamed(functionDecl, this->search.getName());
  if (functionDecl->body == nullptr || isHidden) {
    return;
  }
  const Type *outer = this->search.getReturns();
  const Type *returned = this->writtenReturnType(functionDecl);
  this->search.setReturns(returned);
  this->search.enterScope();
  this->steps.push_back(LookFunctionEnd{.outerReturns = outer});
  this->steps.push_back(LookList{.stmts = functionDecl->body->body});
  for (const Param &param : llvm::reverse(functionDecl->params)) {
    this->steps.push_back(LookParam{.param = &param});
  }
}

bool DestinationLookahead::hasParamNamed(const FunctionDecl *functionDecl,
                                         llvm::StringRef name) const {
  for (const Param &param : functionDecl->params) {
    if (param.name == name) {
      return true;
    }
  }

  return false;
}

const Type *DestinationLookahead::writtenReturnType(const FunctionDecl *functionDecl) {
  if (!functionDecl->returnType.has_value()) {
    return nullptr;
  }
  const TypeRef &ref = *functionDecl->returnType;
  if (ref.kind() != TypeRefKind::PREDICATE_REF) {
    return this->knownType(ref, TypeUse::STANDALONE_OR_VOID);
  }

  return ref.asserts() ? Type::voidTy() : Type::boolTy();
}

void DestinationLookahead::scanParam(const Param *param) {
  const Type *type = this->writtenParamType(*param);
  bool isOptional = type != nullptr && param->optional();
  const Type *declared = isOptional ? Type::optional(type) : type;
  this->steps.push_back(LookName{.name = param->name, .type = declared});
  if (param->defaultValue() != nullptr) {
    this->steps.push_back(LookExpr{.expr = param->defaultValue(), .sink = type});
  }
}

const Type *DestinationLookahead::writtenParamType(const Param &param) {
  if (!param.type.has_value() || this->state.isIterableRef(*param.type)) {
    return nullptr;
  }

  return this->knownType(*param.type, TypeUse::STANDALONE);
}

void DestinationLookahead::functionEnd(const Type *outerReturns) {
  this->search.leaveScope();
  this->search.setReturns(outerReturns);
}

void DestinationLookahead::assignValue(const AssignExpr *assign) {
  const Type *sink = this->plainAssignType(assign, assign->target);
  this->steps.push_back(LookExpr{.expr = assign->value, .sink = sink});
}

const Type *DestinationLookahead::plainAssignType(const AssignExpr *assign,
                                                  const Expr *side) const {
  if (assign->op.has_value()) {
    return nullptr;
  }

  return this->peeks.peekType(side, this->search);
}

void DestinationLookahead::scanExpr(const LookExpr &scanned) {
  const Expr *expr = scanned.expr;
  if (expr == nullptr) {
    return;
  }
  bool isTyped = scanned.sink != nullptr && !scanned.sink->isError();
  const Type *sink = isTyped ? scanned.sink : nullptr;
  auto parts = this->exprParts(expr, sink);
  for (const LookExpr &part : llvm::reverse(parts)) {
    this->steps.push_back(part);
  }
}

std::vector<LookExpr> DestinationLookahead::exprParts(const Expr *expr, const Type *sink) {
  const Type *object = sink != nullptr ? sink->nonOptional() : nullptr;
  switch (expr->kind()) {
    case Expr::Kind::Name:
      return this->nameParts(cast<NameExpr>(expr), sink);
    case Expr::Kind::ObjectLit:
      return this->objectParts(cast<ObjectLitExpr>(expr), object);
    case Expr::Kind::ArrayLit:
      return this->arrayParts(cast<ArrayLitExpr>(expr), object);
    case Expr::Kind::Conditional:
      return this->conditionalParts(cast<ConditionalExpr>(expr), sink);
    case Expr::Kind::Binary:
      return this->binaryParts(cast<BinaryExpr>(expr), sink);
    case Expr::Kind::Sequence:
      return this->sequenceParts(cast<SequenceExpr>(expr), sink);
    case Expr::Kind::Assign:
      return this->assignParts(cast<AssignExpr>(expr));
    case Expr::Kind::Call:
      return this->callParts(cast<CallExpr>(expr));
    case Expr::Kind::Function:
      this->steps.push_back(LookFunction{.functionDecl = cast<FunctionExpr>(expr)->fn});
      return {};
    case Expr::Kind::Template:
      return this->templateParts(cast<TemplateExpr>(expr));
    case Expr::Kind::Member:
      return {this->untyped(cast<MemberExpr>(expr)->object)};
    case Expr::Kind::Index:
      return {this->untyped(cast<IndexExpr>(expr)->object),
              this->untyped(cast<IndexExpr>(expr)->index)};
    case Expr::Kind::Unary:
      return {this->untyped(cast<UnaryExpr>(expr)->operand)};
    case Expr::Kind::Update:
      return {this->untyped(cast<UpdateExpr>(expr)->target)};
    case Expr::Kind::New:
      return this->newParts(cast<NewExpr>(expr));
    case Expr::Kind::Cast:
      return {this->untyped(cast<CastExpr>(expr)->operand)};
    case Expr::Kind::OptionalChain:
      return {this->untyped(cast<OptionalChainExpr>(expr)->chain)};
    case Expr::Kind::Yield:
      return {this->untyped(cast<YieldExpr>(expr)->value)};
    case Expr::Kind::Await:
      return {this->untyped(cast<AwaitExpr>(expr)->value)};
    case Expr::Kind::IntLit:
    case Expr::Kind::FloatLit:
    case Expr::Kind::BoolLit:
    case Expr::Kind::StringLit:
    case Expr::Kind::Undefined:
    case Expr::Kind::RegexLit:
    case Expr::Kind::Class:
      return {};
  }

  return {};
}

LookExpr DestinationLookahead::untyped(const Expr *expr) const {
  return LookExpr{.expr = expr, .sink = nullptr};
}

std::vector<LookExpr> DestinationLookahead::nameParts(const NameExpr *name,
                                                      const Type *sink) const {
  bool isFound =
      sink != nullptr && name->name == this->search.getName() && !this->search.isHidden(name->name);
  if (isFound) {
    this->search.addFound(sink, name->loc());
  }

  return {};
}

std::vector<LookExpr> DestinationLookahead::objectParts(const ObjectLitExpr *literal,
                                                        const Type *object) const {
  auto parts = std::vector<LookExpr>();
  for (const ObjectLitExpr::Property &property : literal->props) {
    const Type *sink = this->propertySink(property, object);
    parts.push_back(this->untyped(property.key()));
    parts.push_back(LookExpr{.expr = property.value, .sink = sink});
  }

  return parts;
}

const Type *DestinationLookahead::propertySink(const ObjectLitExpr::Property &property,
                                               const Type *object) const {
  bool isTypedField = object != nullptr && object->isObject() && !property.spread();
  if (!isTypedField) {
    return nullptr;
  }
  const Field *field = object->field(property.name);
  if (field == nullptr) {
    return nullptr;
  }

  return field->type;
}

std::vector<LookExpr> DestinationLookahead::conditionalParts(const ConditionalExpr *conditional,
                                                             const Type *sink) const {
  return {this->untyped(conditional->cond), LookExpr{.expr = conditional->thenExpr, .sink = sink},
          LookExpr{.expr = conditional->elseExpr, .sink = sink}};
}

std::vector<LookExpr> DestinationLookahead::binaryParts(const BinaryExpr *binary,
                                                        const Type *sink) const {
  bool isNullish = binary->op == BinaryOp::Nullish;
  bool isPassing = isNullish || binary->op == BinaryOp::LogAnd || binary->op == BinaryOp::LogOr;

  return {LookExpr{.expr = binary->lhs, .sink = isNullish ? sink : nullptr},
          LookExpr{.expr = binary->rhs, .sink = isPassing ? sink : nullptr}};
}

std::vector<LookExpr> DestinationLookahead::sequenceParts(const SequenceExpr *sequence,
                                                          const Type *sink) const {
  auto parts = std::vector<LookExpr>();
  for (size_t index = 0; index < sequence->exprs.size(); ++index) {
    bool isLast = index + 1 == sequence->exprs.size();
    parts.push_back(LookExpr{.expr = sequence->exprs[index], .sink = isLast ? sink : nullptr});
  }

  return parts;
}

std::vector<LookExpr> DestinationLookahead::templateParts(const TemplateExpr *templateExpr) const {
  auto parts = std::vector<LookExpr>();
  for (const Expr *part : templateExpr->exprs) {
    parts.push_back(this->untyped(part));
  }

  return parts;
}

std::vector<LookExpr> DestinationLookahead::arrayParts(const ArrayLitExpr *literal,
                                                       const Type *object) const {
  auto parts = std::vector<LookExpr>();
  for (size_t index = 0; index < literal->elements.size(); ++index) {
    const ArrayLitExpr::Element &element = literal->elements[index];
    const Type *sink = this->elementSink(element, index, object);
    parts.push_back(LookExpr{.expr = element.expr, .sink = sink});
  }

  return parts;
}

const Type *DestinationLookahead::elementSink(const ArrayLitExpr::Element &element, size_t index,
                                              const Type *object) const {
  if (object == nullptr) {
    return nullptr;
  }
  if (object->isArray()) {
    return element.spread ? object : object->inner();
  }
  bool isTupleItem = object->isTuple() && !element.spread && index < object->fields().size();
  if (isTupleItem) {
    return object->fields()[index].type;
  }

  return nullptr;
}

std::vector<LookExpr> DestinationLookahead::assignParts(const AssignExpr *assign) {
  auto *target = dyn_cast<NameExpr>(assign->target);
  bool isWanted = target != nullptr && target->name == this->search.getName() &&
                  !this->search.isHidden(target->name);
  if (isWanted) {
    const Type *type = this->plainAssignType(assign, assign->value);
    if (type != nullptr) {
      this->search.addFound(type, assign->loc());
    }

    return {this->untyped(assign->value)};
  }
  this->steps.push_back(LookAssign{.assign = assign});
  if (target == nullptr) {
    return {this->untyped(assign->target)};
  }

  return {};
}

std::vector<LookExpr> DestinationLookahead::callParts(const CallExpr *call) {
  auto params = this->peeks.calleeParams(call, this->search);
  auto parts = std::vector<LookExpr>{this->untyped(call->callee)};
  for (size_t index = 0; index < call->args.size(); ++index) {
    bool isTyped = index < params.size() && !call->spreads(index);
    const Type *sink = isTyped ? params[index] : nullptr;
    parts.push_back(LookExpr{.expr = call->args[index], .sink = sink});
  }

  return parts;
}

std::vector<LookExpr> DestinationLookahead::newParts(const NewExpr *create) {
  const FuncSymbol *constructor = this->peeks.soleConstructor(create, this->search);
  auto parts = std::vector<LookExpr>();
  for (size_t index = 0; index < create->args.size(); ++index) {
    bool isTyped =
        constructor != nullptr && index < constructor->params.size() && !constructor->hasRest;
    const Type *sink = isTyped ? constructor->params[index] : nullptr;
    parts.push_back(LookExpr{.expr = create->args[index], .sink = sink});
  }

  return parts;
}

const Type *DestinationLookahead::annotatedType(const VarDeclarator &declarator) {
  if (!declarator.typeAnn.has_value()) {
    return nullptr;
  }

  return this->knownType(*declarator.typeAnn, TypeUse::STANDALONE);
}

const Type *DestinationLookahead::declaredFunctionType(const FunctionDecl *functionDecl) {
  auto params = std::vector<const Type *>();
  for (const Param &param : functionDecl->params) {
    if (!param.type.has_value() || this->state.isIterableRef(*param.type)) {
      return nullptr;
    }
    const auto *type = this->knownType(*param.type, TypeUse::STANDALONE);
    if (type == nullptr) {
      return nullptr;
    }
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

const Type *DestinationLookahead::knownType(const TypeRef &ref, TypeUse use) {
  for (const auto &[written, type] : this->types.known) {
    if (written == &ref) {
      return type;
    }
  }
  auto missing = std::make_pair(&ref, use);
  if (!llvm::is_contained(this->types.missing, missing)) {
    this->types.missing.push_back(missing);
  }

  return nullptr;
}

}
