#include "compiler/packages/json/json.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"

#include <cstdlib>

namespace tz::pkg {

const JsonValue *JsonValue::get(llvm::StringRef key) const {
  for (size_t i = 0; i < keys.size(); ++i)
    if (keys[i] == key)
      return &items[i];
  return nullptr;
}

std::string JsonValue::getString(llvm::StringRef key) const {
  const JsonValue *value = get(key);
  return value && value->isString() ? value->string : std::string();
}

namespace {

class Parser {
public:
  explicit Parser(llvm::StringRef text) : text_(text) {}

  bool parse(JsonValue &out, size_t &errorOffset, std::string &error) {
    bool ok = skip() && value(out, 0) && skip();
    if (ok && at_ < text_.size())
      ok = fail("unexpected text after the end of the JSON value");
    if (!ok) {
      errorOffset = errorAt_;
      error = error_;
    }
    return ok;
  }

private:
  bool fail(const std::string &message) {
    errorAt_ = at_;
    error_ = message;
    return false;
  }
  char peek() const { return at_ < text_.size() ? text_[at_] : '\0'; }

  // Whitespace and comments.
  bool skip() {
    while (at_ < text_.size()) {
      char c = text_[at_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++at_;
      } else if (text_.substr(at_).starts_with("//")) {
        while (at_ < text_.size() && text_[at_] != '\n')
          ++at_;
      } else if (text_.substr(at_).starts_with("/*")) {
        size_t close = text_.find("*/", at_ + 2);
        if (close == llvm::StringRef::npos)
          return fail("unterminated comment");
        at_ = close + 2;
      } else {
        break;
      }
    }
    return true;
  }

  bool value(JsonValue &out, unsigned depth) {
    if (depth > 200)
      return fail("the JSON is nested too deeply");
    out.begin = at_;
    char c = peek();
    bool ok;
    if (c == '{')
      ok = object(out, depth);
    else if (c == '[')
      ok = array(out, depth);
    else if (c == '"') {
      out.kind = JsonValue::Kind::String;
      ok = string(out.string);
    } else if (c == '-' || llvm::isDigit(c))
      ok = number(out);
    else if (text_.substr(at_).starts_with("true")) {
      out.kind = JsonValue::Kind::Bool;
      out.boolean = true;
      at_ += 4;
      ok = true;
    } else if (text_.substr(at_).starts_with("false")) {
      out.kind = JsonValue::Kind::Bool;
      at_ += 5;
      ok = true;
    } else if (text_.substr(at_).starts_with("null")) {
      at_ += 4;
      ok = true;
    } else {
      ok = fail(at_ < text_.size() ? "expected a JSON value" : "unexpected end of the JSON text");
    }
    out.end = at_;
    return ok;
  }

  bool object(JsonValue &out, unsigned depth) {
    out.kind = JsonValue::Kind::Object;
    ++at_;
    while (true) {
      if (!skip())
        return false;
      if (peek() == '}') {
        ++at_;
        return true;
      }
      if (peek() != '"')
        return fail("expected a key in double quotes, or '}'");
      size_t keyAt = at_;
      std::string key;
      if (!string(key) || !skip())
        return false;
      if (peek() != ':')
        return fail("expected ':' after the key");
      ++at_;
      if (!skip())
        return false;
      out.keys.push_back(std::move(key));
      out.keyOffsets.push_back(keyAt);
      out.items.emplace_back();
      if (!value(out.items.back(), depth + 1) || !skip())
        return false;
      if (peek() == ',') {
        ++at_;
        continue;
      }
      if (peek() == '}') {
        ++at_;
        return true;
      }
      return fail("expected ',' or '}'");
    }
  }

  bool array(JsonValue &out, unsigned depth) {
    out.kind = JsonValue::Kind::Array;
    ++at_;
    while (true) {
      if (!skip())
        return false;
      if (peek() == ']') {
        ++at_;
        return true;
      }
      out.items.emplace_back();
      if (!value(out.items.back(), depth + 1) || !skip())
        return false;
      if (peek() == ',') {
        ++at_;
        continue;
      }
      if (peek() == ']') {
        ++at_;
        return true;
      }
      return fail("expected ',' or ']'");
    }
  }

  bool number(JsonValue &out) {
    out.kind = JsonValue::Kind::Number;
    size_t start = at_;
    if (peek() == '-')
      ++at_;
    while (llvm::isDigit(peek()) || peek() == '.' || peek() == 'e' || peek() == 'E' ||
           peek() == '+' || peek() == '-')
      ++at_;
    std::string digits = text_.slice(start, at_).str();
    char *endp = nullptr;
    out.number = std::strtod(digits.c_str(), &endp);
    if (digits.empty() || endp != digits.c_str() + digits.size()) {
      at_ = start;
      return fail("malformed number");
    }
    return true;
  }

  bool hex4(unsigned &code) {
    if (at_ + 4 > text_.size())
      return fail("expected four hex digits after '\\u'");
    code = 0;
    for (int i = 0; i < 4; ++i) {
      unsigned digit = llvm::hexDigitValue(text_[at_ + i]);
      if (digit == ~0U)
        return fail("expected four hex digits after '\\u'");
      code = code * 16 + digit;
    }
    at_ += 4;
    return true;
  }

  bool string(std::string &out) {
    ++at_; // '"'
    while (true) {
      if (at_ >= text_.size())
        return fail("unterminated string");
      char c = text_[at_];
      if (c == '"') {
        ++at_;
        return true;
      }
      if (c == '\n')
        return fail("unterminated string");
      if (c != '\\') {
        out += c;
        ++at_;
        continue;
      }
      ++at_;
      char e = peek();
      ++at_;
      switch (e) {
      case '"': out += '"'; break;
      case '\\': out += '\\'; break;
      case '/': out += '/'; break;
      case 'b': out += '\b'; break;
      case 'f': out += '\f'; break;
      case 'n': out += '\n'; break;
      case 'r': out += '\r'; break;
      case 't': out += '\t'; break;
      case 'u': {
        unsigned code;
        if (!hex4(code))
          return false;
        // A surrogate pair is one code point.
        if (code >= 0xD800 && code < 0xDC00 && text_.substr(at_).starts_with("\\u")) {
          at_ += 2;
          unsigned low;
          if (!hex4(low))
            return false;
          if (low >= 0xDC00 && low < 0xE000)
            code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
        }
        char buffer[UNI_MAX_UTF8_BYTES_PER_CODE_POINT];
        char *p = buffer;
        if (!llvm::ConvertCodePointToUTF8(code, p))
          return fail("invalid '\\u' escape");
        out.append(buffer, p);
        break;
      }
      default:
        --at_;
        return fail("invalid escape in a string");
      }
    }
  }

  llvm::StringRef text_;
  size_t at_ = 0;
  size_t errorAt_ = 0;
  std::string error_;
};

} // namespace

bool parseJson(llvm::StringRef text, JsonValue &out, size_t &errorOffset, std::string &error) {
  out = JsonValue();
  return Parser(text).parse(out, errorOffset, error);
}

std::string jsonQuote(llvm::StringRef s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    switch (c) {
    case '"': out += "\\\""; break;
    case '\\': out += "\\\\"; break;
    case '\n': out += "\\n"; break;
    case '\r': out += "\\r"; break;
    case '\t': out += "\\t"; break;
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
