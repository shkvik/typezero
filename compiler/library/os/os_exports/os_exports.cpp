#include "compiler/library/os/os_exports/os_exports.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

namespace {

const llvm::StringLiteral kOsFunctions[] = {
    "platform", "homedir", "tmpdir",     "hostname", "cpus",    "availableParallelism",
    "totalmem", "freemem", "arch",       "type",     "release", "version",
    "machine",  "uptime",  "endianness", "loadavg",  "userInfo"};

const llvm::StringLiteral kOsExports[] = {"EOL",         "devNull",           "getPriority",
                                          "setPriority", "networkInterfaces", "constants"};

}

OsExports::ResultExportFound OsExports::osExport(llvm::StringRef name, llvm::StringRef path) {
  if (llvm::is_contained(kOsFunctions, name) || name == "EOL" || name == "devNull") {
    return ok(name);
  }
  if (llvm::is_contained(kOsExports, name)) {
    std::string message = ErrorTextModuleExport::notYet(name, path);

    return err(FeatureExportFoundError{
        .code = FeatureExportFoundErrorCode::NOT_YET,
        .message = message,
    });
  }
  std::string message = ErrorTextModuleExport::none(path, name);

  return err(FeatureExportFoundError{
      .code = FeatureExportFoundErrorCode::NO_EXPORT,
      .message = message,
  });
}

}
