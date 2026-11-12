#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum Encoding { Utf8, Utf16le, Latin1, Ascii, Base64, Base64url, Hex };

struct EncodingNameInfo {
  llvm::StringLiteral name;
  Encoding encoding;
};

class EncodingNameTable {
public:
  static std::optional<Encoding> find(llvm::StringRef name);
  static bool isBufferMarker(llvm::StringRef name);
  static bool isExactUtf8Name(llvm::StringRef name);
  static llvm::StringRef getUtf8Label();
};

}
