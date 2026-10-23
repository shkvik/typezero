#include "lex/lexer.h"

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
  case Tok::TemplateString:
  case Tok::TemplateHead: return "template literal";
  case Tok::TemplateMiddle:
  case Tok::TemplateTail: return "'}'";
  case Tok::KwAs: return "'as'";
  case Tok::KwBreak: return "'break'";
  case Tok::KwCase: return "'case'";
  case Tok::KwConst: return "'const'";
  case Tok::KwContinue: return "'continue'";
  case Tok::KwDefault: return "'default'";
  case Tok::KwDo: return "'do'";
  case Tok::KwElse: return "'else'";
  case Tok::KwEnum: return "'enum'";
  case Tok::KwFalse: return "'false'";
  case Tok::KwFor: return "'for'";
  case Tok::KwFunction: return "'function'";
  case Tok::KwIf: return "'if'";
  case Tok::KwLet: return "'let'";
  case Tok::KwNew: return "'new'";
  case Tok::KwReturn: return "'return'";
  case Tok::KwSwitch: return "'switch'";
  case Tok::KwTrue: return "'true'";
  case Tok::KwTypeof: return "'typeof'";
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
  case Tok::Ellipsis: return "'...'";
  case Tok::Question: return "'?'";
  case Tok::QuestionQuestion: return "'??'";
  case Tok::QuestionDot: return "'?.'";
  case Tok::Arrow: return "'=>'";
  case Tok::Plus: return "'+'";
  case Tok::Minus: return "'-'";
  case Tok::Star: return "'*'";
  case Tok::StarStar: return "'**'";
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
  case Tok::StarStarAssign: return "'**='";
  case Tok::SlashAssign: return "'/='";
  case Tok::PercentAssign: return "'%='";
  case Tok::AmpAssign: return "'&='";
  case Tok::PipeAssign: return "'|='";
  case Tok::CaretAssign: return "'^='";
  case Tok::ShlAssign: return "'<<='";
  case Tok::ShrAssign: return "'>>='";
  case Tok::UShrAssign: return "'>>>='";
  case Tok::QuestionQuestionAssign: return "'??='";
  case Tok::AmpAmpAssign: return "'&&='";
  case Tok::PipePipeAssign: return "'||='";
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
  case '{':
    if (!templateBraces_.empty())
      ++templateBraces_.back();
    return make(Tok::LBrace, start, nl);
  case '}':
    if (!templateBraces_.empty()) {
      if (templateBraces_.back() == 0) {
        templateBraces_.pop_back();
        return lexTemplate(start, /*continuation=*/true, nl);
      }
      --templateBraces_.back();
    }
    return make(Tok::RBrace, start, nl);
  case '`':
    return lexTemplate(start, /*continuation=*/false, nl);
  case '[': return make(Tok::LBracket, start, nl);
  case ']': return make(Tok::RBracket, start, nl);
  case ',': return make(Tok::Comma, start, nl);
  case ';': return make(Tok::Semi, start, nl);
  case ':': return make(Tok::Colon, start, nl);
  case '?':
    if (match('?'))
      return make(match('=') ? Tok::QuestionQuestionAssign : Tok::QuestionQuestion, start, nl);
    // `a?.5:1` is a conditional with the number .5.
    if (peek() == '.' && !llvm::isDigit(peek(1))) {
      ++cur_;
      return make(Tok::QuestionDot, start, nl);
    }
    return make(Tok::Question, start, nl);
  case '~': return make(Tok::Tilde, start, nl);
  case '.':
    if (llvm::isDigit(peek()))
      return lexNumber(start, nl);
    if (peek() == '.' && peek(1) == '.') {
      cur_ += 2;
      return make(Tok::Ellipsis, start, nl);
    }
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
    if (match('*'))
      return make(match('=') ? Tok::StarStarAssign : Tok::StarStar, start, nl);
    if (match('=')) return make(Tok::StarAssign, start, nl);
    return make(Tok::Star, start, nl);
  case '/':
    if (match('=')) return make(Tok::SlashAssign, start, nl);
    return make(Tok::Slash, start, nl);
  case '%':
    if (match('=')) return make(Tok::PercentAssign, start, nl);
    return make(Tok::Percent, start, nl);
  case '&':
    if (match('&')) return make(match('=') ? Tok::AmpAmpAssign : Tok::AmpAmp, start, nl);
    if (match('=')) return make(Tok::AmpAssign, start, nl);
    return make(Tok::Amp, start, nl);
  case '|':
    if (match('|')) return make(match('=') ? Tok::PipePipeAssign : Tok::PipePipe, start, nl);
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
    if (match('>'))
      return make(Tok::Arrow, start, nl);
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

Token Lexer::lexTemplate(const char *start, bool continuation, bool newlineBefore) {
  for (;;) {
    if (cur_ >= end_) {
      diags_.error(llvm::SMLoc::getFromPointer(start), "unterminated template literal");
      return make(continuation ? Tok::TemplateTail : Tok::TemplateString, start, newlineBefore);
    }
    char c = *cur_++;
    if (c == '`')
      return make(continuation ? Tok::TemplateTail : Tok::TemplateString, start, newlineBefore);
    if (c == '$' && peek() == '{') {
      ++cur_;
      templateBraces_.push_back(0);
      return make(continuation ? Tok::TemplateMiddle : Tok::TemplateHead, start, newlineBefore);
    }
    if (c == '\\' && cur_ < end_)
      ++cur_;
  }
}

Token Lexer::lexIdentifier(const char *start, bool newlineBefore) {
  while (isIdentChar(peek()))
    ++cur_;
  llvm::StringRef text(start, cur_ - start);
  Tok kind = llvm::StringSwitch<Tok>(text)
                 .Case("as", Tok::KwAs)
                 .Case("break", Tok::KwBreak)
                 .Case("case", Tok::KwCase)
                 .Case("const", Tok::KwConst)
                 .Case("continue", Tok::KwContinue)
                 .Case("default", Tok::KwDefault)
                 .Case("do", Tok::KwDo)
                 .Case("else", Tok::KwElse)
                 .Case("enum", Tok::KwEnum)
                 .Case("false", Tok::KwFalse)
                 .Case("for", Tok::KwFor)
                 .Case("function", Tok::KwFunction)
                 .Case("if", Tok::KwIf)
                 .Case("let", Tok::KwLet)
                 .Case("new", Tok::KwNew)
                 .Case("return", Tok::KwReturn)
                 .Case("switch", Tok::KwSwitch)
                 .Case("true", Tok::KwTrue)
                 .Case("typeof", Tok::KwTypeof)
                 .Case("var", Tok::KwVar)
                 .Case("while", Tok::KwWhile)
                 .Default(Tok::Identifier);
  return make(kind, start, newlineBefore);
}

//===----------------------------------------------------------------------===//
// String literal decoding
//===----------------------------------------------------------------------===//

static void appendUtf16(std::u16string &out, uint32_t cp) {
  if (cp < 0x10000) {
    out += static_cast<char16_t>(cp);
  } else {
    cp -= 0x10000;
    out += static_cast<char16_t>(0xD800 + (cp >> 10));
    out += static_cast<char16_t>(0xDC00 + (cp & 0x3FF));
  }
}

// Decodes one UTF-8 sequence of the source text at `s[i]`, advancing `i`.
// Malformed input becomes U+FFFD.
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
  if (cp < minimum[extra] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
    return 0xFFFD;
  return cp;
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

// Decodes literal text (without its delimiters). Templates keep raw line
// breaks, normalized to "\n" as JavaScript requires.
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
    if (i >= raw.size())
      break;
    char e = raw[i++];
    switch (e) {
    case 'n': out += u'\n'; break;
    case 't': out += u'\t'; break;
    case 'r': out += u'\r'; break;
    case 'b': out += u'\b'; break;
    case 'f': out += u'\f'; break;
    case 'v': out += u'\v'; break;
    case '0': out += u'\0'; break;
    case '\r': // line continuation
      if (i < raw.size() && raw[i] == '\n')
        ++i;
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
      // `😀` naturally yields a surrogate pair, and a lone
      // surrogate stays one code unit, exactly as in JavaScript.
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

bool decodeStringLiteral(const Token &tok, std::u16string &out, Diagnostics &diags) {
  llvm::StringRef raw = tok.text.drop_front();
  if (!raw.empty() && raw.back() == tok.text.front())
    raw = raw.drop_back();
  return decodeText(raw, /*isTemplate=*/false, out, diags);
}

bool decodeTemplatePart(const Token &tok, std::u16string &out, Diagnostics &diags) {
  // Strip the leading '`' or '}' and the trailing '`' or '${'.
  llvm::StringRef raw = tok.text.drop_front();
  if (raw.ends_with("${"))
    raw = raw.drop_back(2);
  else if (raw.ends_with("`"))
    raw = raw.drop_back();
  return decodeText(raw, /*isTemplate=*/true, out, diags);
}

} // namespace tz
