#include "compiler/semantic/feature/feature_errors.h"

namespace tz {

std::string ErrorTextModuleExport::none(llvm::StringRef path, llvm::StringRef name) {
  return "'" + path.str() + "' has no export named '" + name.str() + "'";
}

std::string ErrorTextModuleExport::notYet(llvm::StringRef name, llvm::StringRef path) {
  return "'" + name.str() + "' of '" + path.str() + "' is not supported yet";
}

}
