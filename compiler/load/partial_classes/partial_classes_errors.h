#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class PartialClassesListHasPartErrorCode {
  NO_PART_OF_THIS_FILE,
  ALREADY_REPORTED,
};

struct PartialClassesListHasPartError {
  PartialClassesListHasPartErrorCode code;
  std::string message;
};

enum class PartialClassesPartClassErrorCode {
  NOT_IMPORTED,
  ALREADY_REPORTED,
  LIBRARY_CLASS,
  NOT_DECLARED_THERE,
  NOT_LISTED,
  EXTENDS_VALUE,
};

struct PartialClassesPartClassError {
  PartialClassesPartClassErrorCode code;
  std::string message;
};

enum class PartialClassesPartShapeErrorCode {
  HAS_FIELD,
  HAS_CONSTRUCTOR,
  HAS_HEADER,
  HAS_DECORATORS,
  OTHER_TYPE_PARAMS,
};

struct PartialClassesPartShapeError {
  PartialClassesPartShapeErrorCode code;
  std::string message;
};

enum class PartialClassesMethodIsNewErrorCode {
  DECLARED_IN_OTHER_FILE,
};

struct PartialClassesMethodIsNewError {
  PartialClassesMethodIsNewErrorCode code;
  std::string message;
};

class ErrorTextListHasPart {
public:
  static std::string noPart(llvm::StringRef path);
};

class ErrorTextPartClass {
public:
  static std::string notImported(llvm::StringRef name);
  static std::string library(llvm::StringRef name, llvm::StringRef path);
  static std::string undeclared(llvm::StringRef name, llvm::StringRef path);
  static std::string notListed(llvm::StringRef name, llvm::StringRef file, llvm::StringRef path);
  static std::string fromValue(llvm::StringRef name);
};

class ErrorTextPartShape {
public:
  static std::string field(llvm::StringRef name, llvm::StringRef field);
  static std::string ctor(llvm::StringRef name);
  static std::string header(llvm::StringRef name);
  static std::string decorators(llvm::StringRef name);
  static std::string typeParams(llvm::StringRef name, llvm::StringRef params);
};

class ErrorTextMethodIsNew {
public:
  static std::string twice(llvm::StringRef method, llvm::StringRef name, llvm::StringRef file);
};

}
