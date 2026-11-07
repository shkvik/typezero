#pragma once

#include "compiler/load/token/token.h"
#include "compiler/program/diagnostics/diagnostics.h"
#include <string>

namespace tz {

class LiteralText {
public:
  static void decodeTemplateRaw(const Token &tok, std::u16string &out);
  static void decodeRegex(const Token &tok, std::u16string &pattern, std::u16string &flags);
  static bool decodeTemplatePart(const Token &tok, std::u16string &out, Diagnostics &diags);
  static bool decodeStringLiteral(const Token &tok, std::u16string &out, Diagnostics &diags);

private:
  static void appendUtf16(std::u16string &out, uint32_t cp);
  static uint32_t decodeUtf8(llvm::StringRef s, size_t &i);
  static bool parseHex(llvm::StringRef s, size_t &i, size_t count, uint32_t &value);
  static bool parseUnicodeEscape(llvm::StringRef s, size_t &i, uint32_t &cp);
  static bool decodeText(llvm::StringRef raw, bool isTemplate, std::u16string &out,
                         Diagnostics &diags);
  static llvm::StringRef templateText(const Token &tok);
};

}
