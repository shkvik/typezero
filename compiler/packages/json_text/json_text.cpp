#include "compiler/packages/json_text/json_text.h"
#include "llvm/ADT/StringExtras.h"

namespace tz::pkg {

std::string jsonQuote(llvm::StringRef s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (c < 0x20) {
        static const char digits[] = "0123456789abcdef";
        out += "\\u00";
        out += digits[c >> 4];
        out += digits[c & 15];
      } else {
        out += static_cast<char>(c);
      }
    }
  }

  return out + "\"";
}

std::pair<unsigned, unsigned> lineAndColumn(llvm::StringRef text, size_t offset) {
  unsigned line = 1, column = 1;
  for (size_t i = 0; i < offset && i < text.size(); ++i) {
    if (text[i] == '\n') {
      ++line;
      column = 1;
    } else {
      ++column;
    }
  }

  return {line, column};
}

} // namespace tz::pkg
