#pragma once

#include "compiler/check/checker/checker.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

// Functions that several classes made from sema_destination.cpp use.

struct Checker::DestinationScan {
  llvm::StringRef name; // the variable
  std::vector<std::pair<const Type *, llvm::SMLoc>> found;
  // The names declared in the code scanned so far, which hide the variables
  // of the same name visible at the declaration, with their types where the
  // declaration gives one.
  std::vector<llvm::StringMap<const Type *>> locals;
  // The declared return type of the function whose returns are scanned.
  const Type *returns = nullptr;

  bool hidden(llvm::StringRef other) const {
    return llvm::any_of(
        locals, [&](const llvm::StringMap<const Type *> &scope) { return scope.contains(other); });
  }
  void declare(llvm::StringRef local, const Type *type) {
    if (!locals.empty()) {
      locals.back()[local] = type;
    }
  }
};

} // namespace tz
