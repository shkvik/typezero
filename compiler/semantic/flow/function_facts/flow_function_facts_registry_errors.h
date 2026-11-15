#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class FlowFunctionFactsRegistryNotePredicateExitErrorCode {
  NOT_NARROWED,
};

struct FlowFunctionFactsRegistryNotePredicateExitError {
  FlowFunctionFactsRegistryNotePredicateExitErrorCode code;
  std::string message;
};

class ErrorTextPredicateExit {
public:
  static std::string mayLie(llvm::StringRef function, llvm::StringRef var, llvm::StringRef type,
                            llvm::StringRef shown);
};

class ErrorTextByteView {
public:
  static std::string offset(llvm::StringRef what, int64_t size);
  static std::string size(llvm::StringRef what, int64_t size);
  static std::string end(llvm::StringRef offset, llvm::StringRef length, int64_t size);
  static std::string notFits(llvm::StringRef what, llvm::StringRef end, llvm::StringRef buffer);
  static std::string element(llvm::StringRef type);
  static std::string offsetOf(llvm::StringRef what);
  static std::string lengthOf(llvm::StringRef what);
};

}
