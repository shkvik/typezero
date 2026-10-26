#include "compiler/check/types/conditional_check_helpers/conditional_check_helpers.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include <cmath>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

void distribute(const Type *type, std::vector<const Type *> &out) {
  if (type->isOptional()) {
    distribute(type->inner(), out);
    out.push_back(Type::undefinedTy());
  } else if (type->isUnion()) {
    for (const Type *m : type->members()) {
      distribute(m, out);
    }
  } else if (type->isBool()) {
    out.push_back(Type::boolLiteral(false));
    out.push_back(Type::boolLiteral(true));
  } else if (!type->isNever()) {
    out.push_back(type);
  }
}

}
