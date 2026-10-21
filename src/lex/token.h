#pragma once

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/SMLoc.h"

namespace tz {

enum class Tok {
  Eof,
  Identifier,
  IntLiteral,
  FloatLiteral,
  StringLiteral,
  // Template literals: `text` is a TemplateString; `a${x}b${y}c` lexes as
  // TemplateHead `a${, the tokens of x, TemplateMiddle }b${, the tokens of y,
  // TemplateTail }c`.
  TemplateString,
  TemplateHead,
  TemplateMiddle,
  TemplateTail,

  // Keywords
  KwAs,
  KwBreak,
  KwConst,
  KwContinue,
  KwDo,
  KwElse,
  KwFalse,
  KwFor,
  KwFunction,
  KwIf,
  KwLet,
  KwNew,
  KwReturn,
  KwTrue,
  KwVar,
  KwWhile,

  // Punctuation
  LParen,
  RParen,
  LBrace,
  RBrace,
  LBracket,
  RBracket,
  Comma,
  Semi,
  Colon,
  Dot,
  Ellipsis,
  Question,
  QuestionQuestion,

  // Operators
  Plus,
  Minus,
  Star,
  Slash,
  Percent,
  PlusPlus,
  MinusMinus,
  Amp,
  Pipe,
  Caret,
  Tilde,
  Bang,
  AmpAmp,
  PipePipe,
  Shl,
  Shr,
  UShr,
  Less,
  LessEq,
  Greater,
  GreaterEq,
  EqEq,
  EqEqEq,
  BangEq,
  BangEqEq,
  Assign,
  PlusAssign,
  MinusAssign,
  StarAssign,
  SlashAssign,
  PercentAssign,
  AmpAssign,
  PipeAssign,
  CaretAssign,
  ShlAssign,
  ShrAssign,
  UShrAssign,
  QuestionQuestionAssign,
};

// How a token kind is shown in diagnostics, e.g. "'('" or "identifier".
const char *tokSpelling(Tok kind);

struct Token {
  Tok kind = Tok::Eof;
  // The exact source text; it points into the source buffer.
  llvm::StringRef text;
  // A line break separates this token from the previous one. Drives automatic
  // semicolon insertion.
  bool newlineBefore = false;

  bool is(Tok k) const { return kind == k; }
  llvm::SMLoc loc() const { return llvm::SMLoc::getFromPointer(text.data()); }
};

} // namespace tz
