#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class JumpCheckerCheckJumpErrorCode {
  LABEL_NOT_FOUND,
  BREAK_OUTSIDE,
  CONTINUE_OUTSIDE,
  CONTINUE_NOT_LOOP,
};

struct JumpCheckerCheckJumpError {
  JumpCheckerCheckJumpErrorCode code;
  std::string message;
};

class ErrorTextJump {
public:
  static std::string noLabel(llvm::StringRef label);
  static std::string breakOut();
  static std::string continueOut();
  static std::string notLoop(llvm::StringRef keyword, llvm::StringRef label);
};

enum class JumpCheckerLabelFreshErrorCode {
  DUPLICATE_LABEL,
};

struct JumpCheckerLabelFreshError {
  JumpCheckerLabelFreshErrorCode code;
  std::string message;
};

class ErrorTextLabel {
public:
  static std::string duplicate(llvm::StringRef label);
};

enum class JumpCheckerLabelBodyErrorCode {
  DECLARATION_LABELED,
};

struct JumpCheckerLabelBodyError {
  JumpCheckerLabelBodyErrorCode code;
  std::string message;
};

class ErrorTextLabelBody {
public:
  static std::string declared();
};

}
