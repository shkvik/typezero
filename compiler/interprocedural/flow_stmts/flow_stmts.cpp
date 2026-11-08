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

FlowStmts::FlowStmts(Flows &owner)
    : flows(owner) {}

void FlowStmts::function(FunctionDecl *fn) {
  if (!fn || this->flows.isWalked(fn)) {
    return;
  }
  this->flows.markWalked(fn);
  this->flows.enterFunction(fn);
  for (Param &p : fn->params) {
    if (p.defaultValue()) {
      Refs value = this->flows.getFlowExprs().expr(p.defaultValue());
      if (p.sym) {
        this->flows.addFlow(value, this->flows.varNode(p.sym));
      }
    }
  }
  if (fn->body) {
    this->stmt(fn->body);
  }
  this->flows.leaveFunction();
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
      this->flows.addFlow(this->flows.getFlowExprs().expr(f.init),
                          f.var() ? this->flows.varNode(f.var()) : this->flows.fieldNode(f.name));
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
        Refs value = this->flows.getFlowExprs().expr(d.init);
        if (d.sym) {
          this->flows.addFlow(value, this->flows.varNode(d.sym));
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
    this->flows.getFlowExprs().expr(llvm::cast<ExprStmt>(s)->expr);
    break;
  case Stmt::Kind::If: {
    auto *i = llvm::cast<IfStmt>(s);
    this->flows.getFlowExprs().expr(i->cond);
    this->stmt(i->thenStmt);
    this->stmt(i->elseStmt);
    break;
  }
  case Stmt::Kind::While:
    this->flows.getFlowExprs().expr(llvm::cast<WhileStmt>(s)->cond);
    this->stmt(llvm::cast<WhileStmt>(s)->body);
    break;
  case Stmt::Kind::DoWhile:
    this->flows.getFlowExprs().expr(llvm::cast<DoWhileStmt>(s)->cond);
    this->stmt(llvm::cast<DoWhileStmt>(s)->body);
    break;
  case Stmt::Kind::For: {
    auto *f = llvm::cast<ForStmt>(s);
    this->stmt(f->init);
    if (f->cond) {
      this->flows.getFlowExprs().expr(f->cond);
    }
    if (f->step) {
      this->flows.getFlowExprs().expr(f->step);
    }
    this->stmt(f->body);
    break;
  }
  case Stmt::Kind::ForOf: {
    auto *f = llvm::cast<ForOfStmt>(s);
    Refs iterable = this->flows.getFlowExprs().expr(f->iterable);
    if (!f->keys && f->binding.sym) {
      Refs element = Flows::shifted(iterable, 1);
      if (int held = this->flows.contentsNode(f->iterable->type); held >= 0) {
        element.push_back({held, 0, 0});
      }
      this->flows.addFlow(element, this->flows.varNode(f->binding.sym));
    }
    this->stmt(f->body);
    break;
  }
  case Stmt::Kind::Switch: {
    auto *sw = llvm::cast<SwitchStmt>(s);
    this->flows.getFlowExprs().expr(sw->discriminant);
    for (SwitchCase &c : sw->cases) {
      if (c.test) {
        this->flows.getFlowExprs().expr(c.test);
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
      Refs refs = this->flows.getFlowExprs().expr(value);
      if (this->flows.isInsideFunction()) {
        this->flows.addFlow(refs, this->flows.returnNode(this->flows.getCurrentFunction()));
      } else {
        this->flows.escape(refs);
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
    return {{this->flows.varNode(n->var()), 0, 0}};
  }
  if ((n->name == "this" || n->name == "super") && this->flows.findCurrentThis()) {
    return {{this->flows.varNode(this->flows.findCurrentThis()), 0, 0}};
  }

  return {};
}

void FlowStmts::recordWrite(const Refs &target, bool element, llvm::StringRef field,
                            const Expr *value, const Type *through, llvm::SMLoc loc) {
  this->flows.addWrite(
      {target, element, field.str(), value ? Flows::ownType(value) : nullptr, through, loc});
}

void FlowStmts::assignTo(Expr *target, const Refs &value, const Expr *valueExpr, bool compound) {
  if (auto *n = llvm::dyn_cast<NameExpr>(target)) {
    if (n->var()) {
      this->flows.addFlow(value, this->flows.varNode(n->var()));
    }
    return;
  }
  if (auto *member = llvm::dyn_cast<MemberExpr>(target)) {
    Refs object = this->flows.getFlowExprs().expr(member->object);
    this->recordWrite(object, false, member->name, compound ? nullptr : valueExpr,
                      member->object->type, target->loc());
    this->flows.addFlow(value, this->flows.fieldNode(member->name));
    return;
  }
  if (auto *index = llvm::dyn_cast<IndexExpr>(target)) {
    Refs object = this->flows.getFlowExprs().expr(index->object);
    this->flows.getFlowExprs().expr(index->index);
    const Type *objectType = index->object->type ? index->object->type->nonOptional() : nullptr;
    if (objectType && objectType->isArray()) {
      this->recordWrite(object, true, "", compound ? nullptr : valueExpr, objectType,
                        target->loc());
      this->flows.addFlow(value, this->flows.contentsNode(objectType));
    } else if (objectType && objectType->isDict()) {
      this->flows.addFlow(value, this->flows.contentsNode(objectType));
    } else {
      std::string field = !index->field().empty() ? index->field().str() : "*";
      if (auto *key = llvm::dyn_cast<IntLitExpr>(index->index);
          key && objectType && objectType->isTuple()) {
        field = std::to_string(key->value);
      }
      this->recordWrite(object, false, field, compound ? nullptr : valueExpr, objectType,
                        target->loc());
      this->flows.addFlow(value, field == "*" ? this->flows.getUnknownNode()
                                              : this->flows.fieldNode(field));
    }
    return;
  }
  this->flows.getFlowExprs().expr(target);
  this->flows.escape(value);
}

}
