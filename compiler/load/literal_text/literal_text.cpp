#include "compiler/load/literal_text/literal_text.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSwitch.h"

namespace tz {

namespace {

static void appendUtf16(std::u16string &out, uint32_t cp) {
  if (cp < 0x10000) {
    out += static_cast<char16_t>(cp);
  } else {
    cp -= 0x10000;
    out += static_cast<char16_t>(0xD800 + (cp >> 10));
    out += static_cast<char16_t>(0xDC00 + (cp & 0x3FF));
  }
}

static uint32_t decodeUtf8(llvm::StringRef s, size_t &i) {
  unsigned char lead = static_cast<unsigned char>(s[i]);
  if (lead < 0x80) {
    ++i;
    return lead;
  }
  size_t extra = lead >= 0xF0 ? 3 : lead >= 0xE0 ? 2 : lead >= 0xC0 ? 1 : 0;
  if (extra == 0 || lead >= 0xF8 || i + extra >= s.size()) {
    ++i;
    return 0xFFFD;
  }
  uint32_t cp = lead & (0x3F >> extra);
  for (size_t k = 1; k <= extra; ++k) {
    unsigned char c = static_cast<unsigned char>(s[i + k]);
    if ((c & 0xC0) != 0x80) {
      ++i;
      return 0xFFFD;
    }
    cp = (cp << 6) | (c & 0x3F);
  }
  i += extra + 1;
  static const uint32_t minimum[] = {0, 0x80, 0x800, 0x10000};
  if (cp < minimum[extra] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
    return 0xFFFD;
  }

  return cp;
}

static bool parseHex(llvm::StringRef s, size_t &i, size_t count, uint32_t &value) {
  value = 0;
  for (size_t n = 0; n < count; ++n, ++i) {
    if (i >= s.size() || !llvm::isHexDigit(s[i])) {
      return false;
    }
    value = value * 16 + llvm::hexDigitValue(s[i]);
  }

  return true;
}

static bool parseUnicodeEscape(llvm::StringRef s, size_t &i, uint32_t &cp) {
  if (i < s.size() && s[i] == '{') {
    ++i;
    cp = 0;
    size_t digits = 0;
    while (i < s.size() && llvm::isHexDigit(s[i])) {
      cp = cp * 16 + llvm::hexDigitValue(s[i++]);
      if (++digits > 6) {
        return false;
      }
    }
    if (digits == 0 || i >= s.size() || s[i] != '}' || cp > 0x10FFFF) {
      return false;
    }
    ++i;
    return true;
  }

  return parseHex(s, i, 4, cp);
}

static bool decodeText(llvm::StringRef raw, bool isTemplate, std::u16string &out,
                       Diagnostics &diags) {
  out.clear();
  for (size_t i = 0; i < raw.size();) {
    char c = raw[i];
    if (isTemplate && c == '\r') {
      out += u'\n';
      i += raw.substr(i).starts_with("\r\n") ? 2 : 1;
      continue;
    }
    if (c != '\\') {
      appendUtf16(out, decodeUtf8(raw, i));
      continue;
    }

    llvm::SMLoc escapeLoc = llvm::SMLoc::getFromPointer(raw.data() + i);
    ++i;
    if (i >= raw.size()) {
      break;
    }
    char e = raw[i++];
    switch (e) {
    case 'n':
      out += u'\n';
      break;
    case 't':
      out += u'\t';
      break;
    case 'r':
      out += u'\r';
      break;
    case 'b':
      out += u'\b';
      break;
    case 'f':
      out += u'\f';
      break;
    case 'v':
      out += u'\v';
      break;
    case '0':
      out += u'\0';
      break;
    case '\r':
      if (i < raw.size() && raw[i] == '\n') {
        ++i;
      }
      break;
    case '\n':
      break;
    case 'x': {
      uint32_t value;
      if (!parseHex(raw, i, 2, value)) {
        diags.error(escapeLoc, "invalid '\\x' escape: expected two hex digits");
        return false;
      }
      out += static_cast<char16_t>(value);
      break;
    }
    case 'u': {
      uint32_t cp;
      if (!parseUnicodeEscape(raw, i, cp)) {
        diags.error(escapeLoc, "invalid '\\u' escape");
        return false;
      }
      appendUtf16(out, cp);
      break;
    }
    default:
      --i;
      appendUtf16(out, decodeUtf8(raw, i));
      break;
    }
  }

  return true;
}

static llvm::StringRef templateText(const Token &tok) {
  llvm::StringRef raw = tok.text.drop_front();
  if (raw.ends_with("${")) {
    return raw.drop_back(2);
  }
  if (raw.ends_with("`")) {
    return raw.drop_back();
  }

  return raw;
}

}

bool decodeStringLiteral(const Token &tok, std::u16string &out, Diagnostics &diags) {
  llvm::StringRef raw = tok.text.drop_front();
  if (!raw.empty() && raw.back() == tok.text.front()) {
    raw = raw.drop_back();
  }

  return decodeText(raw, false, out, diags);
}

bool decodeTemplatePart(const Token &tok, std::u16string &out, Diagnostics &diags) {
  return decodeText(templateText(tok), true, out, diags);
}

void decodeRegex(const Token &tok, std::u16string &pattern, std::u16string &flags) {
  llvm::StringRef text = tok.text.drop_front();
  size_t slash = text.rfind('/');
  if (slash == llvm::StringRef::npos) {
    slash = text.size();
  }
  llvm::StringRef body = text.take_front(slash);
  llvm::StringRef suffix = text.drop_front(std::min(slash + 1, text.size()));
  pattern.clear();
  for (size_t i = 0; i < body.size();) {
    appendUtf16(pattern, decodeUtf8(body, i));
  }
  flags.clear();
  for (size_t i = 0; i < suffix.size();) {
    appendUtf16(flags, decodeUtf8(suffix, i));
  }
}

void decodeTemplateRaw(const Token &tok, std::u16string &out) {
  llvm::StringRef raw = templateText(tok);
  out.clear();
  for (size_t i = 0; i < raw.size();) {
    if (raw[i] == '\r') {
      out += u'\n';
      i += raw.substr(i).starts_with("\r\n") ? 2 : 1;
      continue;
    }
    appendUtf16(out, decodeUtf8(raw, i));
  }
}

}
