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

bool ProgramScan::onlyReads(Builtin builtin) {
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
  if (!this->check.functionSet.insert(fn).second) {
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
  bool elsewhere = !info.declared || info.owner != this->check.scanOwner;
  if (elsewhere) {
    info.shared = true;
    if (write) {
      info.writtenElsewhere = true;
    }
  }
  if (var->type && var->type->nonOptional()->isArray() && (!consumed || elsewhere)) {
    info.aliased = true;
  }
}

void ProgramScan::scanFunction(const FunctionDecl *fn) {
  const FunctionDecl *outer = this->check.scanOwner;
  this->check.scanOwner = fn;
  for (const Param &p : fn->params) {
    declareVar(p.sym);
    if (!this->check.declaring && p.sym && p.sym->type && p.sym->type->nonOptional()->isArray()) {
      this->check.vars_[p.sym].aliased = true;
    }
    if (p.defaultValue()) {
      scanExpr(p.defaultValue(), false);
    }
  }
  if (fn->body) {
    scanStmt(fn->body);
  }
  this->check.scanOwner = outer;
}

void ProgramScan::scanClass(const ClassDecl *cls) {
  if (cls->builtin()) {
    return;
  }
  for (const FunctionDecl *fn : cls->methods) {
    addFunction(fn);
  }
  for (const FunctionDecl *fn : cls->constructors) {
    addFunction(fn);
  }
  addFunction(cls->init);
  for (const ClassField &f : cls->fields) {
    if (f.method()) {
      addFunction(f.method());
    }
    if (f.isStatic()) {
      declareVar(f.var());
      if (f.init) {
        scanExpr(f.init, false);
      }
      if (!this->check.declaring && f.var()) {
        useVar(f.var(), false, true);
      }
    }
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
        scanClass(cls);
        continue;
      }
      scanStmt(item);
    }
    for (const FunctionDecl *fn : this->check.program_.inventory.instances) {
      addFunction(fn);
    }
    for (size_t i = 0; i < this->check.functions_.size(); ++i) {
      scanFunction(this->check.functions_[i]);
    }
  }
}

void ProgramScan::scanStmt(const Stmt *stmt) {
  if (!stmt) {
    return;
  }
  switch (stmt->kind()) {
  case Stmt::Kind::Block:
    for (const Stmt *s : cast<BlockStmt>(stmt)->body) {
      scanStmt(s);
    }
    break;
  case Stmt::Kind::VarDecl: {
    auto *decl = cast<VarDeclStmt>(stmt);
    for (const VarDeclarator &d : decl->decls) {
      declareVar(d.sym, &d, decl->isConst);
      if (!d.init) {
        continue;
      }
      scanExpr(d.init, d.destructured());
      if (this->check.declaring) {
        this->check.inits[d.init] = {&d, decl->isConst};
        if (d.sym && decl->isConst && isConstantTree(d.init)) {
          this->check.constants.insert(d.sym);
        }
      }
    }
    break;
  }
  case Stmt::Kind::Function: {
    auto *fn = cast<FunctionDecl>(stmt);
    if (this->check.declaring) {
      addFunction(fn);
    }
    break;
  }
  case Stmt::Kind::Expr:
    scanExpr(cast<ExprStmt>(stmt)->expr, true);
    break;
  case Stmt::Kind::If: {
    auto *s = cast<IfStmt>(stmt);
    scanExpr(s->cond, true);
    scanStmt(s->thenStmt);
    scanStmt(s->elseStmt);
    break;
  }
  case Stmt::Kind::While:
    scanExpr(cast<WhileStmt>(stmt)->cond, true);
    scanStmt(cast<WhileStmt>(stmt)->body);
    break;
  case Stmt::Kind::DoWhile:
    scanStmt(cast<DoWhileStmt>(stmt)->body);
    scanExpr(cast<DoWhileStmt>(stmt)->cond, true);
    break;
  case Stmt::Kind::For: {
    auto *s = cast<ForStmt>(stmt);
    scanStmt(s->init);
    scanExpr(s->cond, true);
    scanExpr(s->step, true);
    scanStmt(s->body);
    break;
  }
  case Stmt::Kind::ForOf: {
    auto *s = cast<ForOfStmt>(stmt);
    declareVar(s->binding.sym, &s->binding, s->isConst);
    scanExpr(s->iterable, true);
    scanStmt(s->body);
    break;
  }
  case Stmt::Kind::Switch: {
    auto *s = cast<SwitchStmt>(stmt);
    scanExpr(s->discriminant, true);
    for (const SwitchCase &c : s->cases) {
      scanExpr(c.test, true);
      for (const Stmt *body : c.body) {
        scanStmt(body);
      }
    }
    break;
  }
  case Stmt::Kind::Labeled:
    scanStmt(cast<LabeledStmt>(stmt)->body);
    break;
  case Stmt::Kind::Return:
    scanExpr(cast<ReturnStmt>(stmt)->value, false);
    break;
  case Stmt::Kind::Class:
    scanClass(cast<ClassDecl>(stmt));
    break;
  case Stmt::Kind::Break:
  case Stmt::Kind::Continue:
  case Stmt::Kind::Empty:
  case Stmt::Kind::TypeAlias:
  case Stmt::Kind::Enum:
  case Stmt::Kind::Debugger:
    break;
  }
}

void ProgramScan::scanExpr(const Expr *expr, bool consumed) {
  if (!expr) {
    return;
  }
  switch (expr->kind()) {
  case Expr::Kind::IntLit:
  case Expr::Kind::FloatLit:
  case Expr::Kind::BoolLit:
  case Expr::Kind::StringLit:
  case Expr::Kind::Undefined:
  case Expr::Kind::RegexLit:
    break;
  case Expr::Kind::Template:
    for (const Expr *e : cast<TemplateExpr>(expr)->exprs) {
      scanExpr(e, true);
    }
    break;
  case Expr::Kind::ArrayLit:
    for (const ArrayLitExpr::Element &e : cast<ArrayLitExpr>(expr)->elements) {
      scanExpr(e.expr, e.spread);
    }
    break;
  case Expr::Kind::ObjectLit:
    for (const ObjectLitExpr::Property &p : cast<ObjectLitExpr>(expr)->props) {
      scanExpr(p.key(), true);
      scanExpr(p.value, p.spread());
    }
    break;
  case Expr::Kind::Name: {
    auto *name = cast<NameExpr>(expr);
    useVar(name->var(), consumed, false);
    if (!this->check.declaring) {
      if (name->func() && name->func()->decl) {
        this->check.unknownCallers.insert(name->func()->decl);
      }
      if (name->var() && name->var()->function()) {
        this->check.unknownCallers.insert(name->var()->function());
      }
      if (name->var() && name->var()->lambda()) {
        this->check.unknownCallers.insert(name->var()->lambda());
      }
    }
    break;
  }
  case Expr::Kind::Member: {
    auto *member = cast<MemberExpr>(expr);
    scanExpr(member->object, true);
    if (!this->check.declaring && member->method() && member->method()->decl) {
      this->check.unknownCallers.insert(member->method()->decl);
    }
    break;
  }
  case Expr::Kind::Index:
    scanExpr(cast<IndexExpr>(expr)->object, true);
    scanExpr(cast<IndexExpr>(expr)->index, true);
    break;
  case Expr::Kind::Unary:
    scanExpr(cast<UnaryExpr>(expr)->operand, true);
    break;
  case Expr::Kind::Binary: {
    auto *bin = cast<BinaryExpr>(expr);
    bool compares = bin->op == BinaryOp::Eq || bin->op == BinaryOp::Ne;
    bool gives =
        bin->op == BinaryOp::Nullish || bin->op == BinaryOp::LogAnd || bin->op == BinaryOp::LogOr;
    scanExpr(bin->lhs, compares || !gives);
    scanExpr(bin->rhs, compares || !gives);
    break;
  }
  case Expr::Kind::Assign: {
    auto *assign = cast<AssignExpr>(expr);
    if (auto *name = dyn_cast<NameExpr>(assign->target)) {
      useVar(name->var(), true, true);
      if (this->check.declaring && !assign->op) {
        if (auto *bin = dyn_cast<BinaryExpr>(assign->value)) {
          if (auto *lhs = dyn_cast<NameExpr>(OverflowHelpers::stripCasts(bin->lhs));
              lhs && lhs->var() == name->var()) {
            this->check.selfAssigned[bin] = name->var();
          }
        }
      }
    } else {
      scanExpr(assign->target, true);
    }
    scanExpr(assign->value, false);
    break;
  }
  case Expr::Kind::Update: {
    auto *update = cast<UpdateExpr>(expr);
    if (auto *name = dyn_cast<NameExpr>(update->target)) {
      useVar(name->var(), true, true);
    } else {
      scanExpr(update->target, true);
    }
    break;
  }
  case Expr::Kind::Call: {
    auto *call = cast<CallExpr>(expr);
    if (auto *member = dyn_cast<MemberExpr>(call->callee)) {
      scanExpr(member->object, true);
    } else if (auto *name = dyn_cast<NameExpr>(call->callee);
               name && (call->func() || call->builtin() != Builtin::None)) {
      useVar(name->var(), true, false);
    } else {
      scanExpr(call->callee, true);
    }
    std::optional<size_t> callback = OverflowHelpers::callbackArg(call->builtin());
    bool reads = ProgramScan::onlyReads(call->builtin());
    for (size_t i = 0; i < call->args.size(); ++i) {
      const Expr *arg = call->args[i];
      if (callback && *callback == i) {
        if (auto *fe = dyn_cast<FunctionExpr>(OverflowHelpers::stripCasts(arg))) {
          if (this->check.declaring) {
            this->check.inlined.insert(fe->fn);
          }
          if (this->check.declaring && !fe->fn->params.empty() &&
              (call->builtin() == Builtin::ArrReduce ||
               call->builtin() == Builtin::ArrReduceRight)) {
            this->check.accumulators.insert(fe->fn->params[0].sym);
          }
          for (const Param &p : fe->fn->params) {
            declareVar(p.sym);
            if (p.defaultValue()) {
              scanExpr(p.defaultValue(), false);
            }
          }
          scanStmt(fe->fn->body);
          continue;
        }
      }
      scanExpr(arg, reads || call->spreads(i));
    }
    break;
  }
  case Expr::Kind::New: {
    auto *ne = cast<NewExpr>(expr);
    for (const Expr *arg : ne->ctor() ? ne->ctor()->args : ne->args) {
      scanExpr(arg, false);
    }
    break;
  }
  case Expr::Kind::Conditional: {
    auto *c = cast<ConditionalExpr>(expr);
    scanExpr(c->cond, true);
    scanExpr(c->thenExpr, consumed);
    scanExpr(c->elseExpr, consumed);
    break;
  }
  case Expr::Kind::Cast: {
    auto *conv = cast<CastExpr>(expr);
    scanExpr(conv->operand, consumed || (conv->type && conv->type->isString()));
    break;
  }
  case Expr::Kind::Sequence: {
    auto &exprs = cast<SequenceExpr>(expr)->exprs;
    for (size_t i = 0; i < exprs.size(); ++i) {
      scanExpr(exprs[i], i + 1 < exprs.size() || consumed);
    }
    break;
  }
  case Expr::Kind::OptionalChain:
    scanExpr(cast<OptionalChainExpr>(expr)->chain, consumed);
    break;
  case Expr::Kind::Function: {
    const FunctionDecl *fn = cast<FunctionExpr>(expr)->fn;
    if (this->check.declaring && !this->check.inlined.contains(fn)) {
      addFunction(fn);
    }
    break;
  }
  case Expr::Kind::Yield:
    scanExpr(cast<YieldExpr>(expr)->value, false);
    break;
  case Expr::Kind::Await:
    scanExpr(cast<AwaitExpr>(expr)->value, false);
    break;
  case Expr::Kind::Class:
    break;
  }
}

bool ProgramScan::isConstantTree(const Expr *expr) const {
  expr = OverflowHelpers::stripCasts(expr);
  if (!expr) {
    return false;
  }
  if (isa<IntLitExpr>(expr) || isa<FloatLitExpr>(expr)) {
    return true;
  }
  if (auto *unary = dyn_cast<UnaryExpr>(expr)) {
    return unary->op != UnaryOp::Not && isConstantTree(unary->operand);
  }
  if (auto *bin = dyn_cast<BinaryExpr>(expr)) {
    return isConstantTree(bin->lhs) && isConstantTree(bin->rhs);
  }
  if (auto *name = dyn_cast<NameExpr>(expr)) {
    return name->var() && this->check.constants.contains(name->var());
  }

  return false;
}

}
