#include "compiler/semantic/types/destination_scanner/destination_scanner.h"
#include "compiler/program/library/array/array_method_table/array_method_table.h"
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
  this->iterableChecker = &all.iterableChecker;
  this->predicateChecker = &all.predicateChecker;
}

bool DestinationScanner::isResultLiteral(const CallExpr *call) {
  auto *name = dyn_cast<NameExpr>(call->callee);
  if (name == nullptr) {
    return false;
  }
  std::optional<GlobalName> global = GlobalNameTable::find(name->name);
  bool isOkOrErr = global == GlobalName::OK || global == GlobalName::ERR;

  return isOkOrErr && call->args.size() <= 1 && !call->hasSpread() && !call->optional;
}

bool DestinationScanner::scanDestinations(llvm::ArrayRef<Stmt *> stmts, DestinationScan &scan) {
  this->runScan(StartList{stmts}, scan);
  bool isHidingName = llvm::any_of(stmts, [&](const Stmt *stmt) {
    auto *fn = dyn_cast<FunctionDecl>(stmt);
    return (fn != nullptr && fn->name == scan.getName()) ||
           DestinationScanner::declaresName(stmt, scan.getName());
  });

  return !isHidingName;
}

void DestinationScanner::scanFunctionDestinations(const FunctionDecl *fn, DestinationScan &scan) {
  this->runScan(ScanFunction{fn}, scan);
}

void DestinationScanner::scanDestinations(const Stmt *stmt, DestinationScan &scan) {
  this->runScan(ScanStmt{stmt}, scan);
}

void DestinationScanner::scanDestinations(const Expr *expr, const Type *sink,
                                          DestinationScan &scan) {
  this->runScan(ScanExpr{expr, sink}, scan);
}

void DestinationScanner::runScan(ScanStep first, DestinationScan &scan) {
  ScanRun run{scan, {std::move(first)}, {}};
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
      DestinationScanner::listItem(*step.as<ListItem>(), run);
      break;
    case ScanStepKind::STMT:
      this->scanStmt(step.as<ScanStmt>()->stmt, run);
      break;
    case ScanStepKind::EXPR:
      this->scanExpr(*step.as<ScanExpr>(), run);
      break;
    case ScanStepKind::VAR_DECLARE:
      this->varDeclare(step.as<VarDeclare>()->d, run);
      break;
    case ScanStepKind::DECLARE_NAME:
      run.scan.declare(step.as<DeclareName>()->name, step.as<DeclareName>()->type);
      break;
    case ScanStepKind::LEAVE_SCOPE:
      run.scan.leaveScope();
      break;
    case ScanStepKind::FOR_REST:
      DestinationScanner::forRest(step.as<ForRest>()->loop, run);
      break;
    case ScanStepKind::FOR_OF_REST:
      DestinationScanner::forOfRest(step.as<ForOfRest>()->loop, run);
      break;
    case ScanStepKind::FUNCTION:
      this->scanFunction(step.as<ScanFunction>()->fn, run);
      break;
    case ScanStepKind::PARAM:
      this->scanParam(step.as<ScanParam>()->param, run);
      break;
    case ScanStepKind::FUNCTION_END:
      DestinationScanner::functionEnd(step.as<FunctionEnd>()->outerReturns, run);
      break;
    case ScanStepKind::ASSIGN_VALUE:
      this->assignValue(step.as<AssignValue>()->assign, run);
      break;
  }
}

void DestinationScanner::startList(const StartList &list, ScanRun &run) {
  for (const Stmt *stmt : list.stmts) {
    if (auto *fn = dyn_cast<FunctionDecl>(stmt); fn && fn->name == run.scan.getName()) {
      return;
    }
  }
  run.scan.enterScope();
  for (const Stmt *stmt : list.stmts) {
    if (auto *fn = dyn_cast<FunctionDecl>(stmt)) {
      run.scan.declare(fn->name, this->destination->declaredFunctionType(fn));
    }
  }
  run.steps.push_back(ListItem{list.stmts, 0});
}

void DestinationScanner::listItem(const ListItem &item, ScanRun &run) {
  bool isAfterDeclaration = item.index > 0 && DestinationScanner::declaresName(
                                                  item.stmts[item.index - 1], run.scan.getName());
  if (item.index == item.stmts.size() || isAfterDeclaration) {
    run.scan.leaveScope();
  } else {
    run.steps.push_back(ListItem{item.stmts, item.index + 1});
    run.steps.push_back(ScanStmt{item.stmts[item.index]});
  }
}

bool DestinationScanner::declaresName(const Stmt *stmt, llvm::StringRef name) {
  auto *decl = dyn_cast<VarDeclStmt>(stmt);

  return decl != nullptr &&
         llvm::any_of(decl->decls, [&](const VarDeclarator &v) { return v.name == name; });
}

void DestinationScanner::scanStmt(const Stmt *stmt, ScanRun &run) {
  if (stmt == nullptr) {
    return;
  }
  switch (stmt->kind()) {
    case Stmt::Kind::Block:
      return run.steps.push_back(StartList{cast<BlockStmt>(stmt)->body});
    case Stmt::Kind::VarDecl:
      return DestinationScanner::pushDeclarators(cast<VarDeclStmt>(stmt), run);
    case Stmt::Kind::Function:
      return run.steps.push_back(ScanFunction{cast<FunctionDecl>(stmt)});
    case Stmt::Kind::Expr:
      return run.steps.push_back(ScanExpr{cast<ExprStmt>(stmt)->expr, nullptr});
    case Stmt::Kind::If:
      return DestinationScanner::pushIfParts(cast<IfStmt>(stmt), run);
    case Stmt::Kind::While:
      return DestinationScanner::pushWhileParts(cast<WhileStmt>(stmt), run);
    case Stmt::Kind::DoWhile:
      return DestinationScanner::pushDoWhileParts(cast<DoWhileStmt>(stmt), run);
    case Stmt::Kind::For:
      return DestinationScanner::pushForParts(cast<ForStmt>(stmt), run);
    case Stmt::Kind::ForOf:
      return DestinationScanner::pushForOfParts(cast<ForOfStmt>(stmt), run);
    case Stmt::Kind::Switch:
      return DestinationScanner::scanSwitch(cast<SwitchStmt>(stmt), run);
    case Stmt::Kind::Labeled:
      return run.steps.push_back(ScanStmt{cast<LabeledStmt>(stmt)->body});
    case Stmt::Kind::Return:
      return DestinationScanner::pushReturnValue(cast<ReturnStmt>(stmt), run);
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

void DestinationScanner::pushDeclarators(const VarDeclStmt *decl, ScanRun &run) {
  for (const VarDeclarator &d : llvm::reverse(decl->decls)) {
    run.steps.push_back(VarDeclare{&d});
  }
}

void DestinationScanner::pushIfParts(const IfStmt *s, ScanRun &run) {
  run.steps.push_back(ScanStmt{s->elseStmt});
  run.steps.push_back(ScanStmt{s->thenStmt});
  run.steps.push_back(ScanExpr{s->cond, nullptr});
}

void DestinationScanner::pushWhileParts(const WhileStmt *s, ScanRun &run) {
  run.steps.push_back(ScanStmt{s->body});
  run.steps.push_back(ScanExpr{s->cond, nullptr});
}

void DestinationScanner::pushDoWhileParts(const DoWhileStmt *s, ScanRun &run) {
  run.steps.push_back(ScanExpr{s->cond, nullptr});
  run.steps.push_back(ScanStmt{s->body});
}

void DestinationScanner::pushForParts(const ForStmt *s, ScanRun &run) {
  run.scan.enterScope();
  run.steps.push_back(ForRest{s});
  run.steps.push_back(ScanStmt{s->init});
}

void DestinationScanner::pushForOfParts(const ForOfStmt *s, ScanRun &run) {
  run.steps.push_back(ForOfRest{s});
  run.steps.push_back(ScanExpr{s->iterable, nullptr});
}

void DestinationScanner::pushReturnValue(const ReturnStmt *s, ScanRun &run) {
  if (s->value == nullptr) {
    return;
  }
  const Type *returns = run.scan.getReturns();
  const Type *sink = returns != nullptr && !returns->isVoid() ? returns : nullptr;

  return run.steps.push_back(ScanExpr{s->value, sink});
}

void DestinationScanner::scanSwitch(const SwitchStmt *s, ScanRun &run) {
  std::vector<Stmt *> &all = run.caseBodies.emplace_back();
  for (const SwitchCase &c : s->cases) {
    all.insert(all.end(), c.body.begin(), c.body.end());
  }
  run.steps.push_back(StartList{all});
  for (const SwitchCase &c : llvm::reverse(s->cases)) {
    run.steps.push_back(ScanExpr{c.test, nullptr});
  }
  run.steps.push_back(ScanExpr{s->discriminant, nullptr});
}

void DestinationScanner::varDeclare(const VarDeclarator *d, ScanRun &run) {
  const Type *type = d->typeAnn ? this->typeResolver->resolveType(*d->typeAnn, false) : nullptr;
  run.steps.push_back(DeclareName{d->name, type});
  if (d->init) {
    run.steps.push_back(ScanExpr{d->init, type});
  }
}

void DestinationScanner::forRest(const ForStmt *loop, ScanRun &run) {
  run.steps.push_back(LeaveScope{});
  if (!run.scan.isDeclaredInnermost(run.scan.getName())) {
    run.steps.push_back(ScanStmt{loop->body});
    run.steps.push_back(ScanExpr{loop->step, nullptr});
    run.steps.push_back(ScanExpr{loop->cond, nullptr});
  }
}

void DestinationScanner::forOfRest(const ForOfStmt *loop, ScanRun &run) {
  if (loop->binding.name != run.scan.getName()) {
    run.scan.enterScope();
    run.scan.declare(loop->binding.name, nullptr);
    run.steps.push_back(LeaveScope{});
    run.steps.push_back(ScanStmt{loop->body});
  }
}

void DestinationScanner::scanFunction(const FunctionDecl *fn, ScanRun &run) {
  bool isHidden =
      llvm::any_of(fn->params, [&](const Param &p) { return p.name == run.scan.getName(); });
  if (!fn->body || isHidden) {
    return;
  }
  const Type *outer = run.scan.getReturns();
  const Type *returned =
      fn->returnType
          ? this->predicateChecker->resolveReturnType(const_cast<FunctionDecl *>(fn), nullptr)
          : nullptr;
  run.scan.setReturns(returned);
  run.scan.enterScope();
  run.steps.push_back(FunctionEnd{outer});
  run.steps.push_back(StartList{fn->body->body});
  for (const Param &param : llvm::reverse(fn->params)) {
    run.steps.push_back(ScanParam{&param});
  }
}

void DestinationScanner::scanParam(const Param *param, ScanRun &run) {
  const Type *type = param->type && !this->iterableChecker->isIterableRef(*param->type)
                         ? this->typeResolver->resolveType(*param->type, false)
                         : nullptr;
  run.steps.push_back(
      DeclareName{param->name, type && param->optional() ? Type::optional(type) : type});
  if (param->defaultValue()) {
    run.steps.push_back(ScanExpr{param->defaultValue(), type});
  }
}

void DestinationScanner::functionEnd(const Type *outerReturns, ScanRun &run) {
  run.scan.leaveScope();
  run.scan.setReturns(outerReturns);
}

void DestinationScanner::assignValue(const AssignExpr *assign, ScanRun &run) {
  const Type *sink = assign->op ? nullptr : this->peekType(assign->target, run.scan);
  run.steps.push_back(ScanExpr{assign->value, sink});
}

void DestinationScanner::scanExpr(const ScanExpr &scanned, ScanRun &run) {
  const Expr *expr = scanned.expr;
  if (!expr) {
    return;
  }
  const Type *sink = scanned.sink && !scanned.sink->isError() ? scanned.sink : nullptr;
  std::vector<ScanExpr> parts = this->exprParts(expr, sink, run);
  for (const ScanExpr &part : llvm::reverse(parts)) {
    run.steps.push_back(part);
  }
}

std::vector<ScanExpr> DestinationScanner::exprParts(const Expr *expr, const Type *sink,
                                                    ScanRun &run) {
  const Type *object = sink != nullptr ? sink->nonOptional() : nullptr;
  switch (expr->kind()) {
    case Expr::Kind::Name:
      return DestinationScanner::nameParts(cast<NameExpr>(expr), sink, run);
    case Expr::Kind::ObjectLit:
      return DestinationScanner::objectParts(cast<ObjectLitExpr>(expr), object);
    case Expr::Kind::ArrayLit:
      return DestinationScanner::arrayParts(cast<ArrayLitExpr>(expr), object);
    case Expr::Kind::Conditional:
      return DestinationScanner::conditionalParts(cast<ConditionalExpr>(expr), sink);
    case Expr::Kind::Binary:
      return DestinationScanner::binaryParts(cast<BinaryExpr>(expr), sink);
    case Expr::Kind::Sequence:
      return DestinationScanner::sequenceParts(cast<SequenceExpr>(expr), sink);
    case Expr::Kind::Assign:
      return this->assignParts(cast<AssignExpr>(expr), run);
    case Expr::Kind::Call:
      return this->callParts(cast<CallExpr>(expr), run);
    case Expr::Kind::Function:
      run.steps.push_back(ScanFunction{cast<FunctionExpr>(expr)->fn});
      return {};
    case Expr::Kind::Template:
      return DestinationScanner::templateParts(cast<TemplateExpr>(expr));
    case Expr::Kind::Member:
      return {ScanExpr{cast<MemberExpr>(expr)->object, nullptr}};
    case Expr::Kind::Index:
      return {ScanExpr{cast<IndexExpr>(expr)->object, nullptr},
              ScanExpr{cast<IndexExpr>(expr)->index, nullptr}};
    case Expr::Kind::Unary:
      return {ScanExpr{cast<UnaryExpr>(expr)->operand, nullptr}};
    case Expr::Kind::Update:
      return {ScanExpr{cast<UpdateExpr>(expr)->target, nullptr}};
    case Expr::Kind::New:
      return this->newParts(cast<NewExpr>(expr), run);
    case Expr::Kind::Cast:
      return {ScanExpr{cast<CastExpr>(expr)->operand, nullptr}};
    case Expr::Kind::OptionalChain:
      return {ScanExpr{cast<OptionalChainExpr>(expr)->chain, nullptr}};
    case Expr::Kind::Yield:
      return {ScanExpr{cast<YieldExpr>(expr)->value, nullptr}};
    case Expr::Kind::Await:
      return {ScanExpr{cast<AwaitExpr>(expr)->value, nullptr}};
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

std::vector<ScanExpr> DestinationScanner::nameParts(const NameExpr *name, const Type *sink,
                                                    ScanRun &run) {
  bool isFound =
      sink != nullptr && name->name == run.scan.getName() && !run.scan.isHidden(name->name);
  if (isFound) {
    run.scan.addFound(sink, name->loc());
  }

  return {};
}

std::vector<ScanExpr> DestinationScanner::objectParts(const ObjectLitExpr *lit,
                                                      const Type *object) {
  std::vector<ScanExpr> parts;
  for (const ObjectLitExpr::Property &prop : lit->props) {
    parts.push_back(ScanExpr{prop.key(), nullptr});
    bool isTypedField = object != nullptr && object->isObject() && !prop.spread();
    const Field *field = isTypedField ? object->field(prop.name) : nullptr;
    parts.push_back(ScanExpr{prop.value, field != nullptr ? field->type : nullptr});
  }

  return parts;
}

std::vector<ScanExpr> DestinationScanner::conditionalParts(const ConditionalExpr *cond,
                                                           const Type *sink) {
  return {ScanExpr{cond->cond, nullptr}, ScanExpr{cond->thenExpr, sink},
          ScanExpr{cond->elseExpr, sink}};
}

std::vector<ScanExpr> DestinationScanner::binaryParts(const BinaryExpr *bin, const Type *sink) {
  bool isNullish = bin->op == BinaryOp::Nullish;
  bool isPassing = isNullish || bin->op == BinaryOp::LogAnd || bin->op == BinaryOp::LogOr;

  return {ScanExpr{bin->lhs, isNullish ? sink : nullptr},
          ScanExpr{bin->rhs, isPassing ? sink : nullptr}};
}

std::vector<ScanExpr> DestinationScanner::sequenceParts(const SequenceExpr *seq, const Type *sink) {
  std::vector<ScanExpr> parts;
  for (size_t i = 0; i < seq->exprs.size(); ++i) {
    bool isLast = i + 1 == seq->exprs.size();
    parts.push_back(ScanExpr{seq->exprs[i], isLast ? sink : nullptr});
  }

  return parts;
}

std::vector<ScanExpr> DestinationScanner::templateParts(const TemplateExpr *tmpl) {
  std::vector<ScanExpr> parts;
  for (const Expr *part : tmpl->exprs) {
    parts.push_back(ScanExpr{part, nullptr});
  }

  return parts;
}

std::vector<ScanExpr> DestinationScanner::arrayParts(const ArrayLitExpr *lit, const Type *object) {
  std::vector<ScanExpr> parts;
  for (size_t i = 0; i < lit->elements.size(); ++i) {
    const ArrayLitExpr::Element &e = lit->elements[i];
    const Type *to = nullptr;
    if (object && object->isArray()) {
      to = e.spread ? object : object->inner();
    } else if (object && object->isTuple() && !e.spread && i < object->fields().size()) {
      to = object->fields()[i].type;
    }
    parts.push_back(ScanExpr{e.expr, to});
  }

  return parts;
}

std::vector<ScanExpr> DestinationScanner::assignParts(const AssignExpr *assign, ScanRun &run) {
  auto *target = dyn_cast<NameExpr>(assign->target);
  if (target && target->name == run.scan.getName() && !run.scan.isHidden(target->name)) {
    if (const Type *type = assign->op ? nullptr : this->peekType(assign->value, run.scan)) {
      run.scan.addFound(type, assign->loc());
    }
    return {ScanExpr{assign->value, nullptr}};
  }
  run.steps.push_back(AssignValue{assign});
  if (!isa<NameExpr>(assign->target)) {
    return {ScanExpr{assign->target, nullptr}};
  }

  return {};
}

std::vector<ScanExpr> DestinationScanner::callParts(const CallExpr *call, ScanRun &run) {
  std::vector<const Type *> params = this->calleeParams(call, run.scan);
  std::vector<ScanExpr> parts = {ScanExpr{call->callee, nullptr}};
  for (size_t i = 0; i < call->args.size(); ++i) {
    const Type *to = i < params.size() && !call->spreads(i) ? params[i] : nullptr;
    parts.push_back(ScanExpr{call->args[i], to});
  }

  return parts;
}

std::vector<ScanExpr> DestinationScanner::newParts(const NewExpr *create, ScanRun &run) {
  const FuncSymbol *ctor = nullptr;
  if (ClassDecl *cls =
          run.scan.isHidden(create->name) ? nullptr : this->scopes->lookup(create->name).cls()) {
    if (cls->constructors.size() == 1 && cls->constructors[0]->sym) {
      ctor = cls->constructors[0]->sym;
    }
  }
  std::vector<ScanExpr> parts;
  for (size_t i = 0; i < create->args.size(); ++i) {
    const Type *to = ctor && i < ctor->params.size() && !ctor->hasRest ? ctor->params[i] : nullptr;
    parts.push_back(ScanExpr{create->args[i], to});
  }

  return parts;
}

const Type *DestinationScanner::peekName(llvm::StringRef name, const DestinationScan &scan) const {
  std::optional<const Type *> local = scan.findLocal(name);
  if (local.has_value()) {
    return *local;
  }
  VarSymbol *var = this->scopes->lookup(name).var();

  return var ? var->type : nullptr;
}

const Type *DestinationScanner::peekType(const Expr *expr, const DestinationScan &scan) const {
  std::vector<const Type *> answers;
  for (const Expr *node : ChildrenFirst::order(expr, DestinationScanner::peekParts)) {
    const Type *type = this->nodePeekType(node, scan, answers);
    answers.push_back(type);
  }

  return answers.back();
}

NodeChildren<const Expr *> DestinationScanner::peekParts(const Expr *expr) {
  if (auto *member = dyn_cast<MemberExpr>(expr)) {
    return {member->object};
  }
  if (auto *index = dyn_cast<IndexExpr>(expr)) {
    return {index->object};
  }
  if (auto *cond = dyn_cast<ConditionalExpr>(expr); cond && Destination::literalShaped(cond)) {
    return {cond->thenExpr, cond->elseExpr};
  }
  auto *call = dyn_cast<CallExpr>(expr);
  bool isLiteral = call != nullptr && DestinationScanner::isResultLiteral(call);
  if (isLiteral && !call->args.empty()) {
    return {call->args[0]};
  }

  return {};
}

const Type *DestinationScanner::nodePeekType(const Expr *expr, const DestinationScan &scan,
                                             std::vector<const Type *> &answers) const {
  if (auto *name = dyn_cast<NameExpr>(expr)) {
    return name->name == scan.getName() ? nullptr : this->peekName(name->name, scan);
  }
  if (auto *member = dyn_cast<MemberExpr>(expr)) {
    const Type *object = ChildrenFirst::takeAnswer(answers);
    return this->memberPeekType(member, object);
  }
  if (isa<IndexExpr>(expr)) {
    const Type *object = ChildrenFirst::takeAnswer(answers);
    if (object && object->isRecord()) {
      return object->valueType();
    }
    return object && object->isArray() ? object->inner() : nullptr;
  }
  if (auto *cond = dyn_cast<ConditionalExpr>(expr); cond && Destination::literalShaped(cond)) {
    const Type *b = ChildrenFirst::takeAnswer(answers);
    const Type *a = ChildrenFirst::takeAnswer(answers);
    const Type *common = a && b ? TypeRelations::unify(a, b) : nullptr;
    return common && !common->isError() ? common : nullptr;
  }
  if (auto *call = dyn_cast<CallExpr>(expr); call && DestinationScanner::isResultLiteral(call)) {
    return DestinationScanner::resultPeekType(call, answers);
  }

  return nullptr;
}

const Type *DestinationScanner::memberPeekType(const MemberExpr *member, const Type *object) const {
  if (member->name == "subtle" &&
      this->callChecker->isGlobalName(member->object, GlobalName::CRYPTO)) {
    VarSymbol *subtle = this->scopes->lookup(LibraryModuleNames::kSubtleName).var();
    return subtle ? subtle->type : nullptr;
  }
  const Type *plain = object ? object->nonOptional() : nullptr;
  if (plain && plain->isRecord()) {
    return plain->valueType();
  }
  const Field *field = plain && plain->isObject() ? plain->field(member->name) : nullptr;

  return field ? field->type : nullptr;
}

const Type *DestinationScanner::resultPeekType(const CallExpr *call,
                                               std::vector<const Type *> &answers) {
  const Type *half = Type::voidTy();
  if (!call->args.empty()) {
    half = ChildrenFirst::takeAnswer(answers);
    if (!half) {
      half = SharedHelpers::literalType(call->args[0]);
    }
  }
  if (!half) {
    return nullptr;
  }
  std::optional<GlobalName> made = GlobalNameTable::find(cast<NameExpr>(call->callee)->name);

  return made == GlobalName::OK ? Type::result(half, Type::never())
                                : Type::result(Type::never(), half);
}

std::vector<const Type *> DestinationScanner::calleeParams(const CallExpr *call,
                                                           const DestinationScan &scan) {
  auto *name = dyn_cast<NameExpr>(call->callee);
  if (name != nullptr) {
    return this->nameCalleeParams(call, name, scan);
  }
  auto *member = dyn_cast<MemberExpr>(call->callee);
  if (member != nullptr) {
    return this->memberCalleeParams(call, member, scan);
  }

  return {};
}

std::vector<const Type *> DestinationScanner::spreadParams(llvm::ArrayRef<const Type *> params,
                                                           bool hasRest, size_t argCount) {
  std::vector<const Type *> types(params.begin(), params.end());
  if (hasRest && !types.empty()) {
    const Type *rest = types.back();
    types.pop_back();
    const Type *element = rest != nullptr && rest->isArray() ? rest->inner() : nullptr;
    while (types.size() < argCount) {
      types.push_back(element);
    }
  }

  return types;
}

std::vector<const Type *> DestinationScanner::nameCalleeParams(const CallExpr *call,
                                                               const NameExpr *name,
                                                               const DestinationScan &scan) {
  size_t argCount = call->args.size();
  if (scan.isHidden(name->name)) {
    const Type *type = this->peekName(name->name, scan);
    if (type == nullptr || !type->isFunction()) {
      return {};
    }
    return DestinationScanner::spreadParams(type->params(), type->hasRest(), argCount);
  }
  ScopeEntry entry = this->scopes->lookup(name->name);
  VarSymbol *var = entry.var();
  FuncSymbol *func = entry.func();
  if (var != nullptr && var->function() != nullptr) {
    func = var->function()->sym;
  }
  if (func != nullptr) {
    return DestinationScanner::spreadParams(func->params, func->hasRest, argCount);
  }
  bool isFunctionVar = var != nullptr && var->type != nullptr && var->type->isFunction();
  if (isFunctionVar) {
    return DestinationScanner::spreadParams(var->type->params(), var->type->hasRest(), argCount);
  }

  return {};
}

std::vector<const Type *> DestinationScanner::memberCalleeParams(const CallExpr *call,
                                                                 const MemberExpr *member,
                                                                 const DestinationScan &scan) {
  size_t argCount = call->args.size();
  auto *owner = dyn_cast<NameExpr>(member->object);
  if (owner != nullptr && !scan.isHidden(owner->name)) {
    ClassDecl *cls = this->scopes->lookup(owner->name).cls();
    if (cls != nullptr) {
      return DestinationScanner::methodParams(cls, member->name, true, argCount);
    }
  }
  const Type *peeked = this->peekType(member->object, scan);
  if (peeked == nullptr) {
    return {};
  }
  ClassDecl *cls = ClassDeclarer::classOf(peeked->nonOptional());
  if (cls != nullptr) {
    return DestinationScanner::methodParams(cls, member->name, false, argCount);
  }
  const Type *object = peeked->nonOptional();
  if (object->isArray()) {
    return DestinationScanner::arrayMethodParams(object, member->name, argCount);
  }
  if (object->isObject()) {
    const Field *field = object->field(member->name);
    if (field != nullptr && field->type->isFunction()) {
      const Type *function = field->type;
      return DestinationScanner::spreadParams(function->params(), function->hasRest(), argCount);
    }
  }

  return DestinationScanner::dictMethodParams(object, member->name);
}

std::vector<const Type *> DestinationScanner::methodParams(const ClassDecl *cls,
                                                           llvm::StringRef name, bool isStatic,
                                                           size_t argCount) {
  std::vector<FunctionDecl *> methods = ClassDeclarer::methodsNamed(cls, name, isStatic);
  if (methods.size() != 1 || methods[0]->sym == nullptr) {
    return {};
  }
  const FuncSymbol *method = methods[0]->sym;

  return DestinationScanner::spreadParams(method->params, method->hasRest, argCount);
}

std::vector<const Type *>
DestinationScanner::arrayMethodParams(const Type *array, llvm::StringRef name, size_t argCount) {
  const ArrayMethodInfo *method = ArrayMethodTable::find(name);
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
                                                               llvm::StringRef name) {
  const DictMethodInfo *dictMethod = DictMethodTable::find(name);
  std::optional<DictMethod> method = std::nullopt;
  if (dictMethod != nullptr) {
    method = dictMethod->method;
  }
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
