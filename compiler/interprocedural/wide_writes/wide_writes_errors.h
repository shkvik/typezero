#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

class Type;

enum class WideWritesWideningErrorCode {
  PUTS_INTO_ARRAY,
  PUTS_INTO_INNER_ARRAY,
  WRITES_READ_ONLY,
  WRITES_OTHER_TYPE,
};

struct WideWritesWideningError {
  WideWritesWideningErrorCode code;
  std::string message;
};

class ErrorTextWidening {
public:
  static std::string someLine();
  static std::string line(int number);
  static std::string through(const Type *type);
  static std::string lineOf(int number, llvm::StringRef file);
  static std::string intoInner(llvm::StringRef head, const Type *value, const Type *narrow);
  static std::string head(const Type *from, const Type *to, llvm::StringRef role,
                          llvm::StringRef line);
  static std::string intoArray(llvm::StringRef head, const Type *value, const Type *narrow,
                               llvm::StringRef name);
  static std::string readOnly(llvm::StringRef head, const Type *owner, llvm::StringRef field,
                              int depth);
  static std::string otherType(llvm::StringRef head, const Type *value, const Type *owner,
                               llvm::StringRef field, const Type *narrow, int depth);

private:
  static std::string what(const Type *value);
  static std::string article(const Type *type);
  static std::string place(const Type *owner, llvm::StringRef field, int depth);
};

}
