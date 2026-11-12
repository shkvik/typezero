#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class LoopCheckerIterableTypeErrorCode {
  MAY_BE_UNDEFINED,
  TUPLE_NOT_ITERABLE,
  RECORD_NOT_ITERABLE,
  ASYNC_ONLY,
  CONCURRENT_MAP,
  NOT_ITERABLE,
};

struct LoopCheckerIterableTypeError {
  LoopCheckerIterableTypeErrorCode code;
  std::string message;
};

class ErrorTextForOf {
public:
  static std::string mayBeUndef(llvm::StringRef type);
  static std::string tuple();
  static std::string record();
  static std::string later(llvm::StringRef type);
  static std::string concurrent();
  static std::string cannot(llvm::StringRef type);
  static std::string loop();
};

enum class LoopCheckerBindingTypeErrorCode {
  FOR_IN_ANNOTATED,
  FOR_OF_ANNOTATED,
};

struct LoopCheckerBindingTypeError {
  LoopCheckerBindingTypeErrorCode code;
  std::string message;
};

class ErrorTextLoopBinding {
public:
  static std::string forIn();
  static std::string forOf();
};

enum class LoopCheckerBindingShapeErrorCode {
  NOT_AN_ARRAY,
};

struct LoopCheckerBindingShapeError {
  LoopCheckerBindingShapeErrorCode code;
  std::string message;
};

class ErrorTextLoopShape {
public:
  static std::string notArray(llvm::StringRef element);
};

}
