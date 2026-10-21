#include "lex/Lexer.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSwitch.h"

namespace tz {

const char *tokSpelling(Tok kind) {
  switch (kind) {
  case Tok::Eof: return "end of file";
  case Tok::Identifier: return "identifier";
  case Tok::IntLiteral: return "integer literal";
  case Tok::FloatLiteral: return "number literal";
  case Tok::StringLiteral: return "string literal";
  case Tok::KwAs: return "'as'";
  case Tok::KwBreak: return "'break'";
  case Tok::KwConst: return "'const'";
  case Tok::KwContinue: return "'continue'";
  case Tok::KwDo: return "'do'";
  case Tok::KwElse: return "'else'";
  case Tok::KwFalse: return "'false'";
  case Tok::KwFor: return "'for'";
  case Tok::KwFunction: return "'function'";
  case Tok::KwIf: return "'if'";
  case Tok::KwLet: return "'let'";
  case Tok::KwReturn: return "'return'";
  case Tok::KwTrue: return "'true'";
  case Tok::KwVar: return "'var'";
  case Tok::KwWhile: return "'while'";
  case Tok::LParen: return "'('";
  case Tok::RParen: return "')'";
  case Tok::LBrace: return "'{'";
  case Tok::RBrace: return "'}'";
  case Tok::LBracket: return "'['";
  case Tok::RBracket: return "']'";
  case Tok::Comma: return "','";
  case Tok::Semi: return "';'";
  case Tok::Colon: return "':'";
  case Tok::Dot: return "'.'";
  case Tok::Question: return "'?'";
  case Tok::Plus: return "'+'";
  case Tok::Minus: return "'-'";
  case Tok::Star: return "'*'";
  case Tok::Slash: return "'/'";
  case Tok::Percent: return "'%'";
  case Tok::PlusPlus: return "'++'";
  case Tok::MinusMinus: return "'--'";
  case Tok::Amp: return "'&'";
  case Tok::Pipe: return "'|'";
  case Tok::Caret: return "'^'";
  case Tok::Tilde: return "'~'";
  case Tok::Bang: return "'!'";
  case Tok::AmpAmp: return "'&&'";
  case Tok::PipePipe: return "'||'";
  case Tok::Shl: return "'<<'";
  case Tok::Shr: return "'>>'";
  case Tok::UShr: return "'>>>'";
  case Tok::Less: return "'<'";
  case Tok::LessEq: return "'<='";
  case Tok::Greater: return "'>'";
  case Tok::GreaterEq: return "'>='";
  case Tok::EqEq: return "'=='";
  case Tok::EqEqEq: return "'==='";
  case Tok::BangEq: return "'!='";
  case Tok::BangEqEq: return "'!=='";
  case Tok::Assign: return "'='";
  case Tok::PlusAssign: return "'+='";
  case Tok::MinusAssign: return "'-='";
  case Tok::StarAssign: return "'*='";
  case Tok::SlashAssign: return "'/='";
  case Tok::PercentAssign: return "'%='";
  case Tok::AmpAssign: return "'&='";
  case Tok::PipeAssign: return "'|='";
  case Tok::CaretAssign: return "'^='";
  case Tok::ShlAssign: return "'<<='";
  case Tok::ShrAssign: return "'>>='";
  case Tok::UShrAssign: return "'>>>='";
  }
  return "token";
}

static bool isIdentStart(char c) {
  // Bytes >= 0x80 belong to UTF-8 sequences; accept them so identifiers may be
  // written in any script.
  return llvm::isAlpha(c) || c == '_' || c == '$' || static_cast<unsigned char>(c) >= 0x80;
}

static bool isIdentChar(char c) { return isIdentStart(c) || llvm::isDigit(c); }

Lexer::Lexer(llvm::StringRef source, Diagnostics &diags)
    : cur_(source.begin()), end_(source.end()), diags_(diags) {
  if (source.starts_with("\xEF\xBB\xBF"))
    cur_ += 3;
  // A `#!` line lets programs be run as scripts on Unix.
  if (source.substr(cur_ - source.begin()).starts_with("#!"))
    while (cur_ < end_ && *cur_ != '\n')
      ++cur_;
}

bool Lexer::match(char c) {
  if (peek() != c)
    return false;
  ++cur_;
  return true;
}

Token Lexer::make(Tok kind, const char *start, bool newlineBefore) const {
  Token tok;
  tok.kind = kind;
  tok.text = llvm::StringRef(start, cur_ - start);
  tok.newlineBefore = newlineBefore;
  return tok;
}

bool Lexer::skipTrivia() {
  bool newline = false;
  while (cur_ < end_) {
    char c = *cur_;
    if (c == '\n') {
      newline = true;
      ++cur_;
    } else if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
      ++cur_;
    } else if (c == '/' && peek(1) == '/') {
      while (cur_ < end_ && *cur_ != '\n')
        ++cur_;
    } else if (c == '/' && peek(1) == '*') {
      const char *start = cur_;
      cur_ += 2;
      bool closed = false;
      while (cur_ < end_) {
        if (*cur_ == '\n')
          newline = true;
        if (*cur_ == '*' && peek(1) == '/') {
          cur_ += 2;
          closed = true;
          break;
        }
        ++cur_;
      }
      if (!closed)
        diags_.error(llvm::SMLoc::getFromPointer(start), "unterminated comment");
    } else {
      break;
    }
  }
  return newline;
}

Token Lexer::next() {
  bool nl = skipTrivia();
  const char *start = cur_;
  if (cur_ >= end_)
    return make(Tok::Eof, start, nl);

  char c = *cur_++;
  switch (c) {
  case '(': return make(Tok::LParen, start, nl);
  case ')': return make(Tok::RParen, start, nl);
  case '{': return make(Tok::LBrace, start, nl);
  case '}': return make(Tok::RBrace, start, nl);
  case '[': return make(Tok::LBracket, start, nl);
  case ']': return make(Tok::RBracket, start, nl);
  case ',': return make(Tok::Comma, start, nl);
  case ';': return make(Tok::Semi, start, nl);
  case ':': return make(Tok::Colon, start, nl);
  case '?': return make(Tok::Question, start, nl);
  case '~': return make(Tok::Tilde, start, nl);
  case '.':
    if (llvm::isDigit(peek()))
      return lexNumber(start, nl);
    return make(Tok::Dot, start, nl);
  case '+':
    if (match('+')) return make(Tok::PlusPlus, start, nl);
    if (match('=')) return make(Tok::PlusAssign, start, nl);
    return make(Tok::Plus, start, nl);
  case '-':
    if (match('-')) return make(Tok::MinusMinus, start, nl);
    if (match('=')) return make(Tok::MinusAssign, start, nl);
    return make(Tok::Minus, start, nl);
  case '*':
    if (match('=')) return make(Tok::StarAssign, start, nl);
    return make(Tok::Star, start, nl);
  case '/':
    if (match('=')) return make(Tok::SlashAssign, start, nl);
    return make(Tok::Slash, start, nl);
  case '%':
    if (match('=')) return make(Tok::PercentAssign, start, nl);
    return make(Tok::Percent, start, nl);
  case '&':
    if (match('&')) return make(Tok::AmpAmp, start, nl);
    if (match('=')) return make(Tok::AmpAssign, start, nl);
    return make(Tok::Amp, start, nl);
  case '|':
    if (match('|')) return make(Tok::PipePipe, start, nl);
    if (match('=')) return make(Tok::PipeAssign, start, nl);
    return make(Tok::Pipe, start, nl);
  case '^':
    if (match('=')) return make(Tok::CaretAssign, start, nl);
    return make(Tok::Caret, start, nl);
  case '!':
    if (match('='))
      return make(match('=') ? Tok::BangEqEq : Tok::BangEq, start, nl);
    return make(Tok::Bang, start, nl);
  case '=':
    if (match('='))
      return make(match('=') ? Tok::EqEqEq : Tok::EqEq, start, nl);
    return make(Tok::Assign, start, nl);
  case '<':
    if (match('<'))
      return make(match('=') ? Tok::ShlAssign : Tok::Shl, start, nl);
    if (match('=')) return make(Tok::LessEq, start, nl);
    return make(Tok::Less, start, nl);
  case '>':
    if (match('>')) {
      if (match('>'))
        return make(match('=') ? Tok::UShrAssign : Tok::UShr, start, nl);
      return make(match('=') ? Tok::ShrAssign : Tok::Shr, start, nl);
    }
    if (match('=')) return make(Tok::GreaterEq, start, nl);
    return make(Tok::Greater, start, nl);
  case '"':
  case '\'':
    return lexString(start, c, nl);
  default:
    break;
  }

  if (llvm::isDigit(c))
    return lexNumber(start, nl);
  if (isIdentStart(c))
    return lexIdentifier(start, nl);

  diags_.error(llvm::SMLoc::getFromPointer(start),
               "unexpected character '" + llvm::Twine(llvm::StringRef(start, 1)) + "'");
  return next();
}

Token Lexer::lexNumber(const char *start, bool newlineBefore) {
  bool isFloat = false;
  char second = peek();
  if (*start == '0' && (second == 'x' || second == 'X' || second == 'b' || second == 'B' ||
                        second == 'o' || second == 'O')) {
    ++cur_;
    while (llvm::isAlnum(peek()) || peek() == '_')
      ++cur_;
  } else {
    if (*start == '.')
      isFloat = true;
    while (llvm::isDigit(peek()) || peek() == '_')
      ++cur_;
    if (!isFloat && peek() == '.' && llvm::isDigit(peek(1))) {
      isFloat = true;
      ++cur_;
      while (llvm::isDigit(peek()) || peek() == '_')
        ++cur_;
    }
    char e = peek();
    if ((e == 'e' || e == 'E') &&
        (llvm::isDigit(peek(1)) || ((peek(1) == '+' || peek(1) == '-') && llvm::isDigit(peek(2))))) {
      isFloat = true;
      cur_ += 2;
      while (llvm::isDigit(peek()) || peek() == '_')
        ++cur_;
    }
  }

  if (isIdentStart(peek())) {
    diags_.error(llvm::SMLoc::getFromPointer(cur_), "an identifier cannot directly follow a number");
    while (isIdentChar(peek()))
      ++cur_;
  }
  return make(isFloat ? Tok::FloatLiteral : Tok::IntLiteral, start, newlineBefore);
}

Token Lexer::lexString(const char *start, char quote, bool newlineBefore) {
  for (;;) {
    if (cur_ >= end_ || *cur_ == '\n' || *cur_ == '\r') {
      diags_.error(llvm::SMLoc::getFromPointer(start), "unterminated string literal");
      break;
    }
    char c = *cur_++;
    if (c == quote)
      break;
    if (c == '\\' && cur_ < end_)
      ++cur_;
  }
  return make(Tok::StringLiteral, start, newlineBefore);
}

Token Lexer::lexIdentifier(const char *start, bool newlineBefore) {
  while (isIdentChar(peek()))
    ++cur_;
  llvm::StringRef text(start, cur_ - start);
  Tok kind = llvm::StringSwitch<Tok>(text)
                 .Case("as", Tok::KwAs)
                 .Case("break", Tok::KwBreak)
                 .Case("const", Tok::KwConst)
                 .Case("continue", Tok::KwContinue)
                 .Case("do", Tok::KwDo)
                 .Case("else", Tok::KwElse)
                 .Case("false", Tok::KwFalse)
                 .Case("for", Tok::KwFor)
                 .Case("function", Tok::KwFunction)
                 .Case("if", Tok::KwIf)
                 .Case("let", Tok::KwLet)
                 .Case("return", Tok::KwReturn)
                 .Case("true", Tok::KwTrue)
                 .Case("var", Tok::KwVar)
                 .Case("while", Tok::KwWhile)
                 .Default(Tok::Identifier);
  return make(kind, start, newlineBefore);
}

//===----------------------------------------------------------------------===//
// String literal decoding
//===----------------------------------------------------------------------===//

static void appendUtf8(std::string &out, uint32_t cp) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

// Parses exactly `count` hex digits at `s[i]`, advancing `i`.
static bool parseHex(llvm::StringRef s, size_t &i, size_t count, uint32_t &value) {
  value = 0;
  for (size_t n = 0; n < count; ++n, ++i) {
    if (i >= s.size() || !llvm::isHexDigit(s[i]))
      return false;
    value = value * 16 + llvm::hexDigitValue(s[i]);
  }
  return true;
}

// Parses the code point after `\u`: either `XXXX` or `{X...}`.
static bool parseUnicodeEscape(llvm::StringRef s, size_t &i, uint32_t &cp) {
  if (i < s.size() && s[i] == '{') {
    ++i;
    cp = 0;
    size_t digits = 0;
    while (i < s.size() && llvm::isHexDigit(s[i])) {
      cp = cp * 16 + llvm::hexDigitValue(s[i++]);
      if (++digits > 6)
        return false;
    }
    if (digits == 0 || i >= s.size() || s[i] != '}' || cp > 0x10FFFF)
      return false;
    ++i;
    return true;
  }
  return parseHex(s, i, 4, cp);
}

bool decodeStringLiteral(const Token &tok, std::string &out, Diagnostics &diags) {
  llvm::StringRef raw = tok.text.drop_front();
  if (!raw.empty() && raw.back() == tok.text.front())
    raw = raw.drop_back();

  out.clear();
  for (size_t i = 0; i < raw.size();) {
    char c = raw[i];
    if (c != '\\') {
      out += c;
      ++i;
      continue;
    }

    llvm::SMLoc escapeLoc = llvm::SMLoc::getFromPointer(raw.data() + i);
    ++i;
    if (i >= raw.size())
      break;
    char e = raw[i++];
    switch (e) {
    case 'n': out += '\n'; break;
    case 't': out += '\t'; break;
    case 'r': out += '\r'; break;
    case 'b': out += '\b'; break;
    case 'f': out += '\f'; break;
    case 'v': out += '\v'; break;
    case '0': out += '\0'; break;
    case '\n': break; // line continuation
    case 'x': {
      uint32_t value;
      if (!parseHex(raw, i, 2, value)) {
        diags.error(escapeLoc, "invalid '\\x' escape: expected two hex digits");
        return false;
      }
      appendUtf8(out, value);
      break;
    }
    case 'u': {
      uint32_t cp;
      if (!parseUnicodeEscape(raw, i, cp)) {
        diags.error(escapeLoc, "invalid '\\u' escape");
        return false;
      }
      // Join a UTF-16 surrogate pair written as two escapes.
      if (cp >= 0xD800 && cp <= 0xDBFF && raw.substr(i).starts_with("\\u")) {
        size_t j = i + 2;
        uint32_t low;
        if (parseUnicodeEscape(raw, j, low) && low >= 0xDC00 && low <= 0xDFFF) {
          cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
          i = j;
        }
      }
      if (cp >= 0xD800 && cp <= 0xDFFF)
        cp = 0xFFFD; // a lone surrogate cannot be encoded in UTF-8
      appendUtf8(out, cp);
      break;
    }
    default:
      out += e;
      break;
    }
  }
  return true;
}

} // namespace tz
