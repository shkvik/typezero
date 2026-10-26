#include "compiler/check/types/typeops_check_helpers/typeops_check_helpers.h"
#include "compiler/load/lexer/lexer.h"
#include "compiler/load/parser/parser.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/MemoryBuffer.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

std::vector<const Type *> distributedMembers(const Type *type) {
  std::vector<const Type *> out;
  if (type->isOptional()) {
    out = distributedMembers(type->inner());
    out.push_back(Type::undefinedTy());
  } else if (type->isUnion()) {
    for (const Type *m : type->members()) {
      for (const Type *n : distributedMembers(m)) {
        out.push_back(n);
      }
    }
  } else if (type->isBool()) {
    out = {Type::boolLiteral(false), Type::boolLiteral(true)};
  } else if (!type->isNever()) {
    out.push_back(type);
  }

  return out;
}

}
