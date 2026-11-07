#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class TupleCheckerCheckTupleJoinErrorCode {
  ALREADY_REPORTED,
  WRONG_ARITY,
  ELEMENT_NOT_TEXT,
};

struct TupleCheckerCheckTupleJoinError {
  TupleCheckerCheckTupleJoinErrorCode code;
  std::string message;
};

class ErrorTextTupleJoin {
public:
  static std::string notText(size_t index, llvm::StringRef tuple, llvm::StringRef element);
};

enum class TupleCheckerCheckTupleMemberErrorCode {
  NO_SUCH_PROPERTY,
};

struct TupleCheckerCheckTupleMemberError {
  TupleCheckerCheckTupleMemberErrorCode code;
  std::string message;
};

class ErrorTextTupleMember {
public:
  static std::string noProperty(llvm::StringRef name, llvm::StringRef type);
};

enum class TupleCheckerCheckTupleMethodErrorCode {
  ALREADY_REPORTED,
  JOIN_FAILED,
  FIXED_LENGTH,
  COPY_FIRST,
  DESTRUCTURE_INSTEAD,
};

struct TupleCheckerCheckTupleMethodError {
  TupleCheckerCheckTupleMethodErrorCode code;
  std::string message;
};

class ErrorTextTupleMethod {
public:
  static std::string fixedLen(llvm::StringRef name, llvm::StringRef text);
  static std::string copyFirst(llvm::StringRef name, llvm::StringRef text);
  static std::string destruct(llvm::StringRef name, llvm::StringRef text);
};

enum class TupleCheckerCheckTupleLitErrorCode {
  SPREAD_IN_TUPLE,
  WRONG_LENGTH,
};

struct TupleCheckerCheckTupleLitError {
  TupleCheckerCheckTupleLitErrorCode code;
  std::string message;
};

class ErrorTextTupleLit {
public:
  static std::string spread(llvm::StringRef tuple);
  static std::string length(llvm::StringRef tuple, size_t want, size_t got);
  static std::string element(size_t index);
};

}
