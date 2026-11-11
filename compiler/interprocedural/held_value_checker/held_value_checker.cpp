#include "compiler/interprocedural/held_value_checker/held_value_checker.h"
#include "compiler/interprocedural/held_value_checker/held_value_checker_errors.h"
#include "compiler/parallel/parallel_helpers/parallel_helpers.h"
#include "compiler/program/module/module.h"
#include "compiler/program/types/types.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

HeldValueChecker::HeldValueChecker(Program &checked, const LibraryRanges &library)
    : program(checked),
      library(library),
      current(nullptr),
      reporting(false) {}

std::string HeldValueChecker::calleeName(const CallExpr *c) {
  const Expr *callee = c->callee;
  if (auto *member = dyn_cast_or_null<MemberExpr>(callee)) {
    return member->name.str();
  }
  if (auto *name = dyn_cast_or_null<NameExpr>(callee)) {
    return name->name.str();
  }

  return "";
}

bool HeldValueChecker::usesModule(llvm::StringRef name) const {
  return llvm::any_of(this->program.file.modules,
                      [&](const Module *m) { return m->builtin && m->name == name; });
}

bool HeldValueChecker::inLibrary(llvm::SMLoc loc) const {
  return this->library.contains(loc);
}

HeldValueChecker::ResultHeldValuesChecked HeldValueChecker::run() {
  if (!this->used()) {
    return ok();
  }
  auto known = [&] {
    return this->heldVars.size() + this->heldParams.size() + this->returnsHeld.size() +
           this->heldThis.size() + this->extraKnown();
  };
  for (int round = 0; round < 32; ++round) {
    size_t before = known();
    this->walkProgram();
    if (known() == before) {
      break;
    }
  }
  this->reporting = true;
  auto inWalk = this->walkProgram();
  auto atFinish = this->finish();

  auto found = std::vector<Diagnostic>();
  llvm::append_range(found, inWalk);
  llvm::append_range(found, atFinish);
  if (!found.empty()) {
    return err(found);
  }

  return ok();
}

std::vector<Diagnostic> HeldValueChecker::walkProgram() {
  this->walked.clear();
  this->callees.clear();
  this->allowedNews.clear();
  this->awaited.clear();
  this->allowedStores.clear();
  this->callSites.clear();
  this->startWalk();
  auto roots = this->programRoots();
  auto steps = std::vector<WalkStep>(roots.rbegin(), roots.rend());

  auto found = std::vector<Diagnostic>();
  while (!steps.empty()) {
    auto step = std::move(steps.back());
    steps.pop_back();
    auto stepFound = this->applyStep(step, steps);
    llvm::append_range(found, stepFound);
  }

  return found;
}

std::vector<HeldValueChecker::WalkStep> HeldValueChecker::programRoots() const {
  auto roots = std::vector<WalkStep>();
  SourceFile &file = this->program.file;
  for (Stmt *item : file.items) {
    auto *function = dyn_cast<FunctionDecl>(item);
    auto *cls = dyn_cast<ClassDecl>(item);
    if (function != nullptr) {
      if (function->typeParams.empty() && !file.unusedFunctions.contains(function)) {
        roots.push_back(StartFunction{function});
      }
      continue;
    }
    if (cls != nullptr) {
      auto rootsOfClass = this->classRoots(cls);
      llvm::append_range(roots, rootsOfClass);
    }
    roots.push_back(StartTopLevel{item});
  }
  for (const FunctionDecl *fn : file.inventory.instances) {
    roots.push_back(StartFunction{fn});
  }

  return roots;
}

std::vector<HeldValueChecker::WalkStep> HeldValueChecker::classRoots(ClassDecl *cls) const {
  auto roots = std::vector<WalkStep>();
  for (const FunctionDecl *fn : cls->methods) {
    if (fn->typeParams.empty() && (fn->isStatic() ? !cls->generic() : cls->isEmitted())) {
      roots.push_back(StartFunction{fn});
    }
  }
  if (!cls->isEmitted()) {
    return roots;
  }
  for (const FunctionDecl *fn : cls->methodInstances) {
    roots.push_back(StartFunction{fn});
  }
  for (const FunctionDecl *fn : cls->constructors) {
    roots.push_back(StartFunction{fn});
  }
  roots.push_back(StartFunction{cls->init});
  for (const ClassField &f : cls->fields) {
    roots.push_back(StartFunction{f.method()});
  }

  return roots;
}

std::vector<Diagnostic> HeldValueChecker::applyStep(const WalkStep &step,
                                                    std::vector<WalkStep> &steps) {
  switch (step.kind()) {
    case WalkStepKind::START_TOP_LEVEL:
      this->startTopLevel(step.as<StartTopLevel>()->stmt, steps);
      return {};
    case WalkStepKind::START_FUNCTION:
      this->startFunction(step.as<StartFunction>()->fn, steps);
      return {};
    case WalkStepKind::START_PARAM:
      this->startParam(*step.as<StartParam>(), steps);
      return {};
    case WalkStepKind::FINISH_FUNCTION:
      this->current = step.as<FinishFunction>()->outer;
      return {};
    case WalkStepKind::START_STMT:
      this->startStmt(step.as<StartStmt>()->stmt, steps);
      return {};
    case WalkStepKind::START_EXPR:
      this->startExpr(step.as<StartExpr>()->expr, steps);
      return {};
    case WalkStepKind::MARK_AWAITED:
      this->awaited.insert(HeldValueChecker::bare(step.as<MarkAwaited>()->value));
      return {};
    case WalkStepKind::START_CALL:
      return this->sawCall(step.as<StartCall>()->call);
    case WalkStepKind::FINISH_CALL:
      return this->finishCall(step.as<FinishCall>()->call);
    case WalkStepKind::FINISH_NEW:
      return this->finishNew(step.as<FinishNew>()->create);
    case WalkStepKind::FINISH_ASSIGN:
      return this->finishAssign(step.as<FinishAssign>()->assign);
    case WalkStepKind::FINISH_UNARY:
      return this->finishUnary(step.as<FinishUnary>()->unary);
    case WalkStepKind::FINISH_PROPERTY:
      return this->finishProperty(*step.as<FinishProperty>());
    case WalkStepKind::FINISH_ELEMENT:
      return this->finishElement(step.as<FinishElement>()->element);
    case WalkStepKind::FINISH_DECLARATOR:
      return this->finishDeclarator(step.as<FinishDeclarator>()->declarator);
    case WalkStepKind::FINISH_RETURN:
      this->finishReturn(step.as<FinishReturn>()->value);
      return {};
  }

  return {};
}

void HeldValueChecker::startTopLevel(const Stmt *s, std::vector<WalkStep> &steps) {
  if (s == nullptr || this->inLibrary(s->loc())) {
    return;
  }
  this->current = nullptr;
  const auto *decl = dyn_cast<VarDeclStmt>(s);
  if (decl != nullptr && decl->isConst) {
    for (const VarDeclarator &d : decl->decls) {
      if (!d.destructured()) {
        this->allowedNews.insert(HeldValueChecker::bare(d.init));
      }
    }
  }
  steps.push_back(StartStmt{s});
}

void HeldValueChecker::startFunction(const FunctionDecl *fn, std::vector<WalkStep> &steps) {
  if (fn == nullptr || fn->body == nullptr || this->inLibrary(fn->loc())) {
    return;
  }
  auto [_, isInserted] = this->walked.insert(fn);
  if (!isInserted) {
    return;
  }
  steps.push_back(FinishFunction{this->current});
  this->current = fn;
  if (fn->thisVar() != nullptr && this->heldThis.contains(fn)) {
    this->heldVars.insert(fn->thisVar());
  }

  auto parts = std::vector<WalkStep>();
  for (unsigned index = 0; index < fn->params.size(); ++index) {
    parts.push_back(StartParam{fn, index});
  }
  parts.push_back(StartStmt{fn->body});
  steps.insert(steps.end(), parts.rbegin(), parts.rend());
}

void HeldValueChecker::startParam(const StartParam &step, std::vector<WalkStep> &steps) {
  const Param &param = step.fn->params[step.index];
  if (param.sym != nullptr && this->heldParams.contains({step.fn, step.index})) {
    this->heldVars.insert(param.sym);
  }
  steps.push_back(StartExpr{param.defaultValue()});
}

void HeldValueChecker::startStmt(const Stmt *s, std::vector<WalkStep> &steps) const {
  if (s == nullptr) {
    return;
  }
  auto parts = this->stmtParts(s);
  steps.insert(steps.end(), parts.rbegin(), parts.rend());
}

void HeldValueChecker::startExpr(const Expr *e, std::vector<WalkStep> &steps) const {
  if (e == nullptr) {
    return;
  }
  auto parts = this->exprParts(e);
  steps.insert(steps.end(), parts.rbegin(), parts.rend());
}

std::vector<HeldValueChecker::WalkStep> HeldValueChecker::stmtParts(const Stmt *s) const {
  switch (s->kind()) {
    case Stmt::Kind::Block:
      return this->blockParts(cast<BlockStmt>(s));
    case Stmt::Kind::VarDecl:
      return this->declaratorParts(cast<VarDeclStmt>(s));
    case Stmt::Kind::Function:
      return {StartFunction{cast<FunctionDecl>(s)}};
    case Stmt::Kind::Expr:
      return {StartExpr{cast<ExprStmt>(s)->expr}};
    case Stmt::Kind::If: {
      const auto *branch = cast<IfStmt>(s);
      return {StartExpr{branch->cond}, StartStmt{branch->thenStmt}, StartStmt{branch->elseStmt}};
    }
    case Stmt::Kind::While: {
      const auto *loop = cast<WhileStmt>(s);
      return {StartExpr{loop->cond}, StartStmt{loop->body}};
    }
    case Stmt::Kind::DoWhile: {
      const auto *loop = cast<DoWhileStmt>(s);
      return {StartStmt{loop->body}, StartExpr{loop->cond}};
    }
    case Stmt::Kind::For: {
      const auto *loop = cast<ForStmt>(s);
      return {StartStmt{loop->init}, StartExpr{loop->cond}, StartExpr{loop->step},
              StartStmt{loop->body}};
    }
    case Stmt::Kind::ForOf: {
      const auto *loop = cast<ForOfStmt>(s);
      return {StartExpr{loop->iterable}, StartStmt{loop->body}};
    }
    case Stmt::Kind::Switch:
      return this->switchParts(cast<SwitchStmt>(s));
    case Stmt::Kind::Labeled:
      return {StartStmt{cast<LabeledStmt>(s)->body}};
    case Stmt::Kind::Return: {
      const Expr *value = cast<ReturnStmt>(s)->value;
      return {MarkAwaited{value}, StartExpr{value}, FinishReturn{value}};
    }
    case Stmt::Kind::Class:
      return this->classParts(cast<ClassDecl>(s));
    default:
      return {};
  }
}

std::vector<HeldValueChecker::WalkStep> HeldValueChecker::blockParts(const BlockStmt *block) const {
  auto parts = std::vector<WalkStep>();
  if (block->group != nullptr) {
    for (const FunctionDecl *member : block->group->members) {
      parts.push_back(StartFunction{member});
    }
  }
  for (const Stmt *stmt : block->body) {
    parts.push_back(StartStmt{stmt});
  }

  return parts;
}

std::vector<HeldValueChecker::WalkStep>
HeldValueChecker::declaratorParts(const VarDeclStmt *decl) const {
  auto parts = std::vector<WalkStep>();
  for (const VarDeclarator &d : decl->decls) {
    parts.push_back(StartExpr{d.init});
    parts.push_back(FinishDeclarator{&d});
  }

  return parts;
}

std::vector<HeldValueChecker::WalkStep> HeldValueChecker::switchParts(const SwitchStmt *s) const {
  auto parts = std::vector<WalkStep>();
  parts.push_back(StartExpr{s->discriminant});
  for (const SwitchCase &c : s->cases) {
    parts.push_back(StartExpr{c.test});
    for (const Stmt *stmt : c.body) {
      parts.push_back(StartStmt{stmt});
    }
  }

  return parts;
}

std::vector<HeldValueChecker::WalkStep> HeldValueChecker::classParts(const ClassDecl *cls) const {
  auto parts = std::vector<WalkStep>();
  for (const ClassField &f : cls->fields) {
    if (f.isStatic()) {
      parts.push_back(StartExpr{f.init});
    }
  }
  for (const Stmt *decoration : cls->decorations) {
    parts.push_back(StartStmt{decoration});
  }

  return parts;
}

std::vector<HeldValueChecker::WalkStep> HeldValueChecker::exprParts(const Expr *e) const {
  switch (e->kind()) {
    case Expr::Kind::Function:
      return {StartFunction{cast<FunctionExpr>(e)->fn}};
    case Expr::Kind::Await: {
      const Expr *value = cast<AwaitExpr>(e)->value;
      return {MarkAwaited{value}, StartExpr{value}};
    }
    case Expr::Kind::Call:
      return this->callParts(cast<CallExpr>(e));
    case Expr::Kind::New:
      return this->newParts(cast<NewExpr>(e));
    case Expr::Kind::Assign: {
      const auto *assign = cast<AssignExpr>(e);
      return {StartExpr{assign->target}, StartExpr{assign->value}, FinishAssign{assign}};
    }
    case Expr::Kind::Unary: {
      const auto *unary = cast<UnaryExpr>(e);
      return {StartExpr{unary->operand}, FinishUnary{unary}};
    }
    case Expr::Kind::ObjectLit:
      return this->objectParts(cast<ObjectLitExpr>(e));
    case Expr::Kind::ArrayLit:
      return this->arrayParts(cast<ArrayLitExpr>(e));
    case Expr::Kind::Template:
      return this->exprSteps(cast<TemplateExpr>(e)->exprs);
    case Expr::Kind::Member:
      return {StartExpr{cast<MemberExpr>(e)->object}};
    case Expr::Kind::Index: {
      const auto *index = cast<IndexExpr>(e);
      return {StartExpr{index->object}, StartExpr{index->index}};
    }
    case Expr::Kind::Binary: {
      const auto *binary = cast<BinaryExpr>(e);
      return {StartExpr{binary->lhs}, StartExpr{binary->rhs}};
    }
    case Expr::Kind::Update:
      return {StartExpr{cast<UpdateExpr>(e)->target}};
    case Expr::Kind::Conditional: {
      const auto *choice = cast<ConditionalExpr>(e);
      return {StartExpr{choice->cond}, StartExpr{choice->thenExpr}, StartExpr{choice->elseExpr}};
    }
    case Expr::Kind::Cast:
      return {StartExpr{cast<CastExpr>(e)->operand}};
    case Expr::Kind::Sequence:
      return this->exprSteps(cast<SequenceExpr>(e)->exprs);
    case Expr::Kind::OptionalChain:
      return {StartExpr{cast<OptionalChainExpr>(e)->chain}};
    case Expr::Kind::Yield:
      return {StartExpr{cast<YieldExpr>(e)->value}};
    default:
      return {};
  }
}

std::vector<HeldValueChecker::WalkStep>
HeldValueChecker::exprSteps(llvm::ArrayRef<Expr *> exprs) const {
  auto parts = std::vector<WalkStep>();
  for (const Expr *e : exprs) {
    parts.push_back(StartExpr{e});
  }

  return parts;
}

std::vector<HeldValueChecker::WalkStep> HeldValueChecker::callParts(const CallExpr *c) const {
  auto parts = std::vector<WalkStep>();
  parts.push_back(StartCall{c});
  parts.push_back(StartExpr{c->callee});
  auto args = this->exprSteps(c->args);
  llvm::append_range(parts, args);
  parts.push_back(FinishCall{c});

  return parts;
}

std::vector<HeldValueChecker::WalkStep> HeldValueChecker::newParts(const NewExpr *n) const {
  const std::vector<Expr *> &args = n->ctor() != nullptr ? n->ctor()->args : n->args;
  auto parts = this->exprSteps(args);
  parts.push_back(FinishNew{n});

  return parts;
}

std::vector<HeldValueChecker::WalkStep>
HeldValueChecker::objectParts(const ObjectLitExpr *literal) const {
  auto parts = std::vector<WalkStep>();
  for (const ObjectLitExpr::Property &p : literal->props) {
    parts.push_back(StartExpr{p.key()});
    parts.push_back(StartExpr{p.value});
    parts.push_back(FinishProperty{literal, &p});
  }

  return parts;
}

std::vector<HeldValueChecker::WalkStep>
HeldValueChecker::arrayParts(const ArrayLitExpr *literal) const {
  auto parts = std::vector<WalkStep>();
  for (const ArrayLitExpr::Element &element : literal->elements) {
    parts.push_back(StartExpr{element.expr});
    parts.push_back(FinishElement{&element});
  }

  return parts;
}

std::vector<Diagnostic> HeldValueChecker::finishCall(const CallExpr *c) {
  const FunctionDecl *target = this->programTarget(c);
  if (this->current != nullptr && target != nullptr) {
    this->callees[this->current].push_back(target);
  }
  if (target != nullptr) {
    this->callSites[target].push_back(c);
    if (target->generic != nullptr) {
      this->callSites[target->generic].push_back(c);
    }
  }
  bool isTakenOver = this->takesOver(c);
  if (this->current != nullptr && !isTakenOver) {
    for (const Expr *arg : c->args) {
      const auto *function = dyn_cast<FunctionExpr>(HeldValueChecker::bare(arg));
      if (function != nullptr) {
        this->callees[this->current].push_back(function->fn);
      }
    }
  }
  const auto *member = dyn_cast_or_null<MemberExpr>(c->callee);
  bool isHeldThis = member != nullptr && target != nullptr && c->method() != nullptr &&
                    this->isHeld(member->object);
  if (isHeldThis) {
    this->markThis(target);
  }
  if (isTakenOver) {
    return {};
  }
  std::string name = HeldValueChecker::calleeName(c);

  return this->passed(c, target, name);
}

std::vector<Diagnostic> HeldValueChecker::passed(const CallExpr *c, const FunctionDecl *target,
                                                 llvm::StringRef name) {
  auto found = std::vector<Diagnostic>();
  for (unsigned i = 0; i < c->args.size(); ++i) {
    const Expr *arg = c->args[i];
    if (!this->isHeld(arg)) {
      continue;
    }
    if (target) {
      this->markParam(target, i);
      continue;
    }
    if (parallel::ParallelHelpers::onlyReadsArgs(c->builtin()) ||
        c->builtin() == Builtin::ResultOk || c->builtin() == Builtin::ResultErr) {
      continue;
    }
    if (auto *fe = dyn_cast<FunctionExpr>(HeldValueChecker::bare(arg)); fe && fe->fn->nonEscaping) {
      continue;
    }
    std::string where =
        name.empty() ? ErrorTextHeldPlace::unknownFn() : ErrorTextHeldPlace::storedBy(name);
    auto keptArg = this->keep(arg, where);
    llvm::append_range(found, keptArg);
  }

  return found;
}

std::vector<Diagnostic> HeldValueChecker::finishNew(const NewExpr *n) {
  const std::vector<Expr *> &args = n->ctor() != nullptr ? n->ctor()->args : n->args;
  this->sawNew(n);
  const FunctionDecl *target = n->ctor() != nullptr ? this->programTarget(n->ctor()) : nullptr;
  if (this->current != nullptr && target != nullptr) {
    this->callees[this->current].push_back(target);
  }
  if (this->allowedStores.contains(n)) {
    return {};
  }

  auto found = std::vector<Diagnostic>();
  for (unsigned i = 0; i < args.size(); ++i) {
    if (!this->isHeld(args[i])) {
      continue;
    }
    if (target != nullptr) {
      this->markParam(target, i);
      continue;
    }
    std::string where = ErrorTextHeldPlace::newObject();
    auto keptArg = this->keep(args[i], where);
    llvm::append_range(found, keptArg);
  }

  return found;
}

std::vector<Diagnostic> HeldValueChecker::finishAssign(const AssignExpr *a) {
  if (a->op || !this->isHeld(a->value)) {
    return {};
  }
  const Expr *target = HeldValueChecker::bare(a->target);
  const auto *name = dyn_cast<NameExpr>(target);
  const VarSymbol *var = name != nullptr ? name->var() : nullptr;
  if (var == nullptr) {
    std::string where =
        isa<IndexExpr>(target) ? ErrorTextHeldPlace::container() : ErrorTextHeldPlace::field();
    return this->keep(a->value, where);
  }
  if (var->isGlobal()) {
    std::string where = ErrorTextHeldPlace::moduleVar();
    return this->keep(a->value, where);
  }
  if (var->owner() != nullptr && var->owner() != this->current) {
    std::string where = ErrorTextHeldPlace::outerVar();
    return this->keep(a->value, where);
  }
  this->heldVars.insert(var);

  return {};
}

std::vector<Diagnostic> HeldValueChecker::finishUnary(const UnaryExpr *unary) {
  const auto *call = dyn_cast_or_null<CallExpr>(HeldValueChecker::bare(unary->operand));
  if (call == nullptr || unary->op != UnaryOp::Void) {
    return {};
  }

  return this->sawVoid(call);
}

std::vector<Diagnostic> HeldValueChecker::finishProperty(const FinishProperty &step) {
  const ObjectLitExpr::Property *property = step.property;
  if (!this->isHeld(property->value)) {
    return {};
  }
  if (property->spread()) {
    return this->spreadKept(property->value);
  }
  if (this->allowedStores.contains(step.literal)) {
    return {};
  }
  std::string where = ErrorTextHeldPlace::newObject();

  return this->keep(property->value, where);
}

std::vector<Diagnostic> HeldValueChecker::finishElement(const ArrayLitExpr::Element *element) {
  if (element->spread || !this->isHeld(element->expr)) {
    return {};
  }
  std::string where = ErrorTextHeldPlace::array();

  return this->keep(element->expr, where);
}

std::vector<Diagnostic> HeldValueChecker::finishDeclarator(const VarDeclarator *d) {
  if (d->sym == nullptr || d->destructured() || !this->isHeld(d->init)) {
    return {};
  }
  if (d->sym->isGlobal()) {
    std::string where = ErrorTextHeldPlace::moduleVar();
    return this->keep(d->init, where);
  }
  this->heldVars.insert(d->sym);

  return {};
}

void HeldValueChecker::finishReturn(const Expr *value) {
  if (this->current != nullptr && this->isHeld(value)) {
    this->returnsHeld.insert(this->current);
  }
}

bool HeldValueChecker::isHeld(const Expr *e) const {
  auto pending = std::vector<const Expr *>{e};
  while (!pending.empty()) {
    const Expr *node = HeldValueChecker::bare(pending.back());
    pending.pop_back();
    if (node == nullptr) {
      continue;
    }
    if (this->isHeldItself(node)) {
      return true;
    }
    auto parts = this->heldParts(node);
    pending.insert(pending.end(), parts.rbegin(), parts.rend());
  }

  return false;
}

bool HeldValueChecker::isHeldItself(const Expr *e) const {
  switch (e->kind()) {
    case Expr::Kind::Call: {
      const auto *call = cast<CallExpr>(e);
      const FunctionDecl *target = this->programTarget(call);
      return this->produces(call) || (target != nullptr && this->returnsHeld.contains(target));
    }
    case Expr::Kind::Name: {
      const VarSymbol *var = cast<NameExpr>(e)->var();
      return var != nullptr && this->heldVars.contains(var);
    }
    case Expr::Kind::Function:
      return this->closuresCarry() && this->carries(cast<FunctionExpr>(e)->fn);
    default:
      return false;
  }
}

std::vector<const Expr *> HeldValueChecker::heldParts(const Expr *e) const {
  switch (e->kind()) {
    case Expr::Kind::Call: {
      const auto *call = cast<CallExpr>(e);
      bool isResult = call->builtin() == Builtin::ResultOk || call->builtin() == Builtin::ResultErr;
      if (!isResult || call->args.empty()) {
        return {};
      }
      return {call->args[0]};
    }
    case Expr::Kind::Member: {
      const auto *member = cast<MemberExpr>(e);
      const Type *object = member->object != nullptr && member->object->type != nullptr
                               ? member->object->type->nonOptional()
                               : nullptr;
      bool isResultPart = object != nullptr && object->isResult() &&
                          (member->name == "value" || member->name == "error");
      if (!this->heldWithObject(member) && !isResultPart) {
        return {};
      }
      return {member->object};
    }
    case Expr::Kind::OptionalChain:
      return {cast<OptionalChainExpr>(e)->chain};
    case Expr::Kind::Conditional: {
      const auto *choice = cast<ConditionalExpr>(e);
      return {choice->thenExpr, choice->elseExpr};
    }
    case Expr::Kind::Binary: {
      const auto *binary = cast<BinaryExpr>(e);
      bool isPick = binary->op == BinaryOp::LogOr || binary->op == BinaryOp::LogAnd ||
                    binary->op == BinaryOp::Nullish;
      if (!isPick) {
        return {};
      }
      return {binary->lhs, binary->rhs};
    }
    case Expr::Kind::Assign: {
      const auto *assign = cast<AssignExpr>(e);
      if (assign->op) {
        return {};
      }
      return {assign->value};
    }
    default:
      return {};
  }
}

bool HeldValueChecker::carries(const FunctionDecl *fn) const {
  if (!fn) {
    return false;
  }
  const std::vector<VarSymbol *> &captures = fn->group ? fn->group->captures : fn->captures;

  return llvm::any_of(captures, [&](const VarSymbol *v) { return this->heldVars.contains(v); });
}

std::vector<Diagnostic> HeldValueChecker::keep(const Expr *value, llvm::StringRef where) {
  if (!this->reporting || value == nullptr) {
    return {};
  }
  auto [_, isInserted] = this->reported.insert(value->loc().getPointer());
  if (!isInserted) {
    return {};
  }
  const Type *type = value->type ? value->type->nonOptional() : nullptr;
  bool isFunction = type != nullptr && type->isFunction();

  return this->kept(value, isFunction, where);
}

bool HeldValueChecker::once(const Expr *value) {
  return this->reporting && value && this->reported.insert(value->loc().getPointer()).second;
}

void HeldValueChecker::markParam(const FunctionDecl *fn, unsigned index) {
  if (fn && index < fn->params.size()) {
    this->heldParams.insert({fn, index});
  }
}

void HeldValueChecker::markThis(const FunctionDecl *fn) {
  if (!fn || !fn->thisVar() || fn->isStatic()) {
    return;
  }
  this->heldThis.insert(fn);
  if (!fn->cls()) {
    return;
  }
  std::vector<const ClassDecl *> pending(fn->cls()->children.begin(), fn->cls()->children.end());
  while (!pending.empty()) {
    const ClassDecl *cls = pending.back();
    pending.pop_back();
    for (const FunctionDecl *method : cls->methods) {
      for (const FunctionDecl *up = method->overrides(); up; up = up->overrides()) {
        if (up == fn) {
          this->heldThis.insert(method);
          break;
        }
      }
    }
    pending.insert(pending.end(), cls->children.begin(), cls->children.end());
  }
}

const Expr *HeldValueChecker::bare(const Expr *e) {
  while (auto *conv = dyn_cast_or_null<CastExpr>(e)) {
    e = conv->operand;
  }

  return e;
}

const FunctionDecl *HeldValueChecker::callbackOf(const Expr *e) {
  e = HeldValueChecker::bare(e);
  if (auto *fe = dyn_cast_or_null<FunctionExpr>(e)) {
    return fe->fn;
  }
  if (auto *name = dyn_cast_or_null<NameExpr>(e)) {
    if (name->func()) {
      return name->func()->decl;
    }
    if (name->var()) {
      return name->var()->lambda() ? name->var()->lambda() : name->var()->function();
    }
    return nullptr;
  }
  if (auto *member = dyn_cast_or_null<MemberExpr>(e); member && member->method()) {
    return member->method()->decl;
  }

  return nullptr;
}

const ClassDecl *HeldValueChecker::storageClass(const ClassDecl *cls) {
  for (; cls; cls = cls->parent) {
    const ClassDecl *base = cls->generic() ? cls->generic() : cls;
    if ((base->library() || cls->library()) && base->name == "AsyncLocalStorage") {
      return base;
    }
  }

  return nullptr;
}

bool HeldValueChecker::storageMethod(const CallExpr *c, llvm::StringRef name) {
  const FunctionDecl *decl = c->method() ? c->method()->decl : nullptr;

  return decl && decl->name == name && HeldValueChecker::storageClass(decl->cls());
}

const VarSymbol *HeldValueChecker::storageOf(const CallExpr *c) {
  auto *member = dyn_cast_or_null<MemberExpr>(c->callee);
  auto *name =
      member ? dyn_cast_or_null<NameExpr>(HeldValueChecker::bare(member->object)) : nullptr;

  return name ? name->var() : nullptr;
}

const FunctionDecl *HeldValueChecker::programTarget(const CallExpr *c) const {
  if (c->builtin() != Builtin::None) {
    return nullptr;
  }
  const FunctionDecl *decl = c->func()     ? c->func()->decl
                             : c->method() ? c->method()->decl
                                           : nullptr;
  if (!decl) {
    if (auto *name = dyn_cast_or_null<NameExpr>(c->callee);
        name && name->var() && !name->var()->reassigned) {
      decl = name->var()->lambda() ? name->var()->lambda() : name->var()->function();
    }
  }
  if (!decl || !decl->body || this->inLibrary(decl->loc())) {
    return nullptr;
  }

  return decl;
}

}
