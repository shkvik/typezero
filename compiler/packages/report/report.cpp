#include "compiler/packages/report/report.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

namespace tz::pkg {

void report(const llvm::Twine &message) { llvm::errs() << "tz: " << message << "\n"; }

void reportAt(llvm::StringRef file, llvm::StringRef text, size_t offset,
              const llvm::Twine &message) {
  llvm::SourceMgr sourceMgr;
  unsigned id =
      sourceMgr.AddNewSourceBuffer(llvm::MemoryBuffer::getMemBufferCopy(text, file), llvm::SMLoc());
  const char *start = sourceMgr.getMemoryBuffer(id)->getBufferStart();
  llvm::SMLoc loc = llvm::SMLoc::getFromPointer(start + std::min(offset, text.size()));
  sourceMgr.PrintMessage(llvm::errs(), loc, llvm::SourceMgr::DK_Error, message);
}

void progress(const llvm::Twine &message) { llvm::errs() << "tz: " << message << "\n"; }

}
