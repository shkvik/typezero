#include "compiler/emit/file_writer/file_writer.h"
#include "llvm/Support/FileSystem.h"

namespace tz {

std::vector<Diagnostic>
FileWriter::write(llvm::StringRef path,
                  llvm::function_ref<void(llvm::raw_pwrite_stream &)> contents) const {
  std::vector<Diagnostic> errors;
  std::error_code ec;
  llvm::raw_fd_ostream out(path, ec, llvm::sys::fs::OF_None);
  if (ec) {
    std::string message = ("cannot write '" + path + "': " + ec.message()).str();
    errors.push_back({.form = Diagnostic::Form::Tool, .message = message});
    return errors;
  }
  contents(out);
  out.close();
  if (out.has_error()) {
    std::string message = ("cannot write '" + path + "': " + out.error().message()).str();
    errors.push_back({.form = Diagnostic::Form::Tool, .message = message});
    out.clear_error();
  }

  return errors;
}

}
