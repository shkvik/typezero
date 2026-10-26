#include "compiler/emit/ir_writer/ir_writer.h"
#include "compiler/emit/file_writer/file_writer.h"

namespace tz {

Errors IrWriter::run(const llvm::Module &module, llvm::StringRef path) const {
  FileWriter writer;

  return writer.write(path, [&](llvm::raw_pwrite_stream &out) { module.print(out, nullptr); });
}

}
