#pragma once

#include "compiler/packages/native_entry/native_entry.h"
#include "llvm/TargetParser/Triple.h"
#include <string>

namespace tz {

class NativePlatform {
public:
  explicit NativePlatform(const llvm::Triple &triple);

  bool isFitting(const pkg::NativeEntry &entry) const;
  std::string getName() const;

private:
  bool isListed(const std::vector<std::string> &names, llvm::StringRef name) const;

  std::string system;
  std::string processor;
};

}
