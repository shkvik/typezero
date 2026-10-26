#include "compiler/emit/file_writer/file_writer.h"
#include "llvm/Support/FileSystem.h"

namespace tz {

Errors FileWriter::write(llvm::StringRef path,
                         llvm::function_ref<void(llvm::raw_pwrite_stream &)> contents) const {
  Errors errors;
  std::error_code ec;
  llvm::raw_fd_ostream out(path, ec, llvm::sys::fs::OF_None);
  if (ec) {
    errors.tool("cannot write '" + path + "': " + ec.message());
    return errors;
  }
  contents(out);
  out.close();
  if (out.has_error()) {
    errors.tool("cannot write '" + path + "': " + out.error().message());
    out.clear_error();
  }

  return errors;
}

}
