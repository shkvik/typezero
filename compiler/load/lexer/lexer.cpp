#include "compiler/load/lexer/lexer.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSwitch.h"

namespace tz {

Lexer::Lexer(llvm::StringRef source, Diagnostics &diags)
    : cur(source.begin()),
      end(source.end()),
      diags(diags) {
  if (source.starts_with("\xEF\xBB\xBF")) {
    this->cur += 3;
  }
  if (source.substr(this->cur - source.begin()).starts_with("#!")) {
    while (this->cur < this->end && *this->cur != '\n') {
      ++this->cur;
    }
  }
}

bool Lexer::isIdentStart(char c) {
  return llvm::isAlpha(c) || c == '_' || c == '$' || static_cast<unsigned char>(c) >= 0x80;
}

bool Lexer::isIdentChar(char c) {
  return Lexer::isIdentStart(c) || llvm::isDigit(c);
}

bool Lexer::match(char c) {
  if (peek() != c) {
    return false;
  }
  ++this->cur;

  return true;
}

Token Lexer::make(Tok kind, const char *start, bool newlineBefore) const {
  Token tok;
  tok.kind = kind;
  tok.text = llvm::StringRef(start, this->cur - start);
  tok.newlineBefore = newlineBefore;

  return tok;
}

bool Lexer::skipTrivia() {
  bool newline = false;
  while (this->cur < this->end) {
    char c = *this->cur;
    if (c == '\n') {
      newline = true;
      ++this->cur;
    } else if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
      ++this->cur;
    } else if (c == '/' && peek(1) == '/') {
      while (this->cur < this->end && *this->cur != '\n') {
        ++this->cur;
      }
    } else if (c == '/' && peek(1) == '*') {
      const char *start = this->cur;
      this->cur += 2;
      bool closed = false;
      while (this->cur < this->end) {
        if (*this->cur == '\n') {
          newline = true;
        }
        if (*this->cur == '*' && peek(1) == '/') {
          this->cur += 2;
          closed = true;
          break;
        }
        ++this->cur;
      }
      if (!closed) {
        this->diags.error(llvm::SMLoc::getFromPointer(start), "unterminated comment");
      }
    } else {
      break;
    }
  }

  return newline;
}

Token Lexer::next() {
  bool nl = skipTrivia();
  const char *start = this->cur;
  if (this->cur >= this->end) {
    return make(Tok::Eof, start, nl);
  }

  char c = *this->cur++;
  switch (c) {
  case '(':
    return make(Tok::LParen, start, nl);
  case ')':
    return make(Tok::RParen, start, nl);
  case '{':
    if (!this->templateBraces.empty()) {
      ++this->templateBraces.back();
    }
    return make(Tok::LBrace, start, nl);
  case '}':
    if (!this->templateBraces.empty()) {
      if (this->templateBraces.back() == 0) {
        this->templateBraces.pop_back();
        return lexTemplate(start, true, nl);
      }
      --this->templateBraces.back();
    }
    return make(Tok::RBrace, start, nl);
  case '`':
    return lexTemplate(start, false, nl);
  case '[':
    return make(Tok::LBracket, start, nl);
  case ']':
    return make(Tok::RBracket, start, nl);
  case ',':
    return make(Tok::Comma, start, nl);
  case ';':
    return make(Tok::Semi, start, nl);
  case ':':
    return make(Tok::Colon, start, nl);
  case '?':
    if (match('?')) {
      return make(match('=') ? Tok::QuestionQuestionAssign : Tok::QuestionQuestion, start, nl);
    }
    if (peek() == '.' && !llvm::isDigit(peek(1))) {
      ++this->cur;
      return make(Tok::QuestionDot, start, nl);
    }
    return make(Tok::Question, start, nl);
  case '~':
    return make(Tok::Tilde, start, nl);
  case '.':
    if (llvm::isDigit(peek())) {
      return lexNumber(start, nl);
    }
    if (peek() == '.' && peek(1) == '.') {
      this->cur += 2;
      return make(Tok::Ellipsis, start, nl);
    }
    return make(Tok::Dot, start, nl);
  case '+':
    if (match('+')) {
      return make(Tok::PlusPlus, start, nl);
    }
    if (match('=')) {
      return make(Tok::PlusAssign, start, nl);
    }
    return make(Tok::Plus, start, nl);
  case '-':
    if (match('-')) {
      return make(Tok::MinusMinus, start, nl);
    }
    if (match('=')) {
      return make(Tok::MinusAssign, start, nl);
    }
    return make(Tok::Minus, start, nl);
  case '*':
    if (match('*')) {
      return make(match('=') ? Tok::StarStarAssign : Tok::StarStar, start, nl);
    }
    if (match('=')) {
      return make(Tok::StarAssign, start, nl);
    }
    return make(Tok::Star, start, nl);
  case '/':
    if (match('=')) {
      return make(Tok::SlashAssign, start, nl);
    }
    return make(Tok::Slash, start, nl);
  case '%':
    if (match('=')) {
      return make(Tok::PercentAssign, start, nl);
    }
    return make(Tok::Percent, start, nl);
  case '&':
    if (match('&')) {
      return make(match('=') ? Tok::AmpAmpAssign : Tok::AmpAmp, start, nl);
    }
    if (match('=')) {
      return make(Tok::AmpAssign, start, nl);
    }
    return make(Tok::Amp, start, nl);
  case '|':
    if (match('|')) {
      return make(match('=') ? Tok::PipePipeAssign : Tok::PipePipe, start, nl);
    }
    if (match('=')) {
      return make(Tok::PipeAssign, start, nl);
    }
    return make(Tok::Pipe, start, nl);
  case '^':
    if (match('=')) {
      return make(Tok::CaretAssign, start, nl);
    }
    return make(Tok::Caret, start, nl);
  case '!':
    if (match('=')) {
      return make(match('=') ? Tok::BangEqEq : Tok::BangEq, start, nl);
    }
    return make(Tok::Bang, start, nl);
  case '=':
    if (match('=')) {
      return make(match('=') ? Tok::EqEqEq : Tok::EqEq, start, nl);
    }
    if (match('>')) {
      return make(Tok::Arrow, start, nl);
    }
    return make(Tok::Assign, start, nl);
  case '<':
    if (match('<')) {
      return make(match('=') ? Tok::ShlAssign : Tok::Shl, start, nl);
    }
    if (match('=')) {
      return make(Tok::LessEq, start, nl);
    }
    return make(Tok::Less, start, nl);
  case '>':
    if (match('>')) {
      if (match('>')) {
        return make(match('=') ? Tok::UShrAssign : Tok::UShr, start, nl);
      }
      return make(match('=') ? Tok::ShrAssign : Tok::Shr, start, nl);
    }
    if (match('=')) {
      return make(Tok::GreaterEq, start, nl);
    }
    return make(Tok::Greater, start, nl);
  case '"':
  case '\'':
    return lexString(start, c, nl);
  case '@':
    return make(Tok::At, start, nl);
  case '#':
    if (Lexer::isIdentStart(peek())) {
      Token tok = lexIdentifier(start + 1, nl);
      tok.kind = Tok::Identifier;
      tok.hash = true;

      return tok;
    }
    break;
  default:
    break;
  }

  if (llvm::isDigit(c)) {
    return lexNumber(start, nl);
  }
  if (Lexer::isIdentStart(c)) {
    return lexIdentifier(start, nl);
  }

  this->diags.error(llvm::SMLoc::getFromPointer(start),
                    "unexpected character '" + llvm::Twine(llvm::StringRef(start, 1)) + "'");

  return next();
}

Token Lexer::lexNumber(const char *start, bool newlineBefore) {
  bool isFloat = false;
  char second = peek();
  if (*start == '0' && (second == 'x' || second == 'X' || second == 'b' || second == 'B' ||
                        second == 'o' || second == 'O')) {
    ++this->cur;
    while (llvm::isAlnum(peek()) || peek() == '_') {
      ++this->cur;
    }
    if (this->cur[-1] == 'n' && this->cur - start > 3) {
      --this->cur;
    }
  } else {
    if (*start == '.') {
      isFloat = true;
    }
    while (llvm::isDigit(peek()) || peek() == '_') {
      ++this->cur;
    }
    if (!isFloat && peek() == '.' && Lexer::isIdentStart(peek(1)) && peek(1) != 'e' &&
        peek(1) != 'E') {
      llvm::StringRef number(start, static_cast<size_t>(this->cur - start));
      const char *name = this->cur + 1, *nameEnd = name;
      while (nameEnd < this->end && Lexer::isIdentChar(*nameEnd)) {
        ++nameEnd;
      }
      llvm::StringRef property(name, static_cast<size_t>(nameEnd - name));
      this->diags.error(
          llvm::SMLoc::getFromPointer(name),
          "a point right after a number belongs to the number, so a name cannot follow it; "
          "write '" +
              number + ".." + property + "' or '(" + number + ")." + property + "'");
      return make(Tok::IntLiteral, start, newlineBefore);
    }
    if (!isFloat && peek() == '.') {
      isFloat = true;
      ++this->cur;
      while (llvm::isDigit(peek()) || peek() == '_') {
        ++this->cur;
      }
    }
    char e = peek();
    if ((e == 'e' || e == 'E') && (llvm::isDigit(peek(1)) || ((peek(1) == '+' || peek(1) == '-') &&
                                                              llvm::isDigit(peek(2))))) {
      isFloat = true;
      this->cur += 2;
      while (llvm::isDigit(peek()) || peek() == '_') {
        ++this->cur;
      }
    }
  }

  if (peek() == 'n' && !Lexer::isIdentChar(peek(1))) {
    Token tok = make(isFloat ? Tok::FloatLiteral : Tok::IntLiteral, start, newlineBefore);
    this->diags.error(llvm::SMLoc::getFromPointer(start),
                      "'" + tok.text +
                          "n' is a bigint, and there is no bigint: integers up to 9.2e18 "
                          "fit in 'int', longer ones in 'decimal' (e.g. 'const n: decimal = " +
                          tok.text + "')");
    ++this->cur;
    return tok;
  }
  if (Lexer::isIdentStart(peek())) {
    this->diags.error(llvm::SMLoc::getFromPointer(this->cur),
                      "an identifier cannot directly follow a number");
    while (Lexer::isIdentChar(peek())) {
      ++this->cur;
    }
  }

  return make(isFloat ? Tok::FloatLiteral : Tok::IntLiteral, start, newlineBefore);
}

Token Lexer::lexString(const char *start, char quote, bool newlineBefore) {
  for (;;) {
    if (this->cur >= this->end || *this->cur == '\n' || *this->cur == '\r') {
      this->diags.error(llvm::SMLoc::getFromPointer(start), "unterminated string literal");
      break;
    }
    char c = *this->cur++;
    if (c == quote) {
      break;
    }
    if (c == '\\' && this->cur < this->end) {
      ++this->cur;
    }
  }

  return make(Tok::StringLiteral, start, newlineBefore);
}

Token Lexer::lexTemplate(const char *start, bool continuation, bool newlineBefore) {
  for (;;) {
    if (this->cur >= this->end) {
      this->diags.error(llvm::SMLoc::getFromPointer(start), "unterminated template literal");
      return make(continuation ? Tok::TemplateTail : Tok::TemplateString, start, newlineBefore);
    }
    char c = *this->cur++;
    if (c == '`') {
      return make(continuation ? Tok::TemplateTail : Tok::TemplateString, start, newlineBefore);
    }
    if (c == '$' && peek() == '{') {
      ++this->cur;
      this->templateBraces.push_back(0);
      return make(continuation ? Tok::TemplateMiddle : Tok::TemplateHead, start, newlineBefore);
    }
    if (c == '\\' && this->cur < this->end) {
      ++this->cur;
    }
  }
}

Token Lexer::regex(const char *start, bool newlineBefore) {
  this->cur = start + 1;
  bool inClass = false;
  auto lineBreak = [&] {
    return *this->cur == '\n' || *this->cur == '\r' ||
           llvm::StringRef(this->cur, static_cast<size_t>(this->end - this->cur))
               .starts_with("\xE2\x80\xA8") ||
           llvm::StringRef(this->cur, static_cast<size_t>(this->end - this->cur))
               .starts_with("\xE2\x80\xA9");
  };
  for (;;) {
    if (this->cur >= this->end || lineBreak()) {
      this->diags.error(llvm::SMLoc::getFromPointer(start),
                        "unterminated regular expression literal");
      return make(Tok::RegexLiteral, start, newlineBefore);
    }
    char c = *this->cur++;
    if (c == '\\') {
      if (this->cur < this->end && !lineBreak()) {
        ++this->cur;
      }
    } else if (c == '[') {
      inClass = true;
    } else if (c == ']') {
      inClass = false;
    } else if (c == '/' && !inClass) {
      break;
    }
  }
  while (this->cur < this->end && Lexer::isIdentChar(*this->cur)) {
    ++this->cur;
  }

  return make(Tok::RegexLiteral, start, newlineBefore);
}

Token Lexer::lexIdentifier(const char *start, bool newlineBefore) {
  while (Lexer::isIdentChar(peek())) {
    ++this->cur;
  }
  llvm::StringRef text(start, this->cur - start);
  Tok kind = llvm::StringSwitch<Tok>(text)
                 .Case("as", Tok::KwAs)
                 .Case("break", Tok::KwBreak)
                 .Case("case", Tok::KwCase)
                 .Case("const", Tok::KwConst)
                 .Case("continue", Tok::KwContinue)
                 .Case("debugger", Tok::KwDebugger)
                 .Case("default", Tok::KwDefault)
                 .Case("delete", Tok::KwDelete)
                 .Case("do", Tok::KwDo)
                 .Case("else", Tok::KwElse)
                 .Case("enum", Tok::KwEnum)
                 .Case("false", Tok::KwFalse)
                 .Case("for", Tok::KwFor)
                 .Case("function", Tok::KwFunction)
                 .Case("if", Tok::KwIf)
                 .Case("in", Tok::KwIn)
                 .Case("let", Tok::KwLet)
                 .Case("new", Tok::KwNew)
                 .Case("return", Tok::KwReturn)
                 .Case("switch", Tok::KwSwitch)
                 .Case("true", Tok::KwTrue)
                 .Case("typeof", Tok::KwTypeof)
                 .Case("var", Tok::KwVar)
                 .Case("void", Tok::KwVoid)
                 .Case("while", Tok::KwWhile)
                 .Default(Tok::Identifier);

  return make(kind, start, newlineBefore);
}

}
