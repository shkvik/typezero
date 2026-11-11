#include "compiler/interprocedural/flow_stmts/flow_stmts.h"
#include "compiler/interprocedural/flow_exprs/flow_exprs.h"
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

void FlowStmts::function(FunctionDecl *fn) {
  if (!fn || this->builder.isWalked(fn)) {
    return;
  }
  this->builder.markWalked(fn);
  this->builder.enterFunction(fn);
  for (Param &p : fn->params) {
    if (p.defaultValue()) {
      Refs value = this->builder.getFlowExprs().expr(p.defaultValue());
      if (p.sym) {
        this->builder.addFlow(value, this->builder.varNode(p.sym));
      }
    }
  }
  if (fn->body) {
    this->stmt(fn->body);
  }
  this->builder.leaveFunction();
}

void FlowStmts::classDecl(ClassDecl *cls) {
  if (!cls->isEmitted()) {
    return;
  }
  for (FunctionDecl *fn : cls->constructors) {
    this->function(fn);
  }
  for (FunctionDecl *fn : cls->methods) {
    if (fn->typeParams.empty()) {
      this->function(fn);
    }
  }
  for (FunctionDecl *fn : cls->methodInstances) {
    this->function(fn);
  }
  if (cls->init) {
    this->function(cls->init);
  }
  for (ClassField &f : cls->fields) {
    if (f.init) {
      this->builder.addFlow(this->builder.getFlowExprs().expr(f.init),
                            f.var() ? this->builder.varNode(f.var())
                                    : this->builder.fieldNode(f.name));
    }
  }
  for (Stmt *s : cls->decorations) {
    this->stmt(s);
  }
}

void FlowStmts::stmt(Stmt *s) {
  if (!s) {
    return;
  }
  switch (s->kind()) {
  case Stmt::Kind::Block:
    for (Stmt *child : llvm::cast<BlockStmt>(s)->body) {
      this->stmt(child);
    }
    break;
  case Stmt::Kind::VarDecl:
    for (VarDeclarator &d : llvm::cast<VarDeclStmt>(s)->decls) {
      if (d.init) {
        Refs value = this->builder.getFlowExprs().expr(d.init);
        if (d.sym) {
          this->builder.addFlow(value, this->builder.varNode(d.sym));
        }
      }
    }
    break;
  case Stmt::Kind::Function: {
    auto *fn = llvm::cast<FunctionDecl>(s);
    if (fn->typeParams.empty()) {
      this->function(fn);
    }
    break;
  }
  case Stmt::Kind::Expr:
    this->builder.getFlowExprs().expr(llvm::cast<ExprStmt>(s)->expr);
    break;
  case Stmt::Kind::If: {
    auto *i = llvm::cast<IfStmt>(s);
    this->builder.getFlowExprs().expr(i->cond);
    this->stmt(i->thenStmt);
    this->stmt(i->elseStmt);
    break;
  }
  case Stmt::Kind::While:
    this->builder.getFlowExprs().expr(llvm::cast<WhileStmt>(s)->cond);
    this->stmt(llvm::cast<WhileStmt>(s)->body);
    break;
  case Stmt::Kind::DoWhile:
    this->builder.getFlowExprs().expr(llvm::cast<DoWhileStmt>(s)->cond);
    this->stmt(llvm::cast<DoWhileStmt>(s)->body);
    break;
  case Stmt::Kind::For: {
    auto *f = llvm::cast<ForStmt>(s);
    this->stmt(f->init);
    if (f->cond) {
      this->builder.getFlowExprs().expr(f->cond);
    }
    if (f->step) {
      this->builder.getFlowExprs().expr(f->step);
    }
    this->stmt(f->body);
    break;
  }
  case Stmt::Kind::ForOf: {
    auto *f = llvm::cast<ForOfStmt>(s);
    Refs iterable = this->builder.getFlowExprs().expr(f->iterable);
    if (!f->keys && f->binding.sym) {
      Refs element = FlowGraphBuilder::shifted(iterable, 1);
      if (int held = this->builder.contentsNode(f->iterable->type); held >= 0) {
        element.push_back({held, 0, 0});
      }
      this->builder.addFlow(element, this->builder.varNode(f->binding.sym));
    }
    this->stmt(f->body);
    break;
  }
  case Stmt::Kind::Switch: {
    auto *sw = llvm::cast<SwitchStmt>(s);
    this->builder.getFlowExprs().expr(sw->discriminant);
    for (SwitchCase &c : sw->cases) {
      if (c.test) {
        this->builder.getFlowExprs().expr(c.test);
      }
      for (Stmt *body : c.body) {
        this->stmt(body);
      }
    }
    break;
  }
  case Stmt::Kind::Labeled:
    this->stmt(llvm::cast<LabeledStmt>(s)->body);
    break;
  case Stmt::Kind::Return:
    if (Expr *value = llvm::cast<ReturnStmt>(s)->value) {
      Refs refs = this->builder.getFlowExprs().expr(value);
      if (this->builder.isInsideFunction()) {
        this->builder.addFlow(refs, this->builder.returnNode(this->builder.getCurrentFunction()));
      } else {
        this->builder.escape(refs);
      }
    }
    break;
  case Stmt::Kind::Class:
    this->classDecl(llvm::cast<ClassDecl>(s));
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

void FlowStmts::assignTo(Expr *target, const Refs &value, const Expr *valueExpr, bool compound) {
  if (auto *n = llvm::dyn_cast<NameExpr>(target)) {
    if (n->var()) {
      this->builder.addFlow(value, this->builder.varNode(n->var()));
    }
    return;
  }
  if (auto *member = llvm::dyn_cast<MemberExpr>(target)) {
    Refs object = this->builder.getFlowExprs().expr(member->object);
    this->recordWrite(object, false, member->name, compound ? nullptr : valueExpr,
                      member->object->type, target->loc());
    this->builder.addFlow(value, this->builder.fieldNode(member->name));
    return;
  }
  if (auto *index = llvm::dyn_cast<IndexExpr>(target)) {
    Refs object = this->builder.getFlowExprs().expr(index->object);
    this->builder.getFlowExprs().expr(index->index);
    const Type *objectType = index->object->type ? index->object->type->nonOptional() : nullptr;
    if (objectType && objectType->isArray()) {
      this->recordWrite(object, true, "", compound ? nullptr : valueExpr, objectType,
                        target->loc());
      this->builder.addFlow(value, this->builder.contentsNode(objectType));
    } else if (objectType && objectType->isDict()) {
      this->builder.addFlow(value, this->builder.contentsNode(objectType));
    } else {
      std::string field = !index->field().empty() ? index->field().str() : "*";
      if (auto *key = llvm::dyn_cast<IntLitExpr>(index->index);
          key && objectType && objectType->isTuple()) {
        field = std::to_string(key->value);
      }
      this->recordWrite(object, false, field, compound ? nullptr : valueExpr, objectType,
                        target->loc());
      this->builder.addFlow(value, field == "*" ? this->builder.getUnknownNode()
                                                : this->builder.fieldNode(field));
    }
    return;
  }
  this->builder.getFlowExprs().expr(target);
  this->builder.escape(value);
}

}
