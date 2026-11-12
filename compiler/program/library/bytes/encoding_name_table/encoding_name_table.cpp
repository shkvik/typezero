#include "compiler/program/library/bytes/encoding_name_table/encoding_name_table.h"

namespace tz {

namespace {

const EncodingNameInfo kEncodings[] = {
    {.name = "utf8", .encoding = Utf8},           {.name = "utf-8", .encoding = Utf8},
    {.name = "utf16le", .encoding = Utf16le},     {.name = "utf-16le", .encoding = Utf16le},
    {.name = "ucs2", .encoding = Utf16le},        {.name = "ucs-2", .encoding = Utf16le},
    {.name = "latin1", .encoding = Latin1},       {.name = "binary", .encoding = Latin1},
    {.name = "ascii", .encoding = Ascii},         {.name = "base64", .encoding = Base64},
    {.name = "base64url", .encoding = Base64url}, {.name = "hex", .encoding = Hex},
};

constexpr llvm::StringLiteral kBufferMarker = "buffer";
constexpr llvm::StringLiteral kUtf8Label = "utf-8";

}

std::optional<Encoding> EncodingNameTable::find(llvm::StringRef name) {
  std::string lower = name.lower();
  for (const EncodingNameInfo &info : kEncodings) {
    if (info.name == lower) {
      return info.encoding;
    }
  }

  return std::nullopt;
}

bool EncodingNameTable::isBufferMarker(llvm::StringRef name) {
  return name == kBufferMarker;
}

bool EncodingNameTable::isExactUtf8Name(llvm::StringRef name) {
  for (const EncodingNameInfo &info : kEncodings) {
    if (info.encoding == Utf8 && info.name == name) {
      return true;
    }
  }

  return false;
}

llvm::StringRef EncodingNameTable::getUtf8Label() {
  return kUtf8Label;
}

}
