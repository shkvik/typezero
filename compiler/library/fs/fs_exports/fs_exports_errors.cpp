#include "compiler/library/fs/fs_exports/fs_exports_errors.h"
#include "compiler/library/feature/feature_errors.h"

namespace tz {

std::string ErrorTextFsExport::inTzFs(llvm::StringRef path, llvm::StringRef name) {
  return ErrorTextModuleExport::none(path, name) + "; it is in \"tz:fs\"";
}

std::string ErrorTextFsExport::callback(llvm::StringRef name, llvm::StringRef path,
                                        llvm::StringRef sync) {
  return "'" + name.str() + "' of '" + path.str() +
         "' takes a callback, which tz does not have; import '" + name.str() +
         "' from \"node:fs/promises\", which gives a Promise, or '" + sync.str() +
         "', which gives what it reads" +
         (name == "readFile" ? ", e.g. 'const text = await readFile(path, \"utf8\")'" : "");
}

std::string ErrorTextFsExport::syncOnly(llvm::StringRef name, llvm::StringRef path,
                                        llvm::StringRef sync) {
  return "'" + name.str() + "' of '" + path.str() +
         "' takes a callback, which tz does not have; import '" + sync.str() +
         "' and use what it returns";
}

std::string ErrorTextFsExport::waiting(llvm::StringRef path, llvm::StringRef name) {
  return ErrorTextModuleExport::none(path, name) + "; the function that waits is '" +
         name.drop_back(4).str() + "', or import '" + name.str() + "' from \"node:fs\"";
}

std::string ErrorTextFsExport::readline(llvm::StringRef name, llvm::StringRef path) {
  return ErrorTextModuleExport::notYet(name, path) + "; tz has createInterface";
}

std::string ErrorTextFsExport::tzFs(llvm::StringRef path, llvm::StringRef name, bool inNodeFs) {
  std::string message =
      ErrorTextModuleExport::none(path, name) + "; it has replaceSync, replace and ReplaceError";
  if (inNodeFs) {
    message += "; '" + name.str() + "' is in \"node:fs\"";
  }

  return message;
}

std::string ErrorTextFsExport::tzIo(llvm::StringRef path, llvm::StringRef name) {
  return ErrorTextModuleExport::none(path, name) + "; it has readLine";
}

}
