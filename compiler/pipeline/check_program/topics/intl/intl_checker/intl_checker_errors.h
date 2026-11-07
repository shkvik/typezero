#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class IntlCheckerLanguageTagsErrorCode {
  UNKNOWN_TAGS,
  NOT_A_TAG,
};

struct IntlCheckerLanguageTagsError {
  IntlCheckerLanguageTagsErrorCode code;
  std::string message;
};

class ErrorTextIntlNew {
public:
  static std::string unknown(llvm::StringRef name);
  static std::string withNew(llvm::StringRef name);
  static std::string noType();
  static std::string label(llvm::StringRef name);
};

class ErrorTextLanguageTag {
public:
  static std::string unknown(bool constructor, llvm::StringRef what);
  static std::string notTag(llvm::StringRef tag);
  static std::string takes(llvm::StringRef what, llvm::StringRef type, bool array);
  static std::string list(llvm::StringRef what, llvm::StringRef type);
  static std::string listOpen(llvm::StringRef what);
  static std::string spread(llvm::StringRef what);
};

class ErrorTextIntlLocale {
public:
  static std::string missing();
  static std::string options();
  static std::string label();
  static std::string withNew();
};

class ErrorTextIntlFunction {
public:
  static std::string canonical();
  static std::string values();
  static std::string staticOf(llvm::StringRef service, llvm::StringRef member);
  static std::string onlyLocale(llvm::StringRef what);
};

class ErrorTextIntlMember {
public:
  static std::string dateFormat(llvm::StringRef name);
  static std::string onlyCalled(llvm::StringRef name, llvm::StringRef type);
};

}
