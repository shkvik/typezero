#include "compiler/load/lexer/lexer.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSwitch.h"

namespace tz {

static bool isIdentStart(char c) {
  // Bytes >= 0x80 belong to UTF-8 sequences; accept them so identifiers may be
  // written in any script.
  return llvm::isAlpha(c) || c == '_' || c == '$' || static_cast<unsigned char>(c) >= 0x80;
}

static bool isIdentChar(char c) { return isIdentStart(c) || llvm::isDigit(c); }

Lexer::Lexer(llvm::StringRef source, Diagnostics &diags)
    : cur_(source.begin()), end_(source.end()), diags_(diags) {
  if (source.starts_with("\xEF\xBB\xBF")) {
    cur_ += 3;
  }
  // A `#!` line lets programs be run as scripts on Unix.
  if (source.substr(cur_ - source.begin()).starts_with("#!")) {
    while (cur_ < end_ && *cur_ != '\n') {
      ++cur_;
    }
  }
}

bool Lexer::match(char c) {
  if (peek() != c) {
    return false;
  }
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
      while (cur_ < end_ && *cur_ != '\n') {
        ++cur_;
      }
    } else if (c == '/' && peek(1) == '*') {
      const char *start = cur_;
      cur_ += 2;
      bool closed = false;
      while (cur_ < end_) {
        if (*cur_ == '\n') {
          newline = true;
        }
        if (*cur_ == '*' && peek(1) == '/') {
          cur_ += 2;
          closed = true;
          break;
        }
        ++cur_;
      }
      if (!closed) {
        diags_.error(llvm::SMLoc::getFromPointer(start), "unterminated comment");
      }
    } else {
      break;
    }
  }

  return newline;
}

Token Lexer::next() {
  bool nl = skipTrivia();
  const char *start = cur_;
  if (cur_ >= end_) {
    return make(Tok::Eof, start, nl);
  }

  char c = *cur_++;
  switch (c) {
  case '(':
    return make(Tok::LParen, start, nl);
  case ')':
    return make(Tok::RParen, start, nl);
  case '{':
    if (!templateBraces_.empty()) {
      ++templateBraces_.back();
    }
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
    // `a?.5:1` is a conditional with the number .5.
    if (peek() == '.' && !llvm::isDigit(peek(1))) {
      ++cur_;
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
      cur_ += 2;
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
    // `#x`, a private name of a class: an identifier that keeps its `#`.
    if (isIdentStart(peek())) {
      return lexIdentifier(start, nl);
    }
    break;
  default:
    break;
  }

  if (llvm::isDigit(c)) {
    return lexNumber(start, nl);
  }
  if (isIdentStart(c)) {
    return lexIdentifier(start, nl);
  }

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
    while (llvm::isAlnum(peek()) || peek() == '_') {
      ++cur_;
    }
    if (cur_[-1] == 'n' && cur_ - start > 3) { // `0xffn`, a bigint (see below)
      --cur_;
    }
  } else {
    if (*start == '.') {
      isFloat = true;
    }
    while (llvm::isDigit(peek()) || peek() == '_') {
      ++cur_;
    }
    // As in JavaScript, the point may end the number: `5.` is 5, and in
    // `5..toString()` the second point reads the property.
    if (!isFloat && peek() == '.' && isIdentStart(peek(1)) && peek(1) != 'e' && peek(1) != 'E') {
      // `5.toString()`: the point belongs to the number, and a name cannot
      // follow it. The point is left to be read as `.`, so that the rest
      // parses as meant and this is the only error.
      llvm::StringRef number(start, static_cast<size_t>(cur_ - start));
      const char *name = cur_ + 1, *nameEnd = name;
      while (nameEnd < end_ && isIdentChar(*nameEnd)) {
        ++nameEnd;
      }
      llvm::StringRef property(name, static_cast<size_t>(nameEnd - name));
      diags_.error(
          llvm::SMLoc::getFromPointer(name),
          "a point right after a number belongs to the number, so a name cannot follow it; "
          "write '" +
              number + ".." + property + "' or '(" + number + ")." + property + "'");
      return make(Tok::IntLiteral, start, newlineBefore);
    }
    if (!isFloat && peek() == '.') {
      isFloat = true;
      ++cur_;
      while (llvm::isDigit(peek()) || peek() == '_') {
        ++cur_;
      }
    }
    char e = peek();
    if ((e == 'e' || e == 'E') && (llvm::isDigit(peek(1)) || ((peek(1) == '+' || peek(1) == '-') &&
                                                              llvm::isDigit(peek(2))))) {
      isFloat = true;
      cur_ += 2;
      while (llvm::isDigit(peek()) || peek() == '_') {
        ++cur_;
      }
    }
  }

  // `10n` is a bigint, which the language replaces with long and decimal.
  if (peek() == 'n' && !isIdentChar(peek(1))) {
    Token tok = make(isFloat ? Tok::FloatLiteral : Tok::IntLiteral, start, newlineBefore);
    diags_.error(llvm::SMLoc::getFromPointer(start),
                 "'" + tok.text +
                     "n' is a bigint, and there is no bigint: integers up to 9.2e18 "
                     "fit in 'long', longer ones in 'decimal' (e.g. 'const n: decimal = " +
                     tok.text + "')");
    ++cur_;
    return tok;
  }
  if (isIdentStart(peek())) {
    diags_.error(llvm::SMLoc::getFromPointer(cur_),
                 "an identifier cannot directly follow a number");
    while (isIdentChar(peek())) {
      ++cur_;
    }
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
    if (c == quote) {
      break;
    }
    if (c == '\\' && cur_ < end_) {
      ++cur_;
    }
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
    if (c == '`') {
      return make(continuation ? Tok::TemplateTail : Tok::TemplateString, start, newlineBefore);
    }
    if (c == '$' && peek() == '{') {
      ++cur_;
      templateBraces_.push_back(0);
      return make(continuation ? Tok::TemplateMiddle : Tok::TemplateHead, start, newlineBefore);
    }
    if (c == '\\' && cur_ < end_) {
      ++cur_;
    }
  }
}

// `/body/flags`: the body ends at the first `/` that is neither escaped nor
// in a class `[...]`, and holds no line break.
Token Lexer::regex(const char *start, bool newlineBefore) {
  cur_ = start + 1;
  bool inClass = false;
  auto lineBreak = [&] {
    return *cur_ == '\n' || *cur_ == '\r' ||
           llvm::StringRef(cur_, static_cast<size_t>(end_ - cur_)).starts_with("\xE2\x80\xA8") ||
           llvm::StringRef(cur_, static_cast<size_t>(end_ - cur_)).starts_with("\xE2\x80\xA9");
  };
  for (;;) {
    if (cur_ >= end_ || lineBreak()) {
      diags_.error(llvm::SMLoc::getFromPointer(start), "unterminated regular expression literal");
      return make(Tok::RegexLiteral, start, newlineBefore);
    }
    char c = *cur_++;
    if (c == '\\') {
      if (cur_ < end_ && !lineBreak()) {
        ++cur_;
      }
    } else if (c == '[') {
      inClass = true;
    } else if (c == ']') {
      inClass = false;
    } else if (c == '/' && !inClass) {
      break;
    }
  }
  while (cur_ < end_ && isIdentChar(*cur_)) {
    ++cur_;
  }

  return make(Tok::RegexLiteral, start, newlineBefore);
}

Token Lexer::lexIdentifier(const char *start, bool newlineBefore) {
  while (isIdentChar(peek())) {
    ++cur_;
  }
  llvm::StringRef text(start, cur_ - start);
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

} // namespace tz
