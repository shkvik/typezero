#include "compiler/semantic/library/bytes/buffer_exports/buffer_exports_errors.h"
#include "compiler/semantic/feature/feature_errors.h"

namespace tz {

std::string ErrorTextBufferExport::notYet(llvm::StringRef name, llvm::StringRef path) {
  return ErrorTextModuleExport::notYet(name, path) + "; tz has Buffer";
}

}
