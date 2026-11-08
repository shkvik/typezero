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

StoreStmts::StoreStmts(StoreScanner &scanner)
    : scanner(scanner) {}

void StoreStmts::stmt(const Stmt *stmt) {
  if (!stmt) {
    return;
  }
  switch (stmt->kind()) {
  case Stmt::Kind::Block: {
    auto *block = llvm::cast<BlockStmt>(stmt);
    if (NestedGroup *group = block->group) {
      for (const VarSymbol *v : group->captures) {
        this->scanner.edge(this->scanner.stored(), this->scanner.var(v));
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
        this->scanner.flowInto(this->scanner.var(d.sym), this->scanner.storeExprs().expr(d.init));
      } else {
        this->scanner.storeExprs().expr(d.init);
      }
    }
    break;
  case Stmt::Kind::Function: {
    auto *fn = llvm::cast<FunctionDecl>(stmt);
    this->scanner.function(fn);
    break;
  }
  case Stmt::Kind::Expr:
    this->scanner.storeExprs().expr(llvm::cast<ExprStmt>(stmt)->expr);
    break;
  case Stmt::Kind::If: {
    auto *s = llvm::cast<IfStmt>(stmt);
    this->scanner.storeExprs().expr(s->cond);
    this->stmt(s->thenStmt);
    this->stmt(s->elseStmt);
    break;
  }
  case Stmt::Kind::While:
    this->scanner.storeExprs().expr(llvm::cast<WhileStmt>(stmt)->cond);
    this->stmt(llvm::cast<WhileStmt>(stmt)->body);
    break;
  case Stmt::Kind::DoWhile:
    this->stmt(llvm::cast<DoWhileStmt>(stmt)->body);
    this->scanner.storeExprs().expr(llvm::cast<DoWhileStmt>(stmt)->cond);
    break;
  case Stmt::Kind::For: {
    auto *s = llvm::cast<ForStmt>(stmt);
    this->stmt(s->init);
    this->scanner.storeExprs().expr(s->cond);
    this->scanner.storeExprs().expr(s->step);
    this->stmt(s->body);
    break;
  }
  case Stmt::Kind::ForOf: {
    auto *s = llvm::cast<ForOfStmt>(stmt);
    StoreScanner::Sources iterable = this->scanner.storeExprs().expr(s->iterable);
    if (s->iterable->type && s->iterable->type->isIterator()) {
      this->scanner.store(iterable);
    }
    if (s->binding.sym) {
      this->scanner.var(s->binding.sym);
    }
    this->stmt(s->body);
    break;
  }
  case Stmt::Kind::Switch: {
    auto *s = llvm::cast<SwitchStmt>(stmt);
    this->scanner.storeExprs().expr(s->discriminant);
    for (const SwitchCase &c : s->cases) {
      this->scanner.storeExprs().expr(c.test);
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
    StoreScanner::Sources value =
        this->scanner.storeExprs().expr(llvm::cast<ReturnStmt>(stmt)->value);
    if (!this->scanner.current || this->scanner.current->isGenerator ||
        this->scanner.current->isAsync) {
      this->scanner.store(value);
    } else {
      this->scanner.flowInto(this->scanner.result(this->scanner.current), value);
    }
    break;
  }
  case Stmt::Kind::Class: {
    auto *cls = llvm::cast<ClassDecl>(stmt);
    for (const ClassField &f : cls->fields) {
      if (f.isStatic() && f.var()) {
        this->scanner.flowInto(this->scanner.var(f.var()), this->scanner.storeExprs().expr(f.init));
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
  if (!fn || fn->impl()) {
    for (const StoreScanner::Sources &a : args) {
      this->scanner.store(a);
    }
    return;
  }
  for (size_t i = 0; i < args.size(); ++i) {
    bool rest = fn->sym && fn->sym->hasRest && i + 1 >= fn->params.size();
    if (i < fn->params.size() && !rest && fn->params[i].sym) {
      this->scanner.flowInto(this->scanner.var(fn->params[i].sym), args[i]);
    } else {
      this->scanner.store(args[i]);
    }
  }
}

}
