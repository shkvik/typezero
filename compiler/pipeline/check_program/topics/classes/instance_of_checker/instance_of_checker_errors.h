#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class InstanceOfCheckerCheckInstanceOfErrorCode {
  UNTYPED_COLLECTION,
  NONE_MATCHES,
  ALL_MATCH,
  ALWAYS_TRUE,
  TEST_UNDEFINED,
  ALWAYS_FALSE,
  BYTES_INSTANCE,
  NOT_AN_OBJECT,
  NOT_A_CLASS,
  GENERIC_UNKNOWN,
  NEVER_GENERIC,
  NEVER_IN_UNION,
  NEVER_CLASS,
  NOT_IMPLEMENTED,
  NEVER_CREATED,
};

struct InstanceOfCheckerCheckInstanceOfError {
  InstanceOfCheckerCheckInstanceOfErrorCode code;
  std::string message;
};

class ErrorTextInstanceOf {
public:
  static std::string untypedMap(llvm::StringRef builtin, llvm::StringRef example);
  static std::string noneIs(llvm::StringRef base, llvm::StringRef what);
  static std::string allAre(llvm::StringRef base, llvm::StringRef what, llvm::StringRef name);
  static std::string alwaysTrue(llvm::StringRef type, llvm::StringRef name);
  static std::string orUndef(llvm::StringRef type);
  static std::string neverIs(llvm::StringRef type, llvm::StringRef what, llvm::StringRef name);
  static std::string notObject(llvm::StringRef type);
  static std::string notClass();
  static std::string cantTell(llvm::StringRef generic, llvm::StringRef base, llvm::StringRef hint);
  static std::string extendHint(llvm::StringRef generic);
  static std::string unionHint(llvm::StringRef generic, llvm::StringRef other);
  static std::string neverThat(llvm::StringRef base, llvm::StringRef cls);
  static std::string neverUnion(llvm::StringRef base, llvm::StringRef cls);
  static std::string neverClass(llvm::StringRef base, llvm::StringRef cls);
  static std::string notImpl(llvm::StringRef base, llvm::StringRef cls);
  static std::string neverMade(llvm::StringRef base, llvm::StringRef cls);
};

enum class InstanceOfCheckerCheckBytesInstanceOfErrorCode {
  NONE_MATCHES,
  ALL_MATCH,
  ALWAYS_TRUE,
  TEST_UNDEFINED,
  ALWAYS_FALSE,
};

struct InstanceOfCheckerCheckBytesInstanceOfError {
  InstanceOfCheckerCheckBytesInstanceOfErrorCode code;
  std::string message;
};

}
