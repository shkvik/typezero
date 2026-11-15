#include "compiler/native/native_platform/native_platform.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

NativePlatform::NativePlatform(const llvm::Triple &triple)
    : system(triple.isOSWindows() ? "win32" : "linux"),
      processor(triple.getArch() == llvm::Triple::aarch64 ? "arm64" : "x64") {}

bool NativePlatform::isFitting(const pkg::NativeEntry &entry) const {
  return this->isListed(entry.os, this->system) && this->isListed(entry.cpu, this->processor);
}

std::string NativePlatform::getName() const {
  return this->system + "-" + this->processor;
}

bool NativePlatform::isListed(const std::vector<std::string> &names, llvm::StringRef name) const {
  return names.empty() || llvm::is_contained(names, name);
}

}
