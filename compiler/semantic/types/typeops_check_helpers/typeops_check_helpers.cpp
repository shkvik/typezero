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
  auto members = std::vector<const Type *>();
  auto stack = std::vector<const Type *>{type};
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
      members.push_back(Type::boolLiteral(false));
      members.push_back(Type::boolLiteral(true));
    } else if (!next->isNever()) {
      members.push_back(next);
    }
  }

  return members;
}

}
