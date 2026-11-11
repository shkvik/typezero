#include "compiler/interprocedural/unused_functions/unused_functions.h"
#include "compiler/program/ast_walker/ast_walker.h"
#include "llvm/ADT/STLExtras.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

UnusedFunctions::UnusedFunctions(SourceFile &checked)
    : file(checked) {}

llvm::SmallPtrSet<const FunctionDecl *, 8> UnusedFunctions::findUnused() {
  auto unused = llvm::SmallPtrSet<const FunctionDecl *, 8>();
  if (this->file.modules.size() < 2) {
    return unused;
  }
  this->reachFromRoots();
  this->reachFromQueue();

  for (Module *module : this->file.modules) {
    if (module->isEntry) {
      continue;
    }
    for (Stmt *item : module->file.items) {
      auto *fn = dyn_cast<FunctionDecl>(item);
      if (fn != nullptr && !this->used.contains(fn)) {
        unused.insert(fn);
      }
    }
  }

  return unused;
}

void UnusedFunctions::reachFromRoots() {
  for (Module *module : this->file.modules) {
    for (Stmt *item : module->file.items) {
      bool isRoot = module->isEntry || !isa<FunctionDecl>(item);
      if (!isRoot) {
        continue;
      }
      for (const Expr *e : AstWalker::exprsIn(item, true)) {
        this->reach(e);
      }
    }
  }
}

void UnusedFunctions::reachFromQueue() {
  while (!this->queue.empty()) {
    const auto *fn = this->queue.back();
    this->queue.pop_back();
    for (const Expr *e : AstWalker::exprsIn(fn, true)) {
      this->reach(e);
    }
  }
}

void UnusedFunctions::reach(const Expr *e) {
  const FuncSymbol *fn = nullptr;
  const auto *call = dyn_cast<CallExpr>(e);
  const auto *name = dyn_cast<NameExpr>(e);
  if (call != nullptr) {
    fn = call->func();
  } else if (name != nullptr) {
    fn = name->func();
  }
  if (fn == nullptr || fn->decl == nullptr) {
    return;
  }
  auto [_, isInserted] = this->used.insert(fn->decl);
  if (isInserted) {
    this->queue.push_back(fn->decl);
  }
}

}
