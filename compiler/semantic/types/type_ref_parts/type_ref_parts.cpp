#include "compiler/semantic/types/type_ref_parts/type_ref_parts.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

std::vector<const TypeRef *> TypeRefParts::collect(const TypeRef &ref) {
  std::vector<const TypeRef *> parts;
  std::vector<const TypeRef *> stack = {&ref};
  while (!stack.empty()) {
    const TypeRef *next = stack.back();
    stack.pop_back();
    parts.push_back(next);
    for (const TypeRef &arg : llvm::reverse(next->args)) {
      stack.push_back(&arg);
    }
  }

  return parts;
}

std::vector<const TypeRef *> TypeRefParts::collect(const TypeRef &ref,
                                                   llvm::ArrayRef<TypeRefKind> route) {
  std::vector<const TypeRef *> parts;
  std::vector<const TypeRef *> stack = {&ref};
  while (!stack.empty()) {
    const TypeRef *next = stack.back();
    stack.pop_back();
    parts.push_back(next);
    if (llvm::is_contained(route, next->kind())) {
      for (const TypeRef &arg : llvm::reverse(next->args)) {
        stack.push_back(&arg);
      }
    }
  }

  return parts;
}

}
