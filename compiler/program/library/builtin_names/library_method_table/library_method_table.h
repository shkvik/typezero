#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>
#include <string>

namespace tz {

enum class LibraryMethod {
  LISTENERS_OF,
  REMOVE_LISTENER_WITH,
  ADD_FUNCTION_LISTENER,
  ADD_OBJECT_LISTENER,
  SET_ENCODING,
  PUSH_TYPED,
  UNSHIFT_TYPED,
  WRITE_TYPED,
  END_TYPED,
  FINISHED_RESULT,
  EOS_FOR,
  COMPOSE_ALL,
  PIPELINE_TO,
  PIPELINE_OUT,
  PIPELINE_VALUE,
  PIPELINE_RESULT,
  START_TLS,
};

struct LibraryMethodInfo {
  llvm::StringLiteral name;
  LibraryMethod value;
};

class LibraryMethodTable {
public:
  static std::optional<LibraryMethod> find(llvm::StringRef name);
  static llvm::StringRef getName(LibraryMethod value);
  static std::string getSetterName(llvm::StringRef field);
};

}
