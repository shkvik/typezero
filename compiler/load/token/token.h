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
  TemplateString,
  TemplateHead,
  TemplateMiddle,
  TemplateTail,
  RegexLiteral,

  KwAs,
  KwBreak,
  KwCase,
  KwConst,
  KwContinue,
  KwDebugger,
  KwDefault,
  KwDelete,
  KwDo,
  KwElse,
  KwEnum,
  KwFalse,
  KwFor,
  KwFunction,
  KwIf,
  KwIn,
  KwLet,
  KwNew,
  KwReturn,
  KwSwitch,
  KwTrue,
  KwTypeof,
  KwVar,
  KwVoid,
  KwWhile,

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
  QuestionDot,
  Arrow,
  At,

  Plus,
  Minus,
  Star,
  StarStar,
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
  StarStarAssign,
  SlashAssign,
  PercentAssign,
  AmpAssign,
  PipeAssign,
  CaretAssign,
  ShlAssign,
  ShrAssign,
  UShrAssign,
  QuestionQuestionAssign,
  AmpAmpAssign,
  PipePipeAssign,
};

const char *tokSpelling(Tok kind);

inline bool isKeyword(Tok kind) { return kind >= Tok::KwAs && kind <= Tok::KwWhile; }

struct Token {
  Tok kind = Tok::Eof;
  llvm::StringRef text;
  bool newlineBefore = false;
  bool hash = false;

  bool is(Tok k) const { return kind == k; }
  llvm::SMLoc loc() const { return llvm::SMLoc::getFromPointer(text.data()); }
};

}
