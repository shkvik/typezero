#pragma once

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Program.h"
#include <optional>
#include <string>

namespace tz {

class CheckProcess {
public:
  bool start(llvm::StringRef program, llvm::ArrayRef<llvm::StringRef> args, llvm::StringRef log,
             std::string &error);
  std::optional<int> poll();
  void kill();

private:
  llvm::sys::ProcessInfo info_;
  bool running_ = false;
};

}
