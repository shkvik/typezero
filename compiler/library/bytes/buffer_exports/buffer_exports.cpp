#include "compiler/library/bytes/buffer_exports/buffer_exports.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

namespace {

const llvm::StringLiteral kBufferExports[] = {"Blob",
                                              "File",
                                              "SlowBuffer",
                                              "atob",
                                              "btoa",
                                              "constants",
                                              "isAscii",
                                              "isUtf8",
                                              "kMaxLength",
                                              "kStringMaxLength",
                                              "resolveObjectURL",
                                              "transcode",
                                              "INSPECT_MAX_BYTES"};

}

std::string bufferExportProblem(llvm::StringRef name, llvm::StringRef path) {
  std::string quoted = "'" + path.str() + "'";
  if (name == "Buffer") {
    return "";
  }
  if (llvm::is_contained(kBufferExports, name)) {
    return "'" + name.str() + "' of " + quoted + " is not supported yet; tz has Buffer";
  }

  return quoted + " has no export named '" + name.str() + "'";
}

}
