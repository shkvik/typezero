#include "compiler/whole_program/unused/unused.h"
#include "llvm/ADT/STLExtras.h"
#include <cassert>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::dyn_cast_or_null;

namespace tz {

Unused::Unused(Sema &sema) : sema_(sema) {}

// The entry file is compiled whole; of the files it imports, only the
// functions the program reaches.
void Unused::findUnused(SourceFile &program) {
  if (program.modules.size() < 2) {
    return;
  }
  llvm::SmallPtrSet<const FunctionDecl *, 16> used;
  std::vector<const FunctionDecl *> queue;
  auto reach = [&](const Expr *e) {
    const FuncSymbol *fn = nullptr;
    if (auto *call = dyn_cast<CallExpr>(e)) {
      fn = call->func;
    } else if (auto *name = dyn_cast<NameExpr>(e)) {
      fn = name->func;
    }
    if (fn && fn->decl && used.insert(fn->decl).second) {
      queue.push_back(fn->decl);
    }
  };
  for (Module *module : program.modules) {
    for (Stmt *item : module->file.items) {
      if (module->isEntry || !isa<FunctionDecl>(item)) {
        visitExprs(item, reach);
      }
    }
  }
  while (!queue.empty()) {
    const FunctionDecl *fn = queue.back();
    queue.pop_back();
    visitExprs(fn, reach);
  }
  for (Module *module : program.modules) {
    if (!module->isEntry) {
      for (Stmt *item : module->file.items) {
        if (auto *fn = dyn_cast<FunctionDecl>(item); fn && !used.contains(fn)) {
          program.unusedFunctions.insert(fn);
        }
      }
    }
  }
}

} // namespace tz
