#include "compiler/whole_program/flow_stmts/flow_stmts.h"
#include "compiler/whole_program/flow_exprs/flow_exprs.h"
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
using llvm::isa;
using llvm::dyn_cast_or_null;

namespace tz {

FlowStmts::FlowStmts(Flows &flows) : flows_(flows) {}

void FlowStmts::function(FunctionDecl *fn) {
  if (!fn || !flows_.walked_.insert(fn).second) {
    return;
  }
  flows_.functions_.push_back(fn);
  for (Param &p : fn->params) {
    if (p.defaultValue) {
      Refs value = flows_.flowExprs().expr(p.defaultValue);
      if (p.sym) {
        flows_.flow(value, flows_.var(p.sym));
      }
    }
  }
  if (fn->body) {
    stmt(fn->body);
  }
  flows_.functions_.pop_back();
}

void FlowStmts::classDecl(ClassDecl *cls) {
  if (!cls->isEmitted()) {
    return;
  }
  for (FunctionDecl *fn : cls->constructors) {
    function(fn);
  }
  for (FunctionDecl *fn : cls->methods) {
    if (fn->typeParams.empty()) {
      function(fn);
    }
  }
  for (FunctionDecl *fn : cls->methodInstances) {
    function(fn);
  }
  if (cls->init) {
    function(cls->init);
  }
  for (ClassField &f : cls->fields) {
    if (f.init) {
      flows_.flow(flows_.flowExprs().expr(f.init), f.var ? flows_.var(f.var) : flows_.fieldNode(f.name));
    }
  }
  for (Stmt *s : cls->decorations) {
    stmt(s);
  }
}

void FlowStmts::stmt(Stmt *s) {
  if (!s) {
    return;
  }
  switch (s->kind()) {
  case Stmt::Kind::Block:
    for (Stmt *child : llvm::cast<BlockStmt>(s)->body) {
      stmt(child);
    }
    break;
  case Stmt::Kind::VarDecl:
    for (VarDeclarator &d : llvm::cast<VarDeclStmt>(s)->decls) {
      if (d.init) {
        Refs value = flows_.flowExprs().expr(d.init);
        if (d.sym) {
          flows_.flow(value, flows_.var(d.sym));
        }
      }
    }
    break;
  case Stmt::Kind::Function: {
    auto *fn = llvm::cast<FunctionDecl>(s);
    if (fn->typeParams.empty()) {
      function(fn);
    }
    break;
  }
  case Stmt::Kind::Expr:
    flows_.flowExprs().expr(llvm::cast<ExprStmt>(s)->expr);
    break;
  case Stmt::Kind::If: {
    auto *i = llvm::cast<IfStmt>(s);
    flows_.flowExprs().expr(i->cond);
    stmt(i->thenStmt);
    stmt(i->elseStmt);
    break;
  }
  case Stmt::Kind::While:
    flows_.flowExprs().expr(llvm::cast<WhileStmt>(s)->cond);
    stmt(llvm::cast<WhileStmt>(s)->body);
    break;
  case Stmt::Kind::DoWhile:
    flows_.flowExprs().expr(llvm::cast<DoWhileStmt>(s)->cond);
    stmt(llvm::cast<DoWhileStmt>(s)->body);
    break;
  case Stmt::Kind::For: {
    auto *f = llvm::cast<ForStmt>(s);
    stmt(f->init);
    if (f->cond) {
      flows_.flowExprs().expr(f->cond);
    }
    if (f->step) {
      flows_.flowExprs().expr(f->step);
    }
    stmt(f->body);
    break;
  }
  case Stmt::Kind::ForOf: {
    auto *f = llvm::cast<ForOfStmt>(s);
    Refs iterable = flows_.flowExprs().expr(f->iterable);
    if (!f->keys && f->binding.sym) {
      Refs element = shifted(iterable, 1);
      if (int held = flows_.contents(f->iterable->type); held >= 0) {
        element.push_back({held, 0, 0});
      }
      flows_.flow(element, flows_.var(f->binding.sym));
    }
    stmt(f->body);
    break;
  }
  case Stmt::Kind::Switch: {
    auto *sw = llvm::cast<SwitchStmt>(s);
    flows_.flowExprs().expr(sw->discriminant);
    for (SwitchCase &c : sw->cases) {
      if (c.test) {
        flows_.flowExprs().expr(c.test);
      }
      for (Stmt *body : c.body) {
        stmt(body);
      }
    }
    break;
  }
  case Stmt::Kind::Labeled:
    stmt(llvm::cast<LabeledStmt>(s)->body);
    break;
  case Stmt::Kind::Return:
    if (Expr *value = llvm::cast<ReturnStmt>(s)->value) {
      Refs refs = flows_.flowExprs().expr(value);
      if (!flows_.functions_.empty()) {
        flows_.flow(refs, flows_.returned(flows_.functions_.back()));
      } else {
        flows_.escape(refs);
      }
    }
    break;
  case Stmt::Kind::Throw:
    flows_.flow(flows_.flowExprs().expr(llvm::cast<ThrowStmt>(s)->value), flows_.thrown_);
    break;
  case Stmt::Kind::Try: {
    auto *t = llvm::cast<TryStmt>(s);
    stmt(t->block);
    if (t->handler && t->param.sym) {
      flows_.flow({{flows_.thrown_, 0, 0}}, flows_.var(t->param.sym));
    }
    stmt(t->handler);
    stmt(t->finalizer);
    break;
  }
  case Stmt::Kind::Class:
    classDecl(llvm::cast<ClassDecl>(s));
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

const VarSymbol *FlowStmts::currentThis() const {
  for (auto it = flows_.functions_.rbegin(); it != flows_.functions_.rend(); ++it) {
    if ((*it)->thisVar) {
      return (*it)->thisVar;
    }
  }

  return nullptr;
}

Refs FlowStmts::name(const NameExpr *n) {
  if (n->var) {
    return {{flows_.var(n->var), 0, 0}};
  }
  if ((n->name == "this" || n->name == "super") && currentThis()) {
    return {{flows_.var(currentThis()), 0, 0}};
  }

  return {};
}

void FlowStmts::recordWrite(const Refs &target, bool element, llvm::StringRef field, const Expr *value,
                 const Type *through, llvm::SMLoc loc) {
  flows_.writes.push_back(
      {target, element, field.str(), value ? ownType(value) : nullptr, through, loc});
}

void FlowStmts::assignTo(Expr *target, const Refs &value, const Expr *valueExpr, bool compound) {
  if (auto *n = llvm::dyn_cast<NameExpr>(target)) {
    if (n->var) {
      flows_.flow(value, flows_.var(n->var));
    }
    return;
  }
  if (auto *member = llvm::dyn_cast<MemberExpr>(target)) {
    Refs object = flows_.flowExprs().expr(member->object);
    recordWrite(object, false, member->name, compound ? nullptr : valueExpr, member->object->type,
                target->loc());
    flows_.flow(value, flows_.fieldNode(member->name));
    return;
  }
  if (auto *index = llvm::dyn_cast<IndexExpr>(target)) {
    Refs object = flows_.flowExprs().expr(index->object);
    flows_.flowExprs().expr(index->index);
    const Type *objectType = index->object->type ? index->object->type->nonOptional() : nullptr;
    if (objectType && objectType->isArray()) {
      recordWrite(object, true, "", compound ? nullptr : valueExpr, objectType, target->loc());
      flows_.flow(value, flows_.contents(objectType));
    } else if (objectType && objectType->isDict()) {
      flows_.flow(value, flows_.contents(objectType));
    } else {
      // A field by its key: known in a copy, or any.
      std::string field = !index->field.empty() ? index->field.str() : "*";
      if (auto *key = llvm::dyn_cast<IntLitExpr>(index->index);
          key && objectType && objectType->isTuple()) {
        field = std::to_string(key->value);
      }
      recordWrite(object, false, field, compound ? nullptr : valueExpr, objectType,
                  target->loc());
      flows_.flow(value, field == "*" ? flows_.unknown_ : flows_.fieldNode(field));
    }
    return;
  }
  flows_.flowExprs().expr(target);
  flows_.escape(value);
}

} // namespace tz
