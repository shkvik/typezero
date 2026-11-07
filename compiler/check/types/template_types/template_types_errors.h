#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class TemplateTypesHoleTypeErrorCode {
  NOT_A_HOLE,
};

struct TemplateTypesHoleTypeError {
  TemplateTypesHoleTypeErrorCode code;
  std::string message;
};

class ErrorTextHoleType {
public:
  static std::string notHole(llvm::StringRef type);
};

enum class TemplateTypesStringCountErrorCode {
  TOO_MANY_STRINGS,
};

struct TemplateTypesStringCountError {
  TemplateTypesStringCountErrorCode code;
  std::string message;
};

class ErrorTextStringCount {
public:
  static std::string tooMany(size_t most);
};

}
