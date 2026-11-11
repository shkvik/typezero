#include "compiler/interprocedural/wide_writes/flow_graph/flow_stmts/flow_stmts.h"
#include "compiler/interprocedural/wide_writes/flow_graph/flow_exprs/flow_exprs.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SourceMgr.h"
#include <cctype>
#include <functional>
#include <map>
#include <set>
#include <tuple>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

FlowStmts::FlowStmts(FlowGraphBuilder &owner)
    : builder(owner) {}

void FlowStmts::startFunction(FunctionDecl *fn, FlowRun &run) {
  if (!fn || this->builder.isWalked(fn)) {
    return;
  }
  this->builder.markWalked(fn);
  this->builder.enterFunction(fn);
  auto parts = std::vector<FlowRun::Step>();
  for (Param &p : fn->params) {
    if (p.defaultValue()) {
      parts.push_back(FlowRun::WalkExpr{p.defaultValue()});
      parts.push_back(FlowRun::BindVar{p.sym});
    }
  }
  if (fn->body) {
    parts.push_back(FlowRun::WalkStmt{fn->body});
  }
  parts.push_back(FlowRun::LeaveFunction{});
  this->builder.pushSteps(run, parts);
}

void FlowStmts::startClass(ClassDecl *cls, FlowRun &run) {
  if (!cls->isEmitted()) {
    return;
  }
  auto parts = std::vector<FlowRun::Step>();
  for (FunctionDecl *fn : cls->constructors) {
    parts.push_back(FlowRun::WalkFunction{fn});
  }
  for (FunctionDecl *fn : cls->methods) {
    if (fn->typeParams.empty()) {
      parts.push_back(FlowRun::WalkFunction{fn});
    }
  }
  for (FunctionDecl *fn : cls->methodInstances) {
    parts.push_back(FlowRun::WalkFunction{fn});
  }
  if (cls->init) {
    parts.push_back(FlowRun::WalkFunction{cls->init});
  }
  for (ClassField &f : cls->fields) {
    if (f.init) {
      parts.push_back(FlowRun::WalkExpr{f.init});
      parts.push_back(FlowRun::BindField{&f});
    }
  }
  for (Stmt *s : cls->decorations) {
    parts.push_back(FlowRun::WalkStmt{s});
  }
  this->builder.pushSteps(run, parts);
}

void FlowStmts::startStmt(Stmt *s, FlowRun &run) {
  if (!s) {
    return;
  }
  auto parts = this->stmtParts(s);
  this->builder.pushSteps(run, parts);
}

std::vector<FlowRun::Step> FlowStmts::stmtParts(Stmt *s) const {
  switch (s->kind()) {
    case Stmt::Kind::Block: {
      auto parts = std::vector<FlowRun::Step>();
      for (Stmt *child : llvm::cast<BlockStmt>(s)->body) {
        parts.push_back(FlowRun::WalkStmt{child});
      }
      return parts;
    }
    case Stmt::Kind::VarDecl:
      return this->declaratorParts(llvm::cast<VarDeclStmt>(s));
    case Stmt::Kind::Function: {
      auto *fn = llvm::cast<FunctionDecl>(s);
      if (!fn->typeParams.empty()) {
        return {};
      }
      return {FlowRun::WalkFunction{fn}};
    }
    case Stmt::Kind::Expr:
      return {FlowRun::WalkExpr{llvm::cast<ExprStmt>(s)->expr}, FlowRun::DropAnswer{}};
    case Stmt::Kind::If: {
      auto *i = llvm::cast<IfStmt>(s);
      return {FlowRun::WalkExpr{i->cond}, FlowRun::DropAnswer{}, FlowRun::WalkStmt{i->thenStmt},
              FlowRun::WalkStmt{i->elseStmt}};
    }
    case Stmt::Kind::While: {
      auto *loop = llvm::cast<WhileStmt>(s);
      return {FlowRun::WalkExpr{loop->cond}, FlowRun::DropAnswer{}, FlowRun::WalkStmt{loop->body}};
    }
    case Stmt::Kind::DoWhile: {
      auto *loop = llvm::cast<DoWhileStmt>(s);
      return {FlowRun::WalkExpr{loop->cond}, FlowRun::DropAnswer{}, FlowRun::WalkStmt{loop->body}};
    }
    case Stmt::Kind::For:
      return this->forParts(llvm::cast<ForStmt>(s));
    case Stmt::Kind::ForOf: {
      auto *loop = llvm::cast<ForOfStmt>(s);
      return {FlowRun::WalkExpr{loop->iterable}, FlowRun::BindForOf{loop},
              FlowRun::WalkStmt{loop->body}};
    }
    case Stmt::Kind::Switch:
      return this->switchParts(llvm::cast<SwitchStmt>(s));
    case Stmt::Kind::Labeled:
      return {FlowRun::WalkStmt{llvm::cast<LabeledStmt>(s)->body}};
    case Stmt::Kind::Return: {
      Expr *value = llvm::cast<ReturnStmt>(s)->value;
      if (!value) {
        return {};
      }
      return {FlowRun::WalkExpr{value}, FlowRun::FinishReturn{}};
    }
    case Stmt::Kind::Class:
      return {FlowRun::WalkClass{llvm::cast<ClassDecl>(s)}};
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

std::vector<FlowRun::Step> FlowStmts::declaratorParts(VarDeclStmt *decl) const {
  auto parts = std::vector<FlowRun::Step>();
  for (VarDeclarator &d : decl->decls) {
    if (d.init) {
      parts.push_back(FlowRun::WalkExpr{d.init});
      parts.push_back(FlowRun::BindVar{d.sym});
    }
  }

  return parts;
}

std::vector<FlowRun::Step> FlowStmts::forParts(ForStmt *loop) const {
  auto parts = std::vector<FlowRun::Step>();
  parts.push_back(FlowRun::WalkStmt{loop->init});
  if (loop->cond) {
    parts.push_back(FlowRun::WalkExpr{loop->cond});
    parts.push_back(FlowRun::DropAnswer{});
  }
  if (loop->step) {
    parts.push_back(FlowRun::WalkExpr{loop->step});
    parts.push_back(FlowRun::DropAnswer{});
  }
  parts.push_back(FlowRun::WalkStmt{loop->body});

  return parts;
}

std::vector<FlowRun::Step> FlowStmts::switchParts(SwitchStmt *choice) const {
  auto parts = std::vector<FlowRun::Step>();
  parts.push_back(FlowRun::WalkExpr{choice->discriminant});
  parts.push_back(FlowRun::DropAnswer{});
  for (SwitchCase &c : choice->cases) {
    if (c.test) {
      parts.push_back(FlowRun::WalkExpr{c.test});
      parts.push_back(FlowRun::DropAnswer{});
    }
    for (Stmt *body : c.body) {
      parts.push_back(FlowRun::WalkStmt{body});
    }
  }

  return parts;
}

void FlowStmts::bindVar(const VarSymbol *var, FlowRun &run) {
  Refs value = this->builder.takeAnswer(run);
  if (var) {
    this->builder.addFlow(value, this->builder.varNode(var));
  }
}

void FlowStmts::bindField(ClassField *field, FlowRun &run) {
  Refs value = this->builder.takeAnswer(run);
  int to =
      field->var() ? this->builder.varNode(field->var()) : this->builder.fieldNode(field->name);
  this->builder.addFlow(value, to);
}

void FlowStmts::bindForOf(ForOfStmt *loop, FlowRun &run) {
  Refs iterable = this->builder.takeAnswer(run);
  if (loop->keys || !loop->binding.sym) {
    return;
  }
  Refs element = FlowGraphBuilder::shifted(iterable, 1);
  int held = this->builder.contentsNode(loop->iterable->type);
  if (held >= 0) {
    element.push_back({held, 0, 0});
  }
  this->builder.addFlow(element, this->builder.varNode(loop->binding.sym));
}

void FlowStmts::finishReturn(FlowRun &run) {
  Refs refs = this->builder.takeAnswer(run);
  if (this->builder.isInsideFunction()) {
    this->builder.addFlow(refs, this->builder.returnNode(this->builder.getCurrentFunction()));
  } else {
    this->builder.escape(refs);
  }
}

Refs FlowStmts::name(const NameExpr *n) {
  if (n->var()) {
    return {{this->builder.varNode(n->var()), 0, 0}};
  }
  if ((n->name == "this" || n->name == "super") && this->builder.findCurrentThis()) {
    return {{this->builder.varNode(this->builder.findCurrentThis()), 0, 0}};
  }

  return {};
}

void FlowStmts::recordWrite(const Refs &target, bool element, llvm::StringRef field,
                            const Expr *value, const Type *through, llvm::SMLoc loc) {
  this->builder.addWrite({target, element, field.str(),
                          value ? FlowGraphBuilder::ownType(value) : nullptr, through, loc});
}

void FlowStmts::startAssign(const FlowRun::AssignTarget &assign, FlowRun &run) {
  auto parts = std::vector<FlowRun::Step>();
  auto *member = llvm::dyn_cast<MemberExpr>(assign.target);
  auto *index = llvm::dyn_cast<IndexExpr>(assign.target);
  if (member) {
    parts.push_back(FlowRun::WalkExpr{member->object});
  } else if (index) {
    parts.push_back(FlowRun::WalkExpr{index->object});
    parts.push_back(FlowRun::WalkExpr{index->index});
  } else if (!llvm::isa<NameExpr>(assign.target)) {
    parts.push_back(FlowRun::WalkExpr{assign.target});
  }
  parts.push_back(FlowRun::FinishAssign{assign});
  this->builder.pushSteps(run, parts);
}

void FlowStmts::finishAssign(const FlowRun::AssignTarget &assign, FlowRun &run) {
  Expr *target = assign.target;
  const Expr *recorded = assign.isCompound ? nullptr : assign.valueExpr;
  if (auto *n = llvm::dyn_cast<NameExpr>(target)) {
    Refs value = this->builder.takeAnswer(run);
    if (n->var()) {
      this->builder.addFlow(value, this->builder.varNode(n->var()));
    }
    return this->finishAssignResult(assign, value, run);
  }
  if (auto *member = llvm::dyn_cast<MemberExpr>(target)) {
    Refs object = this->builder.takeAnswer(run);
    Refs value = this->builder.takeAnswer(run);
    this->recordWrite(object, false, member->name, recorded, member->object->type, target->loc());
    this->builder.addFlow(value, this->builder.fieldNode(member->name));
    return this->finishAssignResult(assign, value, run);
  }
  if (auto *index = llvm::dyn_cast<IndexExpr>(target)) {
    return this->finishIndexAssign(assign, index, run);
  }
  this->builder.takeAnswer(run);
  Refs value = this->builder.takeAnswer(run);
  this->builder.escape(value);

  return this->finishAssignResult(assign, value, run);
}

void FlowStmts::finishIndexAssign(const FlowRun::AssignTarget &assign, IndexExpr *index,
                                  FlowRun &run) {
  this->builder.takeAnswer(run);
  Refs object = this->builder.takeAnswer(run);
  Refs value = this->builder.takeAnswer(run);
  Expr *target = assign.target;
  const Expr *recorded = assign.isCompound ? nullptr : assign.valueExpr;
  const Type *objectType = index->object->type ? index->object->type->nonOptional() : nullptr;
  if (objectType && objectType->isArray()) {
    this->recordWrite(object, true, "", recorded, objectType, target->loc());
    this->builder.addFlow(value, this->builder.contentsNode(objectType));
  } else if (objectType && objectType->isDict()) {
    this->builder.addFlow(value, this->builder.contentsNode(objectType));
  } else {
    std::string field = !index->field().empty() ? index->field().str() : "*";
    if (auto *key = llvm::dyn_cast<IntLitExpr>(index->index);
        key && objectType && objectType->isTuple()) {
      field = std::to_string(key->value);
    }
    this->recordWrite(object, false, field, recorded, objectType, target->loc());
    this->builder.addFlow(value, field == "*" ? this->builder.getUnknownNode()
                                              : this->builder.fieldNode(field));
  }

  return this->finishAssignResult(assign, value, run);
}

void FlowStmts::finishAssignResult(const FlowRun::AssignTarget &assign, const Refs &value,
                                   FlowRun &run) {
  Refs result = assign.isUpdate ? Refs() : value;
  this->builder.pushAnswer(run, assign.whole, result);
}

}
