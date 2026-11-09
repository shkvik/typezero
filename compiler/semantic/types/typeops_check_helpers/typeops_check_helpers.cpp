#include "compiler/semantic/types/typeops_check_helpers/typeops_check_helpers.h"
#include "compiler/program/ast/ast.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/MemoryBuffer.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

std::vector<const Type *> TypeopsCheckHelpers::distributedMembers(const Type *type) {
  std::vector<const Type *> out;
  std::vector<const Type *> stack = {type};
  while (!stack.empty()) {
    const Type *next = stack.back();
    stack.pop_back();
    if (next->isOptional()) {
      stack.push_back(Type::undefinedTy());
      stack.push_back(next->inner());
    } else if (next->isUnion()) {
      for (const Type *member : llvm::reverse(next->members())) {
        stack.push_back(member);
      }
    } else if (next->isBool()) {
      out.push_back(Type::boolLiteral(false));
      out.push_back(Type::boolLiteral(true));
    } else if (!next->isNever()) {
      out.push_back(next);
    }
  }

  return out;
}

}
