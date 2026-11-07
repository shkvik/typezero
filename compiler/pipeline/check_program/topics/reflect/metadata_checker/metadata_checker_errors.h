#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class MetadataCheckerMetadataTypeErrorCode {
  NO_RECORD_TYPE,
};

struct MetadataCheckerMetadataTypeError {
  MetadataCheckerMetadataTypeErrorCode code;
  std::string message;
};

class ErrorTextMetadataType {
public:
  static std::string unknown(llvm::StringRef label);
};

}
