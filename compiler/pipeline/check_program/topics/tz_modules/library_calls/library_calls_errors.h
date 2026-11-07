#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class LibraryCallsIntrinsicShapeErrorCode {
  NOT_AN_INTRINSIC,
};

struct LibraryCallsIntrinsicShapeError {
  LibraryCallsIntrinsicShapeErrorCode code;
  std::string message;
};

class ErrorTextLibraryCall {
public:
  static std::string listener();
  static std::string asClass();
  static std::string ahead(llvm::StringRef name);
  static std::string context();
};

}
