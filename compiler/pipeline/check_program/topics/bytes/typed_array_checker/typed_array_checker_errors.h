#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class TypedArrayCheckerSetSourceErrorCode {
  ITERATOR_SOURCE,
};

struct TypedArrayCheckerSetSourceError {
  TypedArrayCheckerSetSourceErrorCode code;
  std::string message;
};

class ErrorTextTypedArray {
public:
  static std::string mapFits(llvm::StringRef what, llvm::StringRef range, llvm::StringRef hint);
  static std::string flatMap();
  static std::string iterator();
  static std::string index(llvm::StringRef array);
  static std::string locale();
  static std::string noMember(llvm::StringRef name, llvm::StringRef type);
  static std::string offsetOf(llvm::StringRef name);
  static std::string setOffset();
  static std::string setFits(llvm::StringRef offset, llvm::StringRef source,
                             llvm::StringRef target);
};

}
