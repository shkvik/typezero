#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

struct DestinationScan {
  llvm::StringRef name;
  std::vector<std::pair<const Type *, llvm::SMLoc>> found;
  std::vector<llvm::StringMap<const Type *>> locals;
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

}
