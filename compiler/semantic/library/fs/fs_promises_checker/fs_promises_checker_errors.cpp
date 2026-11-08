#include "compiler/semantic/library/fs/fs_promises_checker/fs_promises_checker_errors.h"

namespace tz {

std::string ErrorTextFsPromises::constants() {
  return "'constants' of 'node:fs/promises' is not a function";
}

std::string ErrorTextFsPromises::open() {
  return "'open' of 'node:fs/promises' is not supported yet: there is no FileHandle; "
         "use 'readFile', 'writeFile' or 'appendFile', which read and write "
         "Buffers too";
}

std::string ErrorTextFsPromises::notYet(llvm::StringRef name) {
  return "'" + name.str() + "' of 'node:fs/promises' is not supported yet";
}

}
