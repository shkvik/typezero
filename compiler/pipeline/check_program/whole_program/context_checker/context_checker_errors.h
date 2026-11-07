#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

class ErrorTextAsyncStore {
public:
  static std::string section();
  static std::string kept(llvm::StringRef where);
  static std::string made();
  static std::string runStore();
  static std::string readsIt(llvm::StringRef through);
  static std::string interval(llvm::StringRef how);
  static std::string restart(llvm::StringRef how);
};

}
