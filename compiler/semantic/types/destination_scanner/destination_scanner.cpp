#include "compiler/semantic/types/destination_scanner/destination_scanner.h"
#include "compiler/program/library/array/array_method_table/array_method_table.h"
#include "compiler/program/library/builtin_names/global_member_table/global_member_table.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/program/library/dict/dict_method_table/dict_method_table.h"
#include "compiler/program/library/library_module_names/library_module_names.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/iterators/iterable_checker/iterable_checker.h"
#include "compiler/semantic/bodies/shared_helpers/shared_helpers.h"
#include "compiler/semantic/declare/class_declarer/class_declarer.h"
#include "compiler/semantic/flow/children_first/children_first.h"
#include "compiler/semantic/flow/predicate_checker/predicate_checker.h"
#include "compiler/semantic/names/scopes/scopes.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/destination/destination.h"
#include "compiler/semantic/types/destination_scan/destination_scan.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "compiler/semantic/types/type_resolver/type_resolver.h"
#include "llvm/ADT/STLExtras.h"
#include <deque>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

DestinationScanner::DestinationScanner(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->scopes = &all.scopes;
  this->callChecker = &all.callChecker;
  this->destination = &all.destination;
  this->typeResolver = &all.typeResolver;
  this->typeRelations = &all.typeRelations;
  this->classDeclarer = &all.classDeclarer;
  this->iterableChecker = &all.iterableChecker;
  this->predicateChecker = &all.predicateChecker;
}

bool DestinationScanner::isResultLiteral(const CallExpr *call) const {
  auto *name = dyn_cast<NameExpr>(call->callee);
  if (name == nullptr) {
    return false;
  }
  auto global = GlobalNameTable::find(name->name);
  bool isOkOrErr = global == GlobalName::OK || global == GlobalName::ERR;

  return isOkOrErr && call->args.size() <= 1 && !call->hasSpread() && !call->optional;
}

void DestinationScanner::scanDestinations(const Stmt *stmt, DestinationScan &scan) {
  this->runScan(ScanStmt{.stmt = stmt}, scan);
}

void DestinationScanner::scanDestinations(const Expr *expr, const Type *sink,
                                          DestinationScan &scan) {
  this->runScan(ScanExpr{.expr = expr, .sink = sink}, scan);
}

void DestinationScanner::runScan(ScanStep first, DestinationScan &scan) {
  auto run = ScanRun{.scan = scan, .steps = {std::move(first)}, .caseBodies = {}};
  while (!run.steps.empty()) {
    ScanStep step = std::move(run.steps.back());
    run.steps.pop_back();
    this->applyStep(step, run);
  }
}

void DestinationScanner::applyStep(const ScanStep &step, ScanRun &run) {
  switch (step.kind()) {
    case ScanStepKind::START_LIST:
      this->startList(*step.as<StartList>(), run);
      break;
    case ScanStepKind::LIST_ITEM:
      this->listItem(*step.as<ListItem>(), run);
      break;
    case ScanStepKind::STMT:
      this->scanStmt(step.as<ScanStmt>()->stmt, run);
      break;
    case ScanStepKind::EXPR:
      this->scanExpr(*step.as<ScanExpr>(), run);
      break;
    case ScanStepKind::VAR_DECLARE:
      this->varDeclare(step.as<VarDeclare>()->declarator, run);
      break;
    case ScanStepKind::DECLARE_NAME:
      run.scan.declare(step.as<DeclareName>()->name, step.as<DeclareName>()->type);
      break;
    case ScanStepKind::LEAVE_SCOPE:
      run.scan.leaveScope();
      break;
    case ScanStepKind::FOR_REST:
      this->forRest(step.as<ForRest>()->loop, run);
      break;
    case ScanStepKind::FOR_OF_REST:
      this->forOfRest(step.as<ForOfRest>()->loop, run);
      break;
    case ScanStepKind::FUNCTION:
      this->scanFunction(step.as<ScanFunction>()->functionDecl, run);
      break;
    case ScanStepKind::PARAM:
      this->scanParam(step.as<ScanParam>()->param, run);
      break;
    case ScanStepKind::FUNCTION_END:
      this->functionEnd(step.as<FunctionEnd>()->outerReturns, run);
      break;
    case ScanStepKind::ASSIGN_VALUE:
      this->assignValue(step.as<AssignValue>()->assign, run);
      break;
  }
}

void DestinationScanner::startList(const StartList &list, ScanRun &run) {
  if (this->isDeclaringFunction(list.stmts, run.scan.getName())) {
    return;
  }
  run.scan.enterScope();
  for (const Stmt *stmt : list.stmts) {
    auto *functionDecl = dyn_cast<FunctionDecl>(stmt);
    if (functionDecl != nullptr) {
      const auto *type = this->destination->declaredFunctionType(functionDecl);
      run.scan.declare(functionDecl->name, type);
    }
  }
  run.steps.push_back(ListItem{.stmts = list.stmts, .index = 0});
}

bool DestinationScanner::isDeclaringFunction(llvm::ArrayRef<Stmt *> stmts,
                                             llvm::StringRef name) const {
  for (const Stmt *stmt : stmts) {
    auto *functionDecl = dyn_cast<FunctionDecl>(stmt);
    if (functionDecl != nullptr && functionDecl->name == name) {
      return true;
    }
  }

  return false;
}

void DestinationScanner::listItem(const ListItem &item, ScanRun &run) const {
  bool isAfterDeclaration =
      item.index > 0 && this->isDeclaringName(item.stmts[item.index - 1], run.scan.getName());
  if (item.index == item.stmts.size() || isAfterDeclaration) {
    run.scan.leaveScope();
    return;
  }
  run.steps.push_back(ListItem{.stmts = item.stmts, .index = item.index + 1});
  run.steps.push_back(ScanStmt{.stmt = item.stmts[item.index]});
}

bool DestinationScanner::isDeclaringName(const Stmt *stmt, llvm::StringRef name) const {
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

void DestinationScanner::scanStmt(const Stmt *stmt, ScanRun &run) {
  if (stmt == nullptr) {
    return;
  }
  switch (stmt->kind()) {
    case Stmt::Kind::Block:
      return run.steps.push_back(StartList{.stmts = cast<BlockStmt>(stmt)->body});
    case Stmt::Kind::VarDecl:
      return this->pushDeclarators(cast<VarDeclStmt>(stmt), run);
    case Stmt::Kind::Function:
      return run.steps.push_back(ScanFunction{.functionDecl = cast<FunctionDecl>(stmt)});
    case Stmt::Kind::Expr:
      return run.steps.push_back(ScanExpr{.expr = cast<ExprStmt>(stmt)->expr, .sink = nullptr});
    case Stmt::Kind::If:
      return this->pushIfParts(cast<IfStmt>(stmt), run);
    case Stmt::Kind::While:
      return this->pushWhileParts(cast<WhileStmt>(stmt), run);
    case Stmt::Kind::DoWhile:
      return this->pushDoWhileParts(cast<DoWhileStmt>(stmt), run);
    case Stmt::Kind::For:
      return this->pushForParts(cast<ForStmt>(stmt), run);
    case Stmt::Kind::ForOf:
      return this->pushForOfParts(cast<ForOfStmt>(stmt), run);
    case Stmt::Kind::Switch:
      return this->scanSwitch(cast<SwitchStmt>(stmt), run);
    case Stmt::Kind::Labeled:
      return run.steps.push_back(ScanStmt{.stmt = cast<LabeledStmt>(stmt)->body});
    case Stmt::Kind::Return:
      return this->pushReturnValue(cast<ReturnStmt>(stmt), run);
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

void DestinationScanner::pushDeclarators(const VarDeclStmt *varDecl, ScanRun &run) const {
  for (const VarDeclarator &declarator : llvm::reverse(varDecl->decls)) {
    run.steps.push_back(VarDeclare{.declarator = &declarator});
  }
}

void DestinationScanner::pushIfParts(const IfStmt *ifStmt, ScanRun &run) const {
  run.steps.push_back(ScanStmt{.stmt = ifStmt->elseStmt});
  run.steps.push_back(ScanStmt{.stmt = ifStmt->thenStmt});
  run.steps.push_back(ScanExpr{.expr = ifStmt->cond, .sink = nullptr});
}

void DestinationScanner::pushWhileParts(const WhileStmt *loop, ScanRun &run) const {
  run.steps.push_back(ScanStmt{.stmt = loop->body});
  run.steps.push_back(ScanExpr{.expr = loop->cond, .sink = nullptr});
}

void DestinationScanner::pushDoWhileParts(const DoWhileStmt *loop, ScanRun &run) const {
  run.steps.push_back(ScanExpr{.expr = loop->cond, .sink = nullptr});
  run.steps.push_back(ScanStmt{.stmt = loop->body});
}

void DestinationScanner::pushForParts(const ForStmt *loop, ScanRun &run) const {
  run.scan.enterScope();
  run.steps.push_back(ForRest{.loop = loop});
  run.steps.push_back(ScanStmt{.stmt = loop->init});
}

void DestinationScanner::pushForOfParts(const ForOfStmt *loop, ScanRun &run) const {
  run.steps.push_back(ForOfRest{.loop = loop});
  run.steps.push_back(ScanExpr{.expr = loop->iterable, .sink = nullptr});
}

void DestinationScanner::pushReturnValue(const ReturnStmt *returnStmt, ScanRun &run) const {
  if (returnStmt->value == nullptr) {
    return;
  }
  const Type *returns = run.scan.getReturns();
  bool isTyped = returns != nullptr && !returns->isVoid();
  const Type *sink = isTyped ? returns : nullptr;

  return run.steps.push_back(ScanExpr{.expr = returnStmt->value, .sink = sink});
}

void DestinationScanner::scanSwitch(const SwitchStmt *switchStmt, ScanRun &run) const {
  std::vector<Stmt *> &bodies = run.caseBodies.emplace_back();
  for (const SwitchCase &switchCase : switchStmt->cases) {
    bodies.insert(bodies.end(), switchCase.body.begin(), switchCase.body.end());
  }
  run.steps.push_back(StartList{.stmts = bodies});
  for (const SwitchCase &switchCase : llvm::reverse(switchStmt->cases)) {
    run.steps.push_back(ScanExpr{.expr = switchCase.test, .sink = nullptr});
  }
  run.steps.push_back(ScanExpr{.expr = switchStmt->discriminant, .sink = nullptr});
}

void DestinationScanner::varDeclare(const VarDeclarator *declarator, ScanRun &run) {
  const Type *type = this->destination->annotatedType(*declarator);
  run.steps.push_back(DeclareName{.name = declarator->name, .type = type});
  if (declarator->init != nullptr) {
    run.steps.push_back(ScanExpr{.expr = declarator->init, .sink = type});
  }
}

void DestinationScanner::forRest(const ForStmt *loop, ScanRun &run) const {
  run.steps.push_back(LeaveScope{});
  if (!run.scan.isDeclaredInnermost(run.scan.getName())) {
    run.steps.push_back(ScanStmt{.stmt = loop->body});
    run.steps.push_back(ScanExpr{.expr = loop->step, .sink = nullptr});
    run.steps.push_back(ScanExpr{.expr = loop->cond, .sink = nullptr});
  }
}

void DestinationScanner::forOfRest(const ForOfStmt *loop, ScanRun &run) const {
  if (loop->binding.name != run.scan.getName()) {
    run.scan.enterScope();
    run.scan.declare(loop->binding.name, nullptr);
    run.steps.push_back(LeaveScope{});
    run.steps.push_back(ScanStmt{.stmt = loop->body});
  }
}

void DestinationScanner::scanFunction(const FunctionDecl *functionDecl, ScanRun &run) {
  bool isHidden = this->hasParamNamed(functionDecl, run.scan.getName());
  if (functionDecl->body == nullptr || isHidden) {
    return;
  }
  const Type *outer = run.scan.getReturns();
  const Type *returned = this->writtenReturnType(functionDecl);
  run.scan.setReturns(returned);
  run.scan.enterScope();
  run.steps.push_back(FunctionEnd{.outerReturns = outer});
  run.steps.push_back(StartList{.stmts = functionDecl->body->body});
  for (const Param &param : llvm::reverse(functionDecl->params)) {
    run.steps.push_back(ScanParam{.param = &param});
  }
}

bool DestinationScanner::hasParamNamed(const FunctionDecl *functionDecl,
                                       llvm::StringRef name) const {
  for (const Param &param : functionDecl->params) {
    if (param.name == name) {
      return true;
    }
  }

  return false;
}

const Type *DestinationScanner::writtenReturnType(const FunctionDecl *functionDecl) {
  if (!functionDecl->returnType.has_value()) {
    return nullptr;
  }
  auto *checked = const_cast<FunctionDecl *>(functionDecl);

  return this->predicateChecker->resolveReturnType(checked, nullptr);
}

void DestinationScanner::scanParam(const Param *param, ScanRun &run) {
  const Type *type = this->writtenParamType(*param);
  bool isOptional = type != nullptr && param->optional();
  const Type *declared = isOptional ? Type::optional(type) : type;
  run.steps.push_back(DeclareName{.name = param->name, .type = declared});
  if (param->defaultValue() != nullptr) {
    run.steps.push_back(ScanExpr{.expr = param->defaultValue(), .sink = type});
  }
}

const Type *DestinationScanner::writtenParamType(const Param &param) {
  if (!param.type.has_value() || this->iterableChecker->isIterableRef(*param.type)) {
    return nullptr;
  }

  return this->typeResolver->resolveType(*param.type);
}

void DestinationScanner::functionEnd(const Type *outerReturns, ScanRun &run) const {
  run.scan.leaveScope();
  run.scan.setReturns(outerReturns);
}

void DestinationScanner::assignValue(const AssignExpr *assign, ScanRun &run) {
  const Type *sink = this->plainAssignType(assign, assign->target, run.scan);
  run.steps.push_back(ScanExpr{.expr = assign->value, .sink = sink});
}

const Type *DestinationScanner::plainAssignType(const AssignExpr *assign, const Expr *side,
                                                const DestinationScan &scan) const {
  if (assign->op.has_value()) {
    return nullptr;
  }

  return this->peekType(side, scan);
}

void DestinationScanner::scanExpr(const ScanExpr &scanned, ScanRun &run) {
  const Expr *expr = scanned.expr;
  if (expr == nullptr) {
    return;
  }
  bool isTyped = scanned.sink != nullptr && !scanned.sink->isError();
  const Type *sink = isTyped ? scanned.sink : nullptr;
  auto parts = this->exprParts(expr, sink, run);
  for (const ScanExpr &part : llvm::reverse(parts)) {
    run.steps.push_back(part);
  }
}

std::vector<ScanExpr> DestinationScanner::exprParts(const Expr *expr, const Type *sink,
                                                    ScanRun &run) {
  const Type *object = sink != nullptr ? sink->nonOptional() : nullptr;
  switch (expr->kind()) {
    case Expr::Kind::Name:
      return this->nameParts(cast<NameExpr>(expr), sink, run);
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
      return this->assignParts(cast<AssignExpr>(expr), run);
    case Expr::Kind::Call:
      return this->callParts(cast<CallExpr>(expr), run);
    case Expr::Kind::Function:
      run.steps.push_back(ScanFunction{.functionDecl = cast<FunctionExpr>(expr)->fn});
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
      return this->newParts(cast<NewExpr>(expr), run);
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

ScanExpr DestinationScanner::untyped(const Expr *expr) const {
  return ScanExpr{.expr = expr, .sink = nullptr};
}

std::vector<ScanExpr> DestinationScanner::nameParts(const NameExpr *name, const Type *sink,
                                                    ScanRun &run) const {
  bool isFound =
      sink != nullptr && name->name == run.scan.getName() && !run.scan.isHidden(name->name);
  if (isFound) {
    run.scan.addFound(sink, name->loc());
  }

  return {};
}

std::vector<ScanExpr> DestinationScanner::objectParts(const ObjectLitExpr *literal,
                                                      const Type *object) const {
  auto parts = std::vector<ScanExpr>();
  for (const ObjectLitExpr::Property &property : literal->props) {
    const Type *sink = this->propertySink(property, object);
    parts.push_back(this->untyped(property.key()));
    parts.push_back(ScanExpr{.expr = property.value, .sink = sink});
  }

  return parts;
}

const Type *DestinationScanner::propertySink(const ObjectLitExpr::Property &property,
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

std::vector<ScanExpr> DestinationScanner::conditionalParts(const ConditionalExpr *conditional,
                                                           const Type *sink) const {
  return {this->untyped(conditional->cond), ScanExpr{.expr = conditional->thenExpr, .sink = sink},
          ScanExpr{.expr = conditional->elseExpr, .sink = sink}};
}

std::vector<ScanExpr> DestinationScanner::binaryParts(const BinaryExpr *binary,
                                                      const Type *sink) const {
  bool isNullish = binary->op == BinaryOp::Nullish;
  bool isPassing = isNullish || binary->op == BinaryOp::LogAnd || binary->op == BinaryOp::LogOr;

  return {ScanExpr{.expr = binary->lhs, .sink = isNullish ? sink : nullptr},
          ScanExpr{.expr = binary->rhs, .sink = isPassing ? sink : nullptr}};
}

std::vector<ScanExpr> DestinationScanner::sequenceParts(const SequenceExpr *sequence,
                                                        const Type *sink) const {
  auto parts = std::vector<ScanExpr>();
  for (size_t index = 0; index < sequence->exprs.size(); ++index) {
    bool isLast = index + 1 == sequence->exprs.size();
    parts.push_back(ScanExpr{.expr = sequence->exprs[index], .sink = isLast ? sink : nullptr});
  }

  return parts;
}

std::vector<ScanExpr> DestinationScanner::templateParts(const TemplateExpr *templateExpr) const {
  auto parts = std::vector<ScanExpr>();
  for (const Expr *part : templateExpr->exprs) {
    parts.push_back(this->untyped(part));
  }

  return parts;
}

std::vector<ScanExpr> DestinationScanner::arrayParts(const ArrayLitExpr *literal,
                                                     const Type *object) const {
  auto parts = std::vector<ScanExpr>();
  for (size_t index = 0; index < literal->elements.size(); ++index) {
    const ArrayLitExpr::Element &element = literal->elements[index];
    const Type *sink = this->elementSink(element, index, object);
    parts.push_back(ScanExpr{.expr = element.expr, .sink = sink});
  }

  return parts;
}

const Type *DestinationScanner::elementSink(const ArrayLitExpr::Element &element, size_t index,
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

std::vector<ScanExpr> DestinationScanner::assignParts(const AssignExpr *assign, ScanRun &run) {
  auto *target = dyn_cast<NameExpr>(assign->target);
  bool isWanted =
      target != nullptr && target->name == run.scan.getName() && !run.scan.isHidden(target->name);
  if (isWanted) {
    const Type *type = this->plainAssignType(assign, assign->value, run.scan);
    if (type != nullptr) {
      run.scan.addFound(type, assign->loc());
    }

    return {this->untyped(assign->value)};
  }
  run.steps.push_back(AssignValue{.assign = assign});
  if (target == nullptr) {
    return {this->untyped(assign->target)};
  }

  return {};
}

std::vector<ScanExpr> DestinationScanner::callParts(const CallExpr *call, ScanRun &run) {
  auto params = this->calleeParams(call, run.scan);
  auto parts = std::vector<ScanExpr>{this->untyped(call->callee)};
  for (size_t index = 0; index < call->args.size(); ++index) {
    bool isTyped = index < params.size() && !call->spreads(index);
    const Type *sink = isTyped ? params[index] : nullptr;
    parts.push_back(ScanExpr{.expr = call->args[index], .sink = sink});
  }

  return parts;
}

std::vector<ScanExpr> DestinationScanner::newParts(const NewExpr *create, ScanRun &run) {
  const FuncSymbol *constructor = this->soleConstructor(create, run.scan);
  auto parts = std::vector<ScanExpr>();
  for (size_t index = 0; index < create->args.size(); ++index) {
    bool isTyped =
        constructor != nullptr && index < constructor->params.size() && !constructor->hasRest;
    const Type *sink = isTyped ? constructor->params[index] : nullptr;
    parts.push_back(ScanExpr{.expr = create->args[index], .sink = sink});
  }

  return parts;
}

const FuncSymbol *DestinationScanner::soleConstructor(const NewExpr *create,
                                                      const DestinationScan &scan) const {
  if (scan.isHidden(create->name)) {
    return nullptr;
  }
  auto entry = this->scopes->lookup(create->name);
  ClassDecl *classDecl = entry.cls();
  if (classDecl == nullptr || classDecl->constructors.size() != 1) {
    return nullptr;
  }

  return classDecl->constructors[0]->sym;
}

const Type *DestinationScanner::peekName(llvm::StringRef name, const DestinationScan &scan) const {
  auto local = scan.findLocal(name);
  if (local.has_value()) {
    return *local;
  }
  auto entry = this->scopes->lookup(name);
  VarSymbol *variable = entry.var();
  if (variable == nullptr) {
    return nullptr;
  }

  return variable->type;
}

const Type *DestinationScanner::peekType(const Expr *expr, const DestinationScan &scan) const {
  auto answers = std::vector<const Type *>();
  for (const Expr *node : ChildrenFirst::order(expr, this, &DestinationScanner::peekParts)) {
    const auto *type = this->nodePeekType(node, scan, answers);
    answers.push_back(type);
  }

  return answers.back();
}

NodeChildren<const Expr *> DestinationScanner::peekParts(const Expr *expr) const {
  auto *member = dyn_cast<MemberExpr>(expr);
  auto *index = dyn_cast<IndexExpr>(expr);
  auto *conditional = dyn_cast<ConditionalExpr>(expr);
  auto *call = dyn_cast<CallExpr>(expr);

  if (member != nullptr) {
    return {member->object};
  }
  if (index != nullptr) {
    return {index->object};
  }
  if (conditional != nullptr && this->destination->isLiteralShaped(conditional)) {
    return {conditional->thenExpr, conditional->elseExpr};
  }
  if (call != nullptr && this->isResultLiteral(call) && !call->args.empty()) {
    return {call->args[0]};
  }

  return {};
}

const Type *DestinationScanner::nodePeekType(const Expr *expr, const DestinationScan &scan,
                                             std::vector<const Type *> &answers) const {
  auto *name = dyn_cast<NameExpr>(expr);
  auto *member = dyn_cast<MemberExpr>(expr);
  auto *index = dyn_cast<IndexExpr>(expr);
  auto *conditional = dyn_cast<ConditionalExpr>(expr);
  auto *call = dyn_cast<CallExpr>(expr);

  if (name != nullptr) {
    return this->namePeekType(name, scan);
  }
  if (member != nullptr) {
    const Type *object = ChildrenFirst::takeAnswer(answers);

    return this->memberPeekType(member, object);
  }
  if (index != nullptr) {
    const Type *object = ChildrenFirst::takeAnswer(answers);

    return this->elementPeekType(object);
  }
  if (conditional != nullptr && this->destination->isLiteralShaped(conditional)) {
    return this->choicePeekType(answers);
  }
  if (call != nullptr && this->isResultLiteral(call)) {
    return this->resultPeekType(call, answers);
  }

  return nullptr;
}

const Type *DestinationScanner::namePeekType(const NameExpr *name,
                                             const DestinationScan &scan) const {
  if (name->name == scan.getName()) {
    return nullptr;
  }

  return this->peekName(name->name, scan);
}

const Type *DestinationScanner::elementPeekType(const Type *object) const {
  if (object == nullptr) {
    return nullptr;
  }
  if (object->isRecord()) {
    return object->valueType();
  }
  if (object->isArray()) {
    return object->inner();
  }

  return nullptr;
}

const Type *DestinationScanner::choicePeekType(std::vector<const Type *> &answers) const {
  const Type *second = ChildrenFirst::takeAnswer(answers);
  const Type *first = ChildrenFirst::takeAnswer(answers);
  if (first == nullptr || second == nullptr) {
    return nullptr;
  }
  const Type *common = this->typeRelations->unify(first, second);
  if (common == nullptr || common->isError()) {
    return nullptr;
  }

  return common;
}

const Type *DestinationScanner::memberPeekType(const MemberExpr *member, const Type *object) const {
  if (this->callChecker->isGlobalMember(member, GlobalMember::CRYPTO_SUBTLE)) {
    return this->peekSubtle();
  }
  if (object == nullptr) {
    return nullptr;
  }
  const Type *plain = object->nonOptional();
  if (plain->isRecord()) {
    return plain->valueType();
  }
  if (!plain->isObject()) {
    return nullptr;
  }
  const Field *field = plain->field(member->name);
  if (field == nullptr) {
    return nullptr;
  }

  return field->type;
}

const Type *DestinationScanner::peekSubtle() const {
  auto entry = this->scopes->lookup(LibraryModuleNames::kSubtleName);
  VarSymbol *subtle = entry.var();
  if (subtle == nullptr) {
    return nullptr;
  }

  return subtle->type;
}

const Type *DestinationScanner::resultPeekType(const CallExpr *call,
                                               std::vector<const Type *> &answers) const {
  const Type *half = this->resultHalf(call, answers);
  if (half == nullptr) {
    return nullptr;
  }
  auto made = GlobalNameTable::find(cast<NameExpr>(call->callee)->name);
  if (made == GlobalName::OK) {
    return Type::result(half, Type::never());
  }

  return Type::result(Type::never(), half);
}

const Type *DestinationScanner::resultHalf(const CallExpr *call,
                                           std::vector<const Type *> &answers) const {
  if (call->args.empty()) {
    return Type::voidTy();
  }
  const Type *peeked = ChildrenFirst::takeAnswer(answers);
  if (peeked != nullptr) {
    return peeked;
  }

  return SharedHelpers::literalType(call->args[0]);
}

std::vector<const Type *> DestinationScanner::calleeParams(const CallExpr *call,
                                                           const DestinationScan &scan) {
  auto *name = dyn_cast<NameExpr>(call->callee);
  auto *member = dyn_cast<MemberExpr>(call->callee);

  if (name != nullptr) {
    return this->nameCalleeParams(call, name, scan);
  }
  if (member != nullptr) {
    return this->memberCalleeParams(call, member, scan);
  }

  return {};
}

std::vector<const Type *> DestinationScanner::spreadParams(llvm::ArrayRef<const Type *> params,
                                                           bool hasRest, size_t argCount) const {
  auto types = std::vector<const Type *>(params.begin(), params.end());
  if (!hasRest || types.empty()) {
    return types;
  }
  const Type *rest = types.back();
  types.pop_back();
  bool isArrayRest = rest != nullptr && rest->isArray();
  const Type *element = isArrayRest ? rest->inner() : nullptr;
  while (types.size() < argCount) {
    types.push_back(element);
  }

  return types;
}

std::vector<const Type *> DestinationScanner::nameCalleeParams(const CallExpr *call,
                                                               const NameExpr *name,
                                                               const DestinationScan &scan) {
  size_t argCount = call->args.size();
  if (scan.isHidden(name->name)) {
    const auto *local = this->peekName(name->name, scan);

    return this->functionTypeParams(local, argCount);
  }
  auto entry = this->scopes->lookup(name->name);
  VarSymbol *variable = entry.var();
  const FuncSymbol *function = this->calledFunction(entry);
  if (function != nullptr) {
    return this->spreadParams(function->params, function->hasRest, argCount);
  }
  if (variable == nullptr) {
    return {};
  }

  return this->functionTypeParams(variable->type, argCount);
}

const FuncSymbol *DestinationScanner::calledFunction(const ScopeEntry &entry) const {
  VarSymbol *variable = entry.var();
  if (variable != nullptr && variable->function() != nullptr) {
    return variable->function()->sym;
  }

  return entry.func();
}

std::vector<const Type *> DestinationScanner::functionTypeParams(const Type *type,
                                                                 size_t argCount) const {
  if (type == nullptr || !type->isFunction()) {
    return {};
  }

  return this->spreadParams(type->params(), type->hasRest(), argCount);
}

std::vector<const Type *> DestinationScanner::memberCalleeParams(const CallExpr *call,
                                                                 const MemberExpr *member,
                                                                 const DestinationScan &scan) {
  size_t argCount = call->args.size();
  const ClassDecl *owner = this->namedClass(member->object, scan);
  if (owner != nullptr) {
    auto methods = this->classDeclarer->staticMethodsNamed(owner, member->name);

    return this->soleMethodParams(methods, argCount);
  }
  const auto *peeked = this->peekType(member->object, scan);
  if (peeked == nullptr) {
    return {};
  }
  const Type *object = peeked->nonOptional();
  auto *classDecl = this->classDeclarer->classOf(object);
  if (classDecl != nullptr) {
    auto methods = this->classDeclarer->methodsNamed(classDecl, member->name);

    return this->soleMethodParams(methods, argCount);
  }
  if (object->isArray()) {
    return this->arrayMethodParams(object, member->name, argCount);
  }
  if (this->hasFunctionField(object, member->name)) {
    return this->functionTypeParams(object->field(member->name)->type, argCount);
  }

  return this->dictMethodParams(object, member->name);
}

const ClassDecl *DestinationScanner::namedClass(const Expr *object,
                                                const DestinationScan &scan) const {
  auto *owner = dyn_cast<NameExpr>(object);
  if (owner == nullptr || scan.isHidden(owner->name)) {
    return nullptr;
  }
  auto entry = this->scopes->lookup(owner->name);

  return entry.cls();
}

bool DestinationScanner::hasFunctionField(const Type *object, llvm::StringRef name) const {
  if (!object->isObject()) {
    return false;
  }
  const Field *field = object->field(name);

  return field != nullptr && field->type->isFunction();
}

std::vector<const Type *>
DestinationScanner::soleMethodParams(llvm::ArrayRef<FunctionDecl *> methods,
                                     size_t argCount) const {
  if (methods.size() != 1 || methods[0]->sym == nullptr) {
    return {};
  }
  const FuncSymbol *method = methods[0]->sym;

  return this->spreadParams(method->params, method->hasRest, argCount);
}

std::vector<const Type *> DestinationScanner::arrayMethodParams(const Type *array,
                                                                llvm::StringRef name,
                                                                size_t argCount) const {
  const auto *method = ArrayMethodTable::find(name);
  if (method == nullptr) {
    return {};
  }
  ArrayMethod which = method->method;
  bool isAdding = which == ArrayMethod::PUSH || which == ArrayMethod::UNSHIFT;
  bool isSearching = which == ArrayMethod::INCLUDES || which == ArrayMethod::INDEX_OF ||
                     which == ArrayMethod::LAST_INDEX_OF;
  if (isAdding || isSearching) {
    return std::vector<const Type *>(argCount, array->inner());
  }
  if (which == ArrayMethod::FILL) {
    return {array->inner()};
  }

  return {};
}

std::vector<const Type *> DestinationScanner::dictMethodParams(const Type *object,
                                                               llvm::StringRef name) const {
  const auto *dictMethod = DictMethodTable::find(name);
  if (dictMethod == nullptr) {
    return {};
  }
  DictMethod method = dictMethod->method;
  if (object->isMap() && method == DictMethod::SET) {
    return {object->keyType(), object->valueType()};
  }
  bool isTakingKey = method == DictMethod::GET || method == DictMethod::HAS ||
                     method == DictMethod::DELETE_KEY || method == DictMethod::ADD;
  if (object->isDict() && !object->isRecord() && isTakingKey) {
    return {object->keyType()};
  }

  return {};
}

}
