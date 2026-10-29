#include "compiler/packages/json_text/json_text.h"
#include "llvm/ADT/StringExtras.h"
#include <vector>

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

JsonProblem checkJsonText(llvm::StringRef s) {
  size_t i = 0, n = s.size();
  auto whitespace = [&] {
    while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) {
      ++i;
    }
  };
  auto digits = [&] {
    size_t start = i;
    while (i < n && llvm::isDigit(s[i])) {
      ++i;
    }

    return i > start;
  };
  auto problem = [&](std::string message) { return JsonProblem{std::move(message), i}; };
  auto string = [&]() -> std::string {
    ++i;
    for (;;) {
      if (i >= n) {
        return "the string is not closed";
      }
      unsigned char c = static_cast<unsigned char>(s[i]);
      if (c == '"') {
        ++i;
        return "";
      }
      if (c < 0x20) {
        return "a control character in a string must be escaped, e.g. '\\n' for a line break";
      }
      if (c != '\\') {
        ++i;
        continue;
      }
      if (++i >= n) {
        return "the string is not closed";
      }
      char e = s[i];
      if (e == 'u') {
        for (int k = 0; k < 4; ++k) {
          if (++i >= n || !llvm::isHexDigit(s[i])) {
            return "'\\u' needs four hexadecimal digits";
          }
        }
      } else if (!llvm::StringRef("\"\\/bfnrt").contains(e)) {
        return "'\\" + std::string(1, e) + "' is not an escape of JSON";
      }
      ++i;
    }
  };
  auto key = [&]() -> std::string {
    whitespace();
    if (i >= n || s[i] != '"') {
      return "expected a property name in double quotes";
    }
    if (std::string m = string(); !m.empty()) {
      return m;
    }
    whitespace();
    if (i >= n || s[i] != ':') {
      return "expected ':' after a property name";
    }
    ++i;

    return "";
  };

  std::vector<char> open;
  for (;;) {
    whitespace();
    if (i >= n) {
      return problem("the text ends where a value should be");
    }
    char c = s[i];
    if (c == '"') {
      size_t start = i;
      if (std::string m = string(); !m.empty()) {
        JsonProblem p = problem(m);
        if (m == "the string is not closed") {
          p.offset = start;
        }
        return p;
      }
    } else if (c == '-' || llvm::isDigit(c)) {
      if (c == '-') {
        ++i;
      }
      if (i < n && s[i] == '0') {
        ++i;
        if (i < n && llvm::isDigit(s[i])) {
          return problem("a number cannot start with 0 followed by more digits");
        }
      } else if (!digits()) {
        return problem("expected digits after '-'");
      }
      if (i < n && s[i] == '.') {
        ++i;
        if (!digits()) {
          return problem("expected digits after the decimal point");
        }
      }
      if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        ++i;
        if (i < n && (s[i] == '+' || s[i] == '-')) {
          ++i;
        }
        if (!digits()) {
          return problem("expected digits in the exponent");
        }
      }
    } else if (c == '{' || c == '[') {
      ++i;
      whitespace();
      if (i < n && s[i] == (c == '{' ? '}' : ']')) {
        ++i;
      } else {
        open.push_back(c);
        if (c == '{') {
          if (std::string m = key(); !m.empty()) {
            return problem(m);
          }
        }
        continue;
      }
    } else if (s.substr(i).starts_with("true") || s.substr(i).starts_with("null")) {
      i += 4;
    } else if (s.substr(i).starts_with("false")) {
      i += 5;
    } else {
      unsigned char u = static_cast<unsigned char>(c);
      return problem(u >= 0x21 && u < 0x7F ? "expected a value, found '" + std::string(1, c) + "'"
                                           : std::string("expected a value"));
    }
    for (;;) {
      whitespace();
      if (open.empty()) {
        if (i < n) {
          return problem("unexpected text after the value");
        }
        return {};
      }
      char container = open.back();
      if (i < n && s[i] == ',') {
        ++i;
        if (container == '{') {
          if (std::string m = key(); !m.empty()) {
            return problem(m);
          }
        }
        break;
      }
      if (i < n && s[i] == (container == '{' ? '}' : ']')) {
        ++i;
        open.pop_back();
        continue;
      }
      return problem(container == '{' ? "expected ',' or '}' after a property value"
                                      : "expected ',' or ']' after an array element");
    }
  }
}

}
