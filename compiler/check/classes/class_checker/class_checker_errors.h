#pragma once

#include <string>

namespace tz {

enum class ClassCheckerTopLevelClassErrorCode {
  NOT_TOP_LEVEL,
};

struct ClassCheckerTopLevelClassError {
  ClassCheckerTopLevelClassErrorCode code;
  std::string message;
};

enum class ClassCheckerFieldStartValueErrorCode {
  NO_START_VALUE,
  NEVER_SET,
};

struct ClassCheckerFieldStartValueError {
  ClassCheckerFieldStartValueErrorCode code;
  std::string message;
};

enum class ClassCheckerOverrideReturnErrorCode {
  RETURNS_OTHER_TYPE,
};

struct ClassCheckerOverrideReturnError {
  ClassCheckerOverrideReturnErrorCode code;
  std::string message;
};

enum class ClassCheckerFieldsSetErrorCode {
  FIELD_NOT_SET_ON_EVERY_PATH,
};

struct ClassCheckerFieldsSetError {
  ClassCheckerFieldsSetErrorCode code;
  std::string message;
};

enum class ClassCheckerThisOfErrorCode {
  OUTSIDE_METHOD,
  IN_STATIC_FIELD,
  IN_STATIC_METHOD,
  IN_PLAIN_FUNCTION,
};

struct ClassCheckerThisOfError {
  ClassCheckerThisOfErrorCode code;
  std::string message;
};

enum class ClassCheckerReadyThisErrorCode {
  IN_CLOSURE_BEFORE_FIELDS_SET,
  BEFORE_SUPER,
  BEFORE_FIELDS_SET,
};

struct ClassCheckerReadyThisError {
  ClassCheckerReadyThisErrorCode code;
  std::string message;
};

}
