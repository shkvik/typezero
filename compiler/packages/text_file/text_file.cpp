#include "compiler/packages/text_file/text_file.h"
#include "compiler/packages/report/report.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

namespace tz::pkg {

bool readText(llvm::StringRef file, std::string &text) {
  auto buffer = llvm::MemoryBuffer::getFile(file, /*IsText=*/true);
  if (!buffer) {
    report("cannot read '" + file + "': " + buffer.getError().message());
    return false;
  }
  text = (*buffer)->getBuffer().str();

  return true;
}

} // namespace tz::pkg
