#include "compiler/interprocedural/unused_functions/unused_functions.h"
#include "compiler/program/ast_walker/ast_walker.h"
#include "llvm/ADT/STLExtras.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

UnusedFunctions::UnusedFunctions(SourceFile &checked)
    : file(checked) {}

llvm::SmallPtrSet<const FunctionDecl *, 8> UnusedFunctions::findUnused() const {
  auto unused = llvm::SmallPtrSet<const FunctionDecl *, 8>();
  if (this->file.modules.size() < 2) {
    return unused;
  }
  auto used = this->findUsed();

  for (Module *module : this->file.modules) {
    if (module->isEntry) {
      continue;
    }
    for (Stmt *item : module->file.items) {
      auto *fn = dyn_cast<FunctionDecl>(item);
      if (fn != nullptr && !used.contains(fn)) {
        unused.insert(fn);
      }
    }
  }

  return unused;
}

llvm::SmallPtrSet<const FunctionDecl *, 16> UnusedFunctions::findUsed() const {
  auto used = llvm::SmallPtrSet<const FunctionDecl *, 16>();
  auto queue = this->rootFunctions();

  while (!queue.empty()) {
    const auto *fn = queue.back();
    queue.pop_back();
    auto [_, isInserted] = used.insert(fn);
    if (!isInserted) {
      continue;
    }
    auto referenced = this->referencedFunctions(fn);
    llvm::append_range(queue, referenced);
  }

  return used;
}

std::vector<const FunctionDecl *> UnusedFunctions::rootFunctions() const {
  auto roots = std::vector<const FunctionDecl *>();
  for (Module *module : this->file.modules) {
    for (Stmt *item : module->file.items) {
      bool isRoot = module->isEntry || !isa<FunctionDecl>(item);
      if (!isRoot) {
        continue;
      }
      auto referenced = this->referencedFunctions(item);
      llvm::append_range(roots, referenced);
    }
  }

  return roots;
}

std::vector<const FunctionDecl *> UnusedFunctions::referencedFunctions(const Stmt *stmt) const {
  auto referenced = std::vector<const FunctionDecl *>();
  for (const Expr *e : AstWalker::exprsIn(stmt, true)) {
    const auto *fn = this->referencedFunction(e);
    if (fn != nullptr) {
      referenced.push_back(fn);
    }
  }

  return referenced;
}

const FunctionDecl *UnusedFunctions::referencedFunction(const Expr *e) const {
  const FuncSymbol *symbol = nullptr;
  const auto *call = dyn_cast<CallExpr>(e);
  const auto *name = dyn_cast<NameExpr>(e);
  if (call != nullptr) {
    symbol = call->func();
  } else if (name != nullptr) {
    symbol = name->func();
  }
  if (symbol == nullptr) {
    return nullptr;
  }

  return symbol->decl;
}

}
