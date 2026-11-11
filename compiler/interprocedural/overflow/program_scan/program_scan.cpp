#include "compiler/interprocedural/overflow/program_scan/program_scan.h"
#include "compiler/interprocedural/overflow/call_ranges/call_ranges.h"
#include "compiler/interprocedural/overflow/evaluator/evaluator.h"
#include "compiler/interprocedural/overflow/executor/executor.h"
#include "compiler/interprocedural/overflow/loop_ranges/loop_ranges.h"
#include "compiler/interprocedural/overflow/narrowing/narrowing.h"
#include "compiler/interprocedural/overflow/overflow_helpers/overflow_helpers.h"
#include "compiler/interprocedural/overflow/overflow_warnings/overflow_warnings.h"
#include "compiler/interprocedural/overflow/slots/slots.h"
#include "compiler/interprocedural/overflow/value_range/value_range.h"
#include "compiler/program/types/types.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Path.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

ProgramScan::ProgramScan(OverflowCheck &check)
    : check(check) {}

bool ProgramScan::onlyReads(Builtin builtin) const {
  switch (builtin) {
  case Builtin::ConsoleLog:
  case Builtin::ConsoleError:
  case Builtin::StringOf:
  case Builtin::JsonStringify:
  case Builtin::ArrConcat:
  case Builtin::ArrayIsArray:
  case Builtin::JsonIsJson:
  case Builtin::MathMin:
  case Builtin::MathMax:
  case Builtin::ArrayFrom:
    return true;
  default:
    return false;
  }
}

void ProgramScan::addFunction(const FunctionDecl *fn) {
  if (!fn || !fn->body || this->check.program_.unusedFunctions.contains(fn)) {
    return;
  }
  if (!fn->typeParams.empty() && !fn->isInstance) {
    return;
  }
  auto [_, isFirstVisit] = this->check.functionSet.insert(fn);
  if (!isFirstVisit) {
    return;
  }
  this->check.functions_.push_back(fn);
  if (fn->overrides() || fn->overridden()) {
    this->check.unknownCallers.insert(fn);
  }
}

void ProgramScan::declareVar(const VarSymbol *var, const VarDeclarator *decl, bool isConst) {
  if (!var || !this->check.declaring) {
    return;
  }
  OverflowCheck::VarInfo &info = this->check.vars_[var];
  if (info.declared) {
    return;
  }
  info.declared = true;
  info.owner = this->check.scanOwner;
  info.decl = decl;
  info.isConst = isConst;
}

void ProgramScan::useVar(const VarSymbol *var, bool consumed, bool write) {
  if (!var || this->check.declaring) {
    return;
  }
  OverflowCheck::VarInfo &info = this->check.vars_[var];
  bool isElsewhere = !info.declared || info.owner != this->check.scanOwner;
  if (isElsewhere) {
    info.shared = true;
    if (write) {
      info.writtenElsewhere = true;
    }
  }
  if (var->type && var->type->nonOptional()->isArray() && (!consumed || isElsewhere)) {
    info.aliased = true;
  }
}

void ProgramScan::scanProgram() {
  for (bool declaring : {true, false}) {
    this->check.declaring = declaring;
    this->check.scanOwner = nullptr;
    for (const Stmt *item : this->check.program_.items) {
      if (auto *fn = dyn_cast<FunctionDecl>(item)) {
        addFunction(fn);
        continue;
      }
      if (auto *cls = dyn_cast<ClassDecl>(item)) {
        this->runScan({ScanClass{cls}});
        continue;
      }
      this->runScan({ScanStmt{item}});
    }
    for (const FunctionDecl *fn : this->check.program_.inventory.instances) {
      addFunction(fn);
    }
    for (size_t i = 0; i < this->check.functions_.size(); ++i) {
      this->scanFunction(this->check.functions_[i]);
    }
  }
}

void ProgramScan::scanFunction(const FunctionDecl *fn) {
  const FunctionDecl *outer = this->check.scanOwner;
  this->check.scanOwner = fn;
  auto roots = std::vector<ScanStep>();
  for (const Param &p : fn->params) {
    roots.push_back(DeclareParam{p.sym});
    if (p.defaultValue()) {
      roots.push_back(ScanExpr{p.defaultValue(), false});
    }
  }
  if (fn->body) {
    roots.push_back(ScanStmt{fn->body});
  }
  this->runScan(roots);
  this->check.scanOwner = outer;
}

void ProgramScan::runScan(const std::vector<ScanStep> &roots) {
  auto steps = std::vector<ScanStep>(roots.rbegin(), roots.rend());
  while (!steps.empty()) {
    auto step = std::move(steps.back());
    steps.pop_back();
    this->applyStep(step, steps);
  }
}

void ProgramScan::applyStep(const ScanStep &step, std::vector<ScanStep> &steps) {
  switch (step.kind()) {
    case ScanStepKind::SCAN_STMT: {
      const Stmt *stmt = step.as<ScanStmt>()->stmt;
      if (stmt) {
        this->pushParts(steps, this->stmtParts(stmt));
      }
      return;
    }
    case ScanStepKind::SCAN_EXPR: {
      const ScanExpr *scanned = step.as<ScanExpr>();
      if (scanned->expr) {
        this->pushParts(steps, this->exprParts(scanned->expr, scanned->consumed));
      }
      return;
    }
    case ScanStepKind::SCAN_CLASS:
      return this->pushParts(steps, this->classParts(step.as<ScanClass>()->cls));
    case ScanStepKind::DECLARE_VAR: {
      const DeclareVar *declared = step.as<DeclareVar>();
      return this->declareVar(declared->var, declared->decl, declared->isConst);
    }
    case ScanStepKind::DECLARE_PARAM:
      return this->declareParam(step.as<DeclareParam>()->var);
    case ScanStepKind::FINISH_DECLARATOR:
      return this->finishDeclarator(*step.as<FinishDeclarator>());
    case ScanStepKind::INLINE_CALLBACK:
      return this->pushParts(steps, this->inlineParts(*step.as<InlineCallback>()));
    case ScanStepKind::ADD_FUNCTION:
      return this->addFunction(step.as<AddFunction>()->fn);
    case ScanStepKind::USE_VAR: {
      const UseVar *used = step.as<UseVar>();
      return this->useVar(used->var, used->consumed, used->write);
    }
  }
}

void ProgramScan::pushParts(std::vector<ScanStep> &steps,
                            const std::vector<ScanStep> &parts) const {
  steps.insert(steps.end(), parts.rbegin(), parts.rend());
}

void ProgramScan::declareParam(const VarSymbol *var) {
  declareVar(var);
  if (!this->check.declaring && var && var->type && var->type->nonOptional()->isArray()) {
    this->check.vars_[var].aliased = true;
  }
}

void ProgramScan::finishDeclarator(const FinishDeclarator &step) {
  if (!this->check.declaring) {
    return;
  }
  const VarDeclarator *d = step.declarator;
  this->check.inits[d->init] = {d, step.isConst};
  if (d->sym && step.isConst && isConstantTree(d->init)) {
    this->check.constants.insert(d->sym);
  }
}

std::vector<ProgramScan::ScanStep> ProgramScan::classParts(const ClassDecl *cls) {
  if (cls->builtin()) {
    return {};
  }
  for (const FunctionDecl *fn : cls->methods) {
    addFunction(fn);
  }
  for (const FunctionDecl *fn : cls->constructors) {
    addFunction(fn);
  }
  addFunction(cls->init);
  auto parts = std::vector<ScanStep>();
  for (const ClassField &f : cls->fields) {
    if (f.method()) {
      parts.push_back(AddFunction{f.method()});
    }
    if (f.isStatic()) {
      parts.push_back(DeclareVar{f.var(), nullptr, false});
      if (f.init) {
        parts.push_back(ScanExpr{f.init, false});
      }
      parts.push_back(UseVar{f.var(), false, true});
    }
  }

  return parts;
}

std::vector<ProgramScan::ScanStep> ProgramScan::stmtParts(const Stmt *stmt) {
  switch (stmt->kind()) {
    case Stmt::Kind::Block: {
      auto parts = std::vector<ScanStep>();
      for (const Stmt *s : cast<BlockStmt>(stmt)->body) {
        parts.push_back(ScanStmt{s});
      }
      return parts;
    }
    case Stmt::Kind::VarDecl:
      return this->declaratorParts(cast<VarDeclStmt>(stmt));
    case Stmt::Kind::Function:
      if (this->check.declaring) {
        addFunction(cast<FunctionDecl>(stmt));
      }
      return {};
    case Stmt::Kind::Expr:
      return {ScanExpr{cast<ExprStmt>(stmt)->expr, true}};
    case Stmt::Kind::If: {
      auto *s = cast<IfStmt>(stmt);
      return {ScanExpr{s->cond, true}, ScanStmt{s->thenStmt}, ScanStmt{s->elseStmt}};
    }
    case Stmt::Kind::While: {
      auto *s = cast<WhileStmt>(stmt);
      return {ScanExpr{s->cond, true}, ScanStmt{s->body}};
    }
    case Stmt::Kind::DoWhile: {
      auto *s = cast<DoWhileStmt>(stmt);
      return {ScanStmt{s->body}, ScanExpr{s->cond, true}};
    }
    case Stmt::Kind::For:
      return this->forParts(cast<ForStmt>(stmt));
    case Stmt::Kind::ForOf: {
      auto *s = cast<ForOfStmt>(stmt);
      declareVar(s->binding.sym, &s->binding, s->isConst);
      return {ScanExpr{s->iterable, true}, ScanStmt{s->body}};
    }
    case Stmt::Kind::Switch:
      return this->switchParts(cast<SwitchStmt>(stmt));
    case Stmt::Kind::Labeled:
      return {ScanStmt{cast<LabeledStmt>(stmt)->body}};
    case Stmt::Kind::Return:
      return {ScanExpr{cast<ReturnStmt>(stmt)->value, false}};
    case Stmt::Kind::Class:
      return {ScanClass{cast<ClassDecl>(stmt)}};
    case Stmt::Kind::Break:
    case Stmt::Kind::Continue:
    case Stmt::Kind::Empty:
    case Stmt::Kind::TypeAlias:
    case Stmt::Kind::Enum:
    case Stmt::Kind::Debugger:
      return {};
  }

  return {};
}

std::vector<ProgramScan::ScanStep> ProgramScan::declaratorParts(const VarDeclStmt *decl) const {
  auto parts = std::vector<ScanStep>();
  for (const VarDeclarator &d : decl->decls) {
    parts.push_back(DeclareVar{d.sym, &d, decl->isConst});
    if (!d.init) {
      continue;
    }
    parts.push_back(ScanExpr{d.init, d.destructured()});
    parts.push_back(FinishDeclarator{&d, decl->isConst});
  }

  return parts;
}

std::vector<ProgramScan::ScanStep> ProgramScan::forParts(const ForStmt *loop) const {
  return {ScanStmt{loop->init}, ScanExpr{loop->cond, true}, ScanExpr{loop->step, true},
          ScanStmt{loop->body}};
}

std::vector<ProgramScan::ScanStep> ProgramScan::switchParts(const SwitchStmt *choice) const {
  auto parts = std::vector<ScanStep>();
  parts.push_back(ScanExpr{choice->discriminant, true});
  for (const SwitchCase &c : choice->cases) {
    parts.push_back(ScanExpr{c.test, true});
    for (const Stmt *body : c.body) {
      parts.push_back(ScanStmt{body});
    }
  }

  return parts;
}

std::vector<ProgramScan::ScanStep> ProgramScan::exprParts(const Expr *expr, bool consumed) {
  switch (expr->kind()) {
  case Expr::Kind::IntLit:
  case Expr::Kind::FloatLit:
  case Expr::Kind::BoolLit:
  case Expr::Kind::StringLit:
  case Expr::Kind::Undefined:
  case Expr::Kind::RegexLit:
  case Expr::Kind::Class:
    return {};
  case Expr::Kind::Template: {
    auto parts = std::vector<ScanStep>();
    for (const Expr *e : cast<TemplateExpr>(expr)->exprs) {
      parts.push_back(ScanExpr{e, true});
    }
    return parts;
  }
  case Expr::Kind::ArrayLit: {
    auto parts = std::vector<ScanStep>();
    for (const ArrayLitExpr::Element &e : cast<ArrayLitExpr>(expr)->elements) {
      parts.push_back(ScanExpr{e.expr, e.spread});
    }
    return parts;
  }
  case Expr::Kind::ObjectLit: {
    auto parts = std::vector<ScanStep>();
    for (const ObjectLitExpr::Property &p : cast<ObjectLitExpr>(expr)->props) {
      parts.push_back(ScanExpr{p.key(), true});
      parts.push_back(ScanExpr{p.value, p.spread()});
    }
    return parts;
  }
  case Expr::Kind::Name:
    return this->nameParts(cast<NameExpr>(expr), consumed);
  case Expr::Kind::Member: {
    auto *member = cast<MemberExpr>(expr);
    if (!this->check.declaring && member->method() && member->method()->decl) {
      this->check.unknownCallers.insert(member->method()->decl);
    }
    return {ScanExpr{member->object, true}};
  }
  case Expr::Kind::Index: {
    auto *index = cast<IndexExpr>(expr);
    return {ScanExpr{index->object, true}, ScanExpr{index->index, true}};
  }
  case Expr::Kind::Unary:
    return {ScanExpr{cast<UnaryExpr>(expr)->operand, true}};
  case Expr::Kind::Binary: {
    auto *bin = cast<BinaryExpr>(expr);
    bool isComparing = bin->op == BinaryOp::Eq || bin->op == BinaryOp::Ne;
    bool isGiving =
        bin->op == BinaryOp::Nullish || bin->op == BinaryOp::LogAnd || bin->op == BinaryOp::LogOr;
    bool isConsumed = isComparing || !isGiving;
    return {ScanExpr{bin->lhs, isConsumed}, ScanExpr{bin->rhs, isConsumed}};
  }
  case Expr::Kind::Assign:
    return this->assignParts(cast<AssignExpr>(expr));
  case Expr::Kind::Update: {
    auto *update = cast<UpdateExpr>(expr);
    if (auto *name = dyn_cast<NameExpr>(update->target)) {
      useVar(name->var(), true, true);
      return {};
    }
    return {ScanExpr{update->target, true}};
  }
  case Expr::Kind::Call:
    return this->callParts(cast<CallExpr>(expr));
  case Expr::Kind::New: {
    auto *ne = cast<NewExpr>(expr);
    auto parts = std::vector<ScanStep>();
    for (const Expr *arg : ne->ctor() ? ne->ctor()->args : ne->args) {
      parts.push_back(ScanExpr{arg, false});
    }
    return parts;
  }
  case Expr::Kind::Conditional: {
    auto *c = cast<ConditionalExpr>(expr);
    return {ScanExpr{c->cond, true}, ScanExpr{c->thenExpr, consumed},
            ScanExpr{c->elseExpr, consumed}};
  }
  case Expr::Kind::Cast: {
    auto *conv = cast<CastExpr>(expr);
    bool isConsumed = consumed || (conv->type && conv->type->isString());
    return {ScanExpr{conv->operand, isConsumed}};
  }
  case Expr::Kind::Sequence: {
    auto &exprs = cast<SequenceExpr>(expr)->exprs;
    auto parts = std::vector<ScanStep>();
    for (size_t i = 0; i < exprs.size(); ++i) {
      parts.push_back(ScanExpr{exprs[i], i + 1 < exprs.size() || consumed});
    }
    return parts;
  }
  case Expr::Kind::OptionalChain:
    return {ScanExpr{cast<OptionalChainExpr>(expr)->chain, consumed}};
  case Expr::Kind::Function: {
    const FunctionDecl *fn = cast<FunctionExpr>(expr)->fn;
    if (this->check.declaring && !this->check.inlined.contains(fn)) {
      addFunction(fn);
    }
    return {};
  }
  case Expr::Kind::Yield:
    return {ScanExpr{cast<YieldExpr>(expr)->value, false}};
  case Expr::Kind::Await:
    return {ScanExpr{cast<AwaitExpr>(expr)->value, false}};
  }

  return {};
}

std::vector<ProgramScan::ScanStep> ProgramScan::nameParts(const NameExpr *name, bool consumed) {
  useVar(name->var(), consumed, false);
  if (this->check.declaring) {
    return {};
  }
  if (name->func() && name->func()->decl) {
    this->check.unknownCallers.insert(name->func()->decl);
  }
  if (name->var() && name->var()->function()) {
    this->check.unknownCallers.insert(name->var()->function());
  }
  if (name->var() && name->var()->lambda()) {
    this->check.unknownCallers.insert(name->var()->lambda());
  }

  return {};
}

std::vector<ProgramScan::ScanStep> ProgramScan::assignParts(const AssignExpr *assign) {
  auto *name = dyn_cast<NameExpr>(assign->target);
  if (!name) {
    return {ScanExpr{assign->target, true}, ScanExpr{assign->value, false}};
  }
  useVar(name->var(), true, true);
  if (this->check.declaring && !assign->op) {
    if (auto *bin = dyn_cast<BinaryExpr>(assign->value)) {
      if (auto *lhs = dyn_cast<NameExpr>(OverflowHelpers::stripCasts(bin->lhs));
          lhs && lhs->var() == name->var()) {
        this->check.selfAssigned[bin] = name->var();
      }
    }
  }

  return {ScanExpr{assign->value, false}};
}

std::vector<ProgramScan::ScanStep> ProgramScan::callParts(const CallExpr *call) {
  auto parts = std::vector<ScanStep>();
  if (auto *member = dyn_cast<MemberExpr>(call->callee)) {
    parts.push_back(ScanExpr{member->object, true});
  } else if (auto *name = dyn_cast<NameExpr>(call->callee);
             name && (call->func() || call->builtin() != Builtin::None)) {
    useVar(name->var(), true, false);
  } else {
    parts.push_back(ScanExpr{call->callee, true});
  }
  std::optional<size_t> callback = OverflowHelpers::callbackArg(call->builtin());
  bool isReading = this->onlyReads(call->builtin());
  for (size_t i = 0; i < call->args.size(); ++i) {
    const Expr *arg = call->args[i];
    bool isCallbackArg = callback && *callback == i;
    auto *fe = isCallbackArg ? dyn_cast<FunctionExpr>(OverflowHelpers::stripCasts(arg)) : nullptr;
    if (fe) {
      parts.push_back(InlineCallback{fe, call->builtin()});
      continue;
    }
    parts.push_back(ScanExpr{arg, isReading || call->spreads(i)});
  }

  return parts;
}

std::vector<ProgramScan::ScanStep> ProgramScan::inlineParts(const InlineCallback &step) {
  const FunctionDecl *fn = step.callback->fn;
  if (this->check.declaring) {
    this->check.inlined.insert(fn);
  }
  bool isReducing = step.builtin == Builtin::ArrReduce || step.builtin == Builtin::ArrReduceRight;
  if (this->check.declaring && !fn->params.empty() && isReducing) {
    this->check.accumulators.insert(fn->params[0].sym);
  }
  auto parts = std::vector<ScanStep>();
  for (const Param &p : fn->params) {
    parts.push_back(DeclareVar{p.sym, nullptr, false});
    if (p.defaultValue()) {
      parts.push_back(ScanExpr{p.defaultValue(), false});
    }
  }
  parts.push_back(ScanStmt{fn->body});

  return parts;
}

bool ProgramScan::isConstantTree(const Expr *expr) const {
  auto pending = std::vector<const Expr *>{expr};
  while (!pending.empty()) {
    const Expr *node = OverflowHelpers::stripCasts(pending.back());
    pending.pop_back();
    if (node == nullptr) {
      return false;
    }
    if (isa<IntLitExpr, FloatLitExpr>(node)) {
      continue;
    }
    const auto *unary = dyn_cast<UnaryExpr>(node);
    const auto *binary = dyn_cast<BinaryExpr>(node);
    const auto *name = dyn_cast<NameExpr>(node);
    if (unary != nullptr && unary->op != UnaryOp::Not) {
      pending.push_back(unary->operand);
    } else if (binary != nullptr) {
      pending.push_back(binary->rhs);
      pending.push_back(binary->lhs);
    } else if (name == nullptr || !name->var() || !this->check.constants.contains(name->var())) {
      return false;
    }
  }

  return true;
}

}
