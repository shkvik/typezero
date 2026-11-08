#pragma once

#include "llvm/ADT/StringRef.h"
#include <cstddef>
#include <cstdint>
#include <string>

namespace tz {

class ErrorTextChildRun {
public:
  static std::string zeroFile(bool exec);
  static std::string emptyName();
  static std::string zeroArg(size_t index);
  static std::string optionsAt(llvm::StringRef fn);
  static std::string zeroCwd();
  static std::string negative(llvm::StringRef limit);
  static std::string unixId(llvm::StringRef id);
  static std::string noSignal(llvm::StringRef signal);
};

class ErrorTextChildOption {
public:
  static std::string encoding(llvm::StringRef fn);
  static std::string stdio(llvm::StringRef fn);
  static std::string bufferText();
  static std::string input(llvm::StringRef fn, llvm::StringRef type);
  static std::string killSignal(llvm::StringRef type);
  static std::string execShell();
  static std::string shellFlag(llvm::StringRef fn);
  static std::string shell(llvm::StringRef type);
};

class ErrorTextChildStdio {
public:
  static std::string noFd(uint64_t fd);
  static std::string literal();
  static std::string ipc();
  static std::string unknown(llvm::StringRef text);
  static std::string three();
  static std::string spread();
};

}
