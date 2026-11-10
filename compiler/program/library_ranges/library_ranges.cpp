#include "compiler/program/library_ranges/library_ranges.h"
#include "compiler/program/module/module.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

LibraryRanges LibraryRanges::collect(const SourceFile &file, const llvm::SourceMgr &sourceMgr) {
  LibraryRanges found;
  for (const Module *module : file.modules) {
    if (!module->builtin) {
      continue;
    }
    for (unsigned id : module->buffers) {
      const llvm::MemoryBuffer *buffer = sourceMgr.getMemoryBuffer(id);
      found.ranges.push_back({buffer->getBufferStart(), buffer->getBufferEnd()});
    }
  }

  return found;
}

bool LibraryRanges::contains(llvm::SMLoc loc) const {
  const char *at = loc.getPointer();
  auto inside = [at](const std::pair<const char *, const char *> &range) {
    return at >= range.first && at < range.second;
  };

  return llvm::any_of(this->ranges, inside);
}

}
