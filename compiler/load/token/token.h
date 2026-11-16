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

  KwAbstract,
  KwAccessor,
  KwAs,
  KwAsync,
  KwBreak,
  KwCase,
  KwClass,
  KwConst,
  KwConstructor,
  KwContinue,
  KwDebugger,
  KwDeclare,
  KwDefault,
  KwDelete,
  KwDo,
  KwElse,
  KwEnum,
  KwExport,
  KwExtends,
  KwFalse,
  KwFor,
  KwFrom,
  KwFunction,
  KwGet,
  KwIf,
  KwImplements,
  KwImport,
  KwIn,
  KwInstanceof,
  KwInterface,
  KwLet,
  KwModule,
  KwNamespace,
  KwNew,
  KwOf,
  KwOverride,
  KwPrivate,
  KwProtected,
  KwPublic,
  KwReadonly,
  KwReturn,
  KwSet,
  KwStatic,
  KwSwitch,
  KwTrue,
  KwType,
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

struct Token {
  Tok kind = Tok::Eof;
  llvm::StringRef text;
  bool newlineBefore = false;
  bool hash = false;

  bool is(Tok k) const { return kind == k; }
  bool isName() const;
  llvm::SMLoc loc() const { return llvm::SMLoc::getFromPointer(text.data()); }
};

class TokenKinds {
public:
  static bool isKeyword(Tok kind);
  static bool isContextual(Tok kind);
  static const char *tokSpelling(Tok kind);
};

}
