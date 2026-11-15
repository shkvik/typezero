#pragma once

#include "llvm/ADT/StringRef.h"
#include <cstddef>
#include <string>

namespace tz::pkg {

enum class NativeManifestErrorCode {
  NOT_A_LIST,
  NOT_AN_OBJECT,
  UNKNOWN_FIELD,
  NOT_A_STRING,
  NOT_A_STRING_LIST,
  EMPTY_LIST,
  MISSING_FIELD,
  BOTH_KINDS,
  NO_KIND,
  SOURCES_ONLY,
  LIBRARIES_ONLY,
  DEFINE_WITH_FLAG,
  NOT_A_DEFINE,
  UNKNOWN_SYSTEM,
  UNKNOWN_PROCESSOR,
  DEBUG_NOT_AN_OBJECT,
  UNKNOWN_DEBUG_FIELD,
};

struct NativeManifestError {
  NativeManifestErrorCode code;
  std::string message;
  size_t offset = 0;
};

class ErrorTextNative {
public:
  static std::string notList();
  static std::string notObject();
  static std::string unknown(llvm::StringRef key);
  static std::string notString(llvm::StringRef key);
  static std::string notStrings(llvm::StringRef key);
  static std::string empty(llvm::StringRef key);
  static std::string missing(llvm::StringRef key);
  static std::string bothKinds();
  static std::string noKind();
  static std::string sourceOnly(llvm::StringRef key);
  static std::string libOnly(llvm::StringRef key);
  static std::string flag(llvm::StringRef define);
  static std::string notDefine(llvm::StringRef define);
  static std::string system(llvm::StringRef value);
  static std::string processor(llvm::StringRef value);
  static std::string debug();
  static std::string debugKey(llvm::StringRef key);
};

}
