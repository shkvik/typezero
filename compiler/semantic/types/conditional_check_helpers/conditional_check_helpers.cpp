#include "compiler/semantic/types/conditional_check_helpers/conditional_check_helpers.h"
#include "compiler/semantic/types/typeops_check_helpers/typeops_check_helpers.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include <cmath>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

void ConditionalCheckHelpers::distribute(const Type *type, std::vector<const Type *> &out) {
  std::vector<const Type *> members = TypeopsCheckHelpers::distributedMembers(type);
  out.insert(out.end(), members.begin(), members.end());
}

}
