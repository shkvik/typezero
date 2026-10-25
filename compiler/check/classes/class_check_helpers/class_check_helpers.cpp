#include "compiler/check/classes/class_check_helpers/class_check_helpers.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

std::string variantText(const FunctionDecl *fn) {
  std::string text = (fn->isConstructor ? llvm::StringRef("constructor") : fn->name).str() + "(";
  const FuncSymbol *sym = fn->sym;
  for (size_t i = 0; i < fn->params.size(); ++i) {
    if (i > 0) {
      text += ", ";
    }
    const Param &param = fn->params[i];
    text += (param.rest ? "..." : "") + param.label().str();
    const Type *type = sym && i < sym->params.size() ? sym->params[i] : nullptr;
    if (!type) {
      continue;
    }
    if (type->isOptional() && !param.rest && (param.optional || param.defaultValue)) {
      text += "?: " + type->inner()->name().str();
    } else {
      text += ": " + type->name().str();
    }
  }

  return text + ")";
}

} // namespace tz
