#include "compiler/load/expr_parser/expr_parser.h"
#include "compiler/load/call_parser/call_parser.h"
#include "compiler/load/class_parser/class_parser.h"
#include "compiler/load/decl_parser/decl_parser.h"
#include "compiler/load/function_parser/function_parser.h"
#include "compiler/load/module_parser/module_parser.h"
#include "compiler/load/pattern_parser/pattern_parser.h"
#include "compiler/load/stmt_parser/stmt_parser.h"
#include "compiler/load/template_parser/template_parser.h"
#include "compiler/load/token_cursor/token_cursor.h"
#include "compiler/load/type_parser/type_parser.h"
#include "compiler/load/var_parser/var_parser.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/SaveAndRestore.h"

namespace tz {

namespace {

constexpr int kRelationalPrec = 7;

constexpr int kPowPrec = 11;

}

ExprParser::ExprParser(Parser &parser)
    : parser(parser) {}

int ExprParser::binaryPrec(Tok kind) {
  switch (kind) {
  case Tok::PipePipe:
  case Tok::QuestionQuestion:
    return 1;
  case Tok::AmpAmp:
    return 2;
  case Tok::Pipe:
    return 3;
  case Tok::Caret:
    return 4;
  case Tok::Amp:
    return 5;
  case Tok::EqEq:
  case Tok::EqEqEq:
  case Tok::BangEq:
  case Tok::BangEqEq:
    return 6;
  case Tok::Less:
  case Tok::LessEq:
  case Tok::Greater:
  case Tok::GreaterEq:
  case Tok::KwIn:
    return kRelationalPrec;
  case Tok::Shl:
  case Tok::Shr:
  case Tok::UShr:
    return 8;
  case Tok::Plus:
  case Tok::Minus:
    return 9;
  case Tok::Star:
  case Tok::Slash:
  case Tok::Percent:
    return 10;
  case Tok::StarStar:
    return kPowPrec;
  default:
    return -1;
  }
}

BinaryOp ExprParser::binaryOpFor(Tok kind) {
  switch (kind) {
  case Tok::PipePipe:
    return BinaryOp::LogOr;
  case Tok::QuestionQuestion:
    return BinaryOp::Nullish;
  case Tok::AmpAmp:
    return BinaryOp::LogAnd;
  case Tok::Pipe:
    return BinaryOp::BitOr;
  case Tok::Caret:
    return BinaryOp::BitXor;
  case Tok::Amp:
    return BinaryOp::BitAnd;
  case Tok::EqEq:
  case Tok::EqEqEq:
    return BinaryOp::Eq;
  case Tok::BangEq:
  case Tok::BangEqEq:
    return BinaryOp::Ne;
  case Tok::Less:
    return BinaryOp::Lt;
  case Tok::LessEq:
    return BinaryOp::Le;
  case Tok::Greater:
    return BinaryOp::Gt;
  case Tok::GreaterEq:
    return BinaryOp::Ge;
  case Tok::KwIn:
    return BinaryOp::In;
  case Tok::Shl:
    return BinaryOp::Shl;
  case Tok::Shr:
    return BinaryOp::Shr;
  case Tok::UShr:
    return BinaryOp::UShr;
  case Tok::Plus:
    return BinaryOp::Add;
  case Tok::Minus:
    return BinaryOp::Sub;
  case Tok::Star:
    return BinaryOp::Mul;
  case Tok::StarStar:
    return BinaryOp::Pow;
  case Tok::Slash:
    return BinaryOp::Div;
  default:
    return BinaryOp::Rem;
  }
}

bool ExprParser::assignmentOp(Tok kind, std::optional<BinaryOp> &op) {
  switch (kind) {
  case Tok::Assign:
    op.reset();
    return true;
  case Tok::PlusAssign:
    op = BinaryOp::Add;
    return true;
  case Tok::MinusAssign:
    op = BinaryOp::Sub;
    return true;
  case Tok::StarAssign:
    op = BinaryOp::Mul;
    return true;
  case Tok::StarStarAssign:
    op = BinaryOp::Pow;
    return true;
  case Tok::SlashAssign:
    op = BinaryOp::Div;
    return true;
  case Tok::PercentAssign:
    op = BinaryOp::Rem;
    return true;
  case Tok::AmpAssign:
    op = BinaryOp::BitAnd;
    return true;
  case Tok::PipeAssign:
    op = BinaryOp::BitOr;
    return true;
  case Tok::CaretAssign:
    op = BinaryOp::BitXor;
    return true;
  case Tok::ShlAssign:
    op = BinaryOp::Shl;
    return true;
  case Tok::ShrAssign:
    op = BinaryOp::Shr;
    return true;
  case Tok::UShrAssign:
    op = BinaryOp::UShr;
    return true;
  case Tok::QuestionQuestionAssign:
    op = BinaryOp::Nullish;
    return true;
  case Tok::AmpAmpAssign:
    op = BinaryOp::LogAnd;
    return true;
  case Tok::PipePipeAssign:
    op = BinaryOp::LogOr;
    return true;
  default:
    return false;
  }
}

Expr *ExprParser::parseYield() {
  llvm::SMLoc loc = this->parser.tok.loc();
  this->parser.cursor().advance();
  bool delegate = !this->parser.tok.newlineBefore && this->parser.cursor().accept(Tok::Star);
  Expr *value = nullptr;
  if (delegate) {
    value = parseExpr();
  } else if (!this->parser.tok.newlineBefore) {
    switch (this->parser.tok.kind) {
      case Tok::RParen:
      case Tok::RBracket:
      case Tok::RBrace:
      case Tok::Comma:
      case Tok::Semi:
      case Tok::Colon:
      case Tok::Question:
      case Tok::Eof:
      case Tok::TemplateMiddle:
      case Tok::TemplateTail:
        break;
      default:
        if (ExprParser::binaryPrec(this->parser.tok.kind) < 0 || this->parser.tok.is(Tok::Plus) ||
            this->parser.tok.is(Tok::Minus) || this->parser.tok.is(Tok::Less)) {
          value = parseExpr();
        }
    }
  }
  if (!this->parser.inGenerator) {
    this->parser.cursor().report(
        loc, "'yield' can only be used in a generator function, declared with 'function*'");
    return value ? value : this->parser.ctx.make<UndefinedExpr>(loc);
  }

  return this->parser.ctx.make<YieldExpr>(loc, value, delegate);
}

Expr *ExprParser::parseSequence() {
  Expr *first = parseExpr();
  if (!this->parser.tok.is(Tok::Comma)) {
    return first;
  }
  std::vector<Expr *> exprs{first};
  while (this->parser.cursor().accept(Tok::Comma)) {
    exprs.push_back(parseExpr());
  }

  return this->parser.ctx.make<SequenceExpr>(first->loc(), std::move(exprs));
}

Expr *ExprParser::parseExpr() {
  const char *start = this->parser.tok.text.begin();
  if (this->parser.tok.is(Tok::Identifier) && this->parser.tok.text == "yield") {
    return parseYield();
  }
  Expr *lhs = parseConditional();
  std::optional<BinaryOp> op;
  if (!ExprParser::assignmentOp(this->parser.tok.kind, op)) {
    return lhs;
  }
  llvm::SMLoc loc = this->parser.tok.loc();
  this->parser.cursor().advance();
  Expr *rhs = parseExpr();
  auto *assign = this->parser.ctx.make<AssignExpr>(loc, op, lhs, rhs);
  assign->source = llvm::StringRef(start, static_cast<size_t>(this->parser.prevEnd - start));

  return assign;
}

Expr *ExprParser::parseConditional() {
  Expr *cond = parseBinary(0);
  if (!this->parser.tok.is(Tok::Question)) {
    return cond;
  }
  llvm::SMLoc loc = this->parser.tok.loc();
  this->parser.cursor().advance();
  Expr *thenExpr = parseExpr();
  this->parser.cursor().expect(Tok::Colon, "in the conditional expression");
  Expr *elseExpr = parseExpr();

  return this->parser.ctx.make<ConditionalExpr>(loc, cond, thenExpr, elseExpr);
}

Expr *ExprParser::parseBinary(int minPrec) {
  const char *start = this->parser.tok.text.begin();
  llvm::StringRef unary;
  if (this->parser.tok.is(Tok::Minus) || this->parser.tok.is(Tok::Plus) ||
      this->parser.tok.is(Tok::Bang) || this->parser.tok.is(Tok::Tilde) ||
      this->parser.tok.is(Tok::KwTypeof) || this->parser.tok.is(Tok::KwVoid) ||
      this->parser.tok.is(Tok::KwDelete)) {
    unary = this->parser.tok.text;
  }
  Expr *lhs = parseUnary();
  if (!unary.empty() && this->parser.tok.is(Tok::StarStar)) {
    this->parser.cursor().report(this->parser.tok.loc(),
                                 "the left side of '**' cannot be a unary '" + unary +
                                     "' expression; add parentheses, e.g. '(" + unary +
                                     (llvm::isAlpha(unary[0]) ? " " : "") + "x) ** 2'");
  }
  for (;;) {
    if (this->parser.tok.is(Tok::KwAs) && kRelationalPrec >= minPrec) {
      llvm::SMLoc loc = this->parser.tok.loc();
      this->parser.cursor().advance();
      lhs = this->parser.ctx.make<CastExpr>(loc, lhs, this->parser.types().parseType());
      continue;
    }
    if (this->parser.tok.is(Tok::Identifier) && this->parser.tok.text == "satisfies" &&
        !this->parser.tok.newlineBefore && kRelationalPrec >= minPrec) {
      llvm::SMLoc loc = this->parser.tok.loc();
      this->parser.cursor().advance();
      const char *typeStart = this->parser.tok.text.data();
      this->parser.types().parseType();
      llvm::StringRef type(
          typeStart, static_cast<size_t>(std::max(this->parser.prevEnd, typeStart) - typeStart));
      this->parser.cursor().report(
          loc, "'satisfies' is not supported, just as 'as' is not; declare the type of the "
               "variable instead: 'const x: " +
                   type + " = ...'");
      continue;
    }
    if (this->parser.tok.is(Tok::KwIn) && this->parser.noIn) {
      return lhs;
    }
    if (this->parser.tok.is(Tok::Identifier) && this->parser.tok.text == "instanceof" &&
        kRelationalPrec >= minPrec) {
      llvm::SMLoc loc = this->parser.tok.loc();
      this->parser.cursor().advance();
      Expr *rhs = parseBinary(kRelationalPrec + 1);
      auto *bin = this->parser.ctx.make<BinaryExpr>(loc, BinaryOp::InstanceOf, lhs, rhs);
      bin->source = llvm::StringRef(start, static_cast<size_t>(this->parser.prevEnd - start));
      lhs = bin;
      continue;
    }
    int prec = ExprParser::binaryPrec(this->parser.tok.kind);
    if (prec < 0 || prec < minPrec) {
      return lhs;
    }
    BinaryOp op = ExprParser::binaryOpFor(this->parser.tok.kind);
    llvm::SMLoc loc = this->parser.tok.loc();
    if (this->parser.tok.is(Tok::EqEqEq)) {
      this->parser.cursor().report(
          loc, "'===' is not supported; use '==', which does not convert types");
    } else if (this->parser.tok.is(Tok::BangEqEq)) {
      this->parser.cursor().report(
          loc, "'!==' is not supported; use '!=', which does not convert types");
    }
    this->parser.cursor().advance();
    Expr *rhs = parseBinary(prec == kPowPrec ? prec : prec + 1);
    auto *bin = this->parser.ctx.make<BinaryExpr>(loc, op, lhs, rhs);
    bin->source = llvm::StringRef(start, static_cast<size_t>(this->parser.prevEnd - start));
    lhs = bin;
  }
}

Expr *ExprParser::parseUnary() {
  llvm::SMLoc loc = this->parser.tok.loc();
  if (this->parser.tok.is(Tok::Identifier) && this->parser.tok.text == "await") {
    this->parser.cursor().advance();
    return this->parser.ctx.make<AwaitExpr>(loc, parseUnary());
  }
  std::optional<UnaryOp> op;
  switch (this->parser.tok.kind) {
    case Tok::Minus:
      op = UnaryOp::Neg;
      break;
    case Tok::Plus:
      op = UnaryOp::Plus;
      break;
    case Tok::Bang:
      op = UnaryOp::Not;
      break;
    case Tok::Tilde:
      op = UnaryOp::BitNot;
      break;
    case Tok::KwTypeof:
      op = UnaryOp::TypeOf;
      break;
    case Tok::KwVoid:
      op = UnaryOp::Void;
      break;
    case Tok::KwDelete:
      op = UnaryOp::Delete;
      break;
    case Tok::PlusPlus:
    case Tok::MinusMinus: {
      bool increment = this->parser.tok.is(Tok::PlusPlus);
      this->parser.cursor().advance();
      return this->parser.ctx.make<UpdateExpr>(loc, increment, true, parseUnary());
    }
    default:
      return parsePostfix();
  }
  this->parser.cursor().advance();

  return this->parser.ctx.make<UnaryExpr>(loc, *op, parseUnary());
}

Expr *ExprParser::parsePostfix() {
  Expr *expr = this->parser.calls().parseCallOrMember();
  if ((this->parser.tok.is(Tok::PlusPlus) || this->parser.tok.is(Tok::MinusMinus)) &&
      !this->parser.tok.newlineBefore) {
    bool increment = this->parser.tok.is(Tok::PlusPlus);
    llvm::SMLoc loc = this->parser.tok.loc();
    this->parser.cursor().advance();
    return this->parser.ctx.make<UpdateExpr>(loc, increment, false, expr);
  }

  return expr;
}

}
