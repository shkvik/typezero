#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

class ErrorTextPoolObject {
public:
  static std::string section();
  static std::string runStore();
  static std::string awaitRun();
  static std::string storeCopy();
  static std::string voidCall();
  static std::string keptFn(llvm::StringRef where);
  static std::string kept(llvm::StringRef where);
  static std::string readsIt(llvm::StringRef through);
  static std::string voidStore(llvm::StringRef how);
  static std::string later(llvm::StringRef how);
  static std::string returned();
};

}
