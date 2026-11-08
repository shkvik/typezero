#include "compiler/interprocedural/unused/unused.h"
#include "compiler/program/ast_walker/ast_walker.h"
#include "llvm/ADT/STLExtras.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

void Unused::findUnused(SourceFile &program) {
  if (program.modules.size() < 2) {
    return;
  }
  llvm::SmallPtrSet<const FunctionDecl *, 16> used;
  std::vector<const FunctionDecl *> queue;
  auto reach = [&](const Expr *e) {
    auto *call = dyn_cast<CallExpr>(e);
    auto *name = dyn_cast<NameExpr>(e);
    const FuncSymbol *fn = nullptr;
    if (call) {
      fn = call->func();
    } else if (name) {
      fn = name->func();
    }
    bool reached = fn && fn->decl && used.insert(fn->decl).second;
    if (reached) {
      queue.push_back(fn->decl);
    }
  };
  for (Module *module : program.modules) {
    for (Stmt *item : module->file.items) {
      if (module->isEntry || !isa<FunctionDecl>(item)) {
        AstWalker::visitExprs(item, reach);
      }
    }
  }
  while (!queue.empty()) {
    const FunctionDecl *fn = queue.back();
    queue.pop_back();
    AstWalker::visitExprs(fn, reach);
  }
  for (Module *module : program.modules) {
    if (module->isEntry) {
      continue;
    }
    for (Stmt *item : module->file.items) {
      auto *fn = dyn_cast<FunctionDecl>(item);
      if (fn && !used.contains(fn)) {
        program.unusedFunctions.insert(fn);
      }
    }
  }
}

}
