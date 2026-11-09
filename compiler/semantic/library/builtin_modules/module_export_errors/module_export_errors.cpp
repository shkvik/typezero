#include "compiler/semantic/library/builtin_modules/module_export_errors/module_export_errors.h"

namespace tz {

std::string ErrorTextModuleExport::none(llvm::StringRef path, llvm::StringRef name) {
  return "'" + path.str() + "' has no export named '" + name.str() + "'";
}

std::string ErrorTextModuleExport::notYet(llvm::StringRef name, llvm::StringRef path) {
  return "'" + name.str() + "' of '" + path.str() + "' is not supported yet";
}

}
