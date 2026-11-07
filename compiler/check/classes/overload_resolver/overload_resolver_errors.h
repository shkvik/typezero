#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class OverloadResolverChooseVariantErrorCode {
  ALREADY_REPORTED,
  SPREAD_ARGUMENT,
  NO_VARIANT_FOR_COUNT,
  NO_VARIANT_FITS,
  GENERIC_VARIANT,
  SEVERAL_VARIANTS_FIT,
};

struct OverloadResolverChooseVariantError {
  OverloadResolverChooseVariantErrorCode code;
  std::string message;
};

class ErrorTextVariant {
public:
  static std::string ctorOf(llvm::StringRef cls);
  static std::string quoted(llvm::StringRef name);
  static std::string spread(llvm::StringRef what);
  static std::string count(llvm::StringRef what, size_t count, llvm::StringRef list);
  static std::string typeParams(llvm::StringRef variant);
  static std::string mismatch(llvm::StringRef variant, llvm::StringRef param, size_t index,
                              llvm::StringRef type);
  static std::string noFit(llvm::StringRef what, llvm::StringRef why, llvm::StringRef hint);
  static std::string generic(llvm::StringRef variant);
  static std::string tie(llvm::StringRef what, llvm::StringRef list, bool literals);
};

enum class OverloadResolverVariantForTypeErrorCode {
  SEVERAL_VARIANTS,
};

struct OverloadResolverVariantForTypeError {
  OverloadResolverVariantForTypeErrorCode code;
  std::string message;
};

class ErrorTextVariantType {
public:
  static std::string several(llvm::StringRef name, llvm::StringRef list, llvm::StringRef type,
                             llvm::StringRef object);
};

}
