#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class ArgumentCheckerCheckArityErrorCode {
  WRONG_COUNT,
  WRONG_COUNT_TAGGED,
};

struct ArgumentCheckerCheckArityError {
  ArgumentCheckerCheckArityErrorCode code;
  std::string message;
};

class ErrorTextArity {
public:
  static std::string count(unsigned min, unsigned max, size_t got);
  static std::string tagged(unsigned min, unsigned max, size_t got);
};

enum class ArgumentCheckerSpreadRestErrorCode {
  OTHER_TYPE,
};

struct ArgumentCheckerSpreadRestError {
  ArgumentCheckerSpreadRestErrorCode code;
  std::string message;
};

class ErrorTextSpreadRest {
public:
  static std::string otherType(llvm::StringRef from, llvm::StringRef to);
};

enum class ArgumentCheckerTupleRestErrorCode {
  WRONG_COUNT,
};

struct ArgumentCheckerTupleRestError {
  ArgumentCheckerTupleRestErrorCode code;
  std::string message;
};

class ErrorTextTupleRest {
public:
  static std::string count(size_t expected, size_t got);
};

}
