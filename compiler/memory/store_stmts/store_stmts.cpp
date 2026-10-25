#include "compiler/memory/store_stmts/store_stmts.h"
#include "compiler/memory/store_exprs/store_exprs.h"
#include "llvm/ADT/STLExtras.h"
#include <functional>
#include <map>
#include <set>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

StoreStmts::StoreStmts(StoreScanner &scanner) : scanner_(scanner) {}

void StoreStmts::stmt(const Stmt *stmt) {
  if (!stmt) {
    return;
  }
  switch (stmt->kind()) {
  case Stmt::Kind::Block: {
    auto *block = llvm::cast<BlockStmt>(stmt);
    // The functions of a block share an environment, which holds what
    // they capture.
    if (NestedGroup *group = block->group) {
      for (const VarSymbol *v : group->captures) {
        scanner_.edge(scanner_.stored(), scanner_.var(v));
      }
    }
    for (const Stmt *s : block->body) {
      this->stmt(s);
    }
    break;
  }
  case Stmt::Kind::VarDecl:
    for (const VarDeclarator &d : llvm::cast<VarDeclStmt>(stmt)->decls) {
      if (d.sym) {
        scanner_.flowInto(scanner_.var(d.sym), scanner_.storeExprs().expr(d.init));
      } else {
        scanner_.storeExprs().expr(d.init);
      }
    }
    break;
  case Stmt::Kind::Function: {
    auto *fn = llvm::cast<FunctionDecl>(stmt);
    scanner_.function(fn);
    break;
  }
  case Stmt::Kind::Expr:
    scanner_.storeExprs().expr(llvm::cast<ExprStmt>(stmt)->expr);
    break;
  case Stmt::Kind::If: {
    auto *s = llvm::cast<IfStmt>(stmt);
    scanner_.storeExprs().expr(s->cond);
    this->stmt(s->thenStmt);
    this->stmt(s->elseStmt);
    break;
  }
  case Stmt::Kind::While:
    scanner_.storeExprs().expr(llvm::cast<WhileStmt>(stmt)->cond);
    this->stmt(llvm::cast<WhileStmt>(stmt)->body);
    break;
  case Stmt::Kind::DoWhile:
    this->stmt(llvm::cast<DoWhileStmt>(stmt)->body);
    scanner_.storeExprs().expr(llvm::cast<DoWhileStmt>(stmt)->cond);
    break;
  case Stmt::Kind::For: {
    auto *s = llvm::cast<ForStmt>(stmt);
    this->stmt(s->init);
    scanner_.storeExprs().expr(s->cond);
    scanner_.storeExprs().expr(s->step);
    this->stmt(s->body);
    break;
  }
  case Stmt::Kind::ForOf: {
    auto *s = llvm::cast<ForOfStmt>(stmt);
    // The loop reads the elements; an iterator it walks may hold it.
    StoreScanner::Sources iterable = scanner_.storeExprs().expr(s->iterable);
    if (s->iterable->type && s->iterable->type->isIterator()) {
      scanner_.store(iterable);
    }
    if (s->binding.sym) {
      scanner_.var(s->binding.sym);
    }
    this->stmt(s->body);
    break;
  }
  case Stmt::Kind::Switch: {
    auto *s = llvm::cast<SwitchStmt>(stmt);
    scanner_.storeExprs().expr(s->discriminant);
    for (const SwitchCase &c : s->cases) {
      scanner_.storeExprs().expr(c.test);
      for (const Stmt *body : c.body) {
        this->stmt(body);
      }
    }
    break;
  }
  case Stmt::Kind::Labeled:
    this->stmt(llvm::cast<LabeledStmt>(stmt)->body);
    break;
  case Stmt::Kind::Return: {
    StoreScanner::Sources value = scanner_.storeExprs().expr(llvm::cast<ReturnStmt>(stmt)->value);
    // A generator's or an async function's result is held by its object.
    if (!scanner_.current_ || scanner_.current_->isGenerator || scanner_.current_->isAsync) {
      scanner_.store(value);
    } else {
      scanner_.flowInto(scanner_.result(scanner_.current_), value);
    }
    break;
  }
  case Stmt::Kind::Throw:
    scanner_.store(scanner_.storeExprs().expr(llvm::cast<ThrowStmt>(stmt)->value));
    break;
  case Stmt::Kind::Try: {
    auto *s = llvm::cast<TryStmt>(stmt);
    this->stmt(s->block);
    if (s->param.sym) {
      scanner_.var(s->param.sym);
    }
    this->stmt(s->handler);
    this->stmt(s->finalizer);
    break;
  }
  case Stmt::Kind::Class: {
    auto *cls = llvm::cast<ClassDecl>(stmt);
    for (const ClassField &f : cls->fields) {
      if (f.isStatic && f.var) {
        scanner_.flowInto(scanner_.var(f.var), scanner_.storeExprs().expr(f.init));
      }
    }
    for (const Stmt *s : cls->decorations) {
      this->stmt(s);
    }
    break;
  }
  default:
    break;
  }
}

void StoreStmts::bindArgs(const FunctionDecl *fn, const std::vector<StoreScanner::Sources> &args) {
  if (!fn || fn->impl) {
    for (const StoreScanner::Sources &a : args) {
      scanner_.store(a);
    }
    return;
  }
  for (size_t i = 0; i < args.size(); ++i) {
    bool rest = fn->sym && fn->sym->hasRest && i + 1 >= fn->params.size();
    if (i < fn->params.size() && !rest && fn->params[i].sym) {
      scanner_.flowInto(scanner_.var(fn->params[i].sym), args[i]);
    } else {
      scanner_.store(args[i]);
    }
  }
}

} // namespace tz
