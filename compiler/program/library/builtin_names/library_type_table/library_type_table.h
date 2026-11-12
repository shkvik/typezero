#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class LibraryType {
  EVENT_LISTENER_OBJECT,
  FINISHED_OPTIONS,
  HOOK_CALLBACK,
  NATIVE_ERROR,
  READABLE_FROM_OPTIONS,
  STREAM_CALLBACK,
  TRANSFORM_HOOK_CALLBACK,
  WRITE_CHUNK,
  READ_STREAM_OPTIONS,
  WRITE_STREAM_OPTIONS,
};

struct LibraryTypeInfo {
  llvm::StringLiteral name;
  LibraryType value;
};

class LibraryTypeTable {
public:
  static std::optional<LibraryType> find(llvm::StringRef name);
  static llvm::StringRef getName(LibraryType value);
};

}
