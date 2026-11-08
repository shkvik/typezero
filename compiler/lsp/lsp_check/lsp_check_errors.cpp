#include "compiler/lsp/lsp_check/lsp_check_errors.h"

namespace tz {

std::string ErrorTextLspCheck::unread(llvm::StringRef path, llvm::StringRef why) {
  return "cannot read '" + path.str() + "': " + why.str();
}

std::string ErrorTextLspCheck::notJson(llvm::StringRef path, llvm::StringRef why) {
  return "'" + path.str() + "' is not JSON: " + why.str();
}

std::string ErrorTextLspCheck::noEntry(llvm::StringRef path) {
  return "'" + path.str() + "' names no \"entry\"";
}

std::string ErrorTextLspCheck::unwritten(llvm::StringRef path, llvm::StringRef why) {
  return "cannot write '" + path.str() + "': " + why.str();
}

std::string ErrorTextLspCheck::packages() {
  return "the packages of package.json are not ready; run 'tz build' to see why";
}

}
