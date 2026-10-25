#include "compiler/load/expr_parser/expr_parser.h"
#include "compiler/load/call_parser/call_parser.h"
#include "compiler/load/class_parser/class_parser.h"
#include "compiler/load/decl_parser/decl_parser.h"
#include "compiler/load/function_parser/function_parser.h"
#include "compiler/load/module_parser/module_parser.h"
#include "compiler/load/pattern_parser/pattern_parser.h"
#include "compiler/load/stmt_parser/stmt_parser.h"
#include "compiler/load/template_parser/template_parser.h"
#include "compiler/load/to_utf8/to_utf8.h"
#include "compiler/load/token_cursor/token_cursor.h"
#include "compiler/load/type_parser/type_parser.h"
#include "compiler/load/var_parser/var_parser.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/SaveAndRestore.h"

namespace tz {

namespace {

// Binding strength of binary operators, following JavaScript. `as` and
// `satisfies` bind like the relational operators; they are parsed only to
// report that the language has neither.
constexpr int kRelationalPrec = 7;

constexpr int kPowPrec = 11; // `**`, the only right-associative one

int binaryPrec(Tok kind) {
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

BinaryOp binaryOpFor(Tok kind) {
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

// Returns true for assignment operators; `op` is set for compound ones.
bool assignmentOp(Tok kind, std::optional<BinaryOp> &op) {
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

} // namespace

ExprParser::ExprParser(Parser &parser) : parser_(parser) {}

// `yield value`, a bare `yield`, or `yield* iterable`, at the level of an
// assignment. The value is on the same line, or there is none.
Expr *ExprParser::parseYield() {
  llvm::SMLoc loc = parser_.tok_.loc();
  parser_.cursor().advance(); // 'yield'
  bool delegate = !parser_.tok_.newlineBefore && parser_.cursor().accept(Tok::Star);
  Expr *value = nullptr;
  if (delegate) {
    value = parseExpr();
  } else if (!parser_.tok_.newlineBefore) {
    // Only a token that can start an expression starts the value: `yield)`
    // and `yield;` have none.
    switch (parser_.tok_.kind) {
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
      if (binaryPrec(parser_.tok_.kind) < 0 || parser_.tok_.is(Tok::Plus) ||
          parser_.tok_.is(Tok::Minus) || parser_.tok_.is(Tok::Less)) {
        value = parseExpr();
      }
    }
  }
  if (!parser_.inGenerator_) {
    parser_.cursor().report(
        loc, "'yield' can only be used in a generator function, declared with 'function*'");
    return value ? value : parser_.ctx_.make<UndefinedExpr>(loc);
  }

  return parser_.ctx_.make<YieldExpr>(loc, value, delegate);
}

// `a, b, c`
Expr *ExprParser::parseSequence() {
  Expr *first = parseExpr();
  if (!parser_.tok_.is(Tok::Comma)) {
    return first;
  }
  std::vector<Expr *> exprs{first};
  while (parser_.cursor().accept(Tok::Comma)) {
    exprs.push_back(parseExpr());
  }

  return parser_.ctx_.make<SequenceExpr>(first->loc(), std::move(exprs));
}

// Assignment is right-associative and has the lowest precedence after the
// comma.
Expr *ExprParser::parseExpr() {
  const char *start = parser_.tok_.text.begin();
  if (parser_.tok_.is(Tok::Identifier) && parser_.tok_.text == "yield") {
    return parseYield();
  }
  Expr *lhs = parseConditional();
  std::optional<BinaryOp> op;
  if (!assignmentOp(parser_.tok_.kind, op)) {
    return lhs;
  }
  llvm::SMLoc loc = parser_.tok_.loc();
  parser_.cursor().advance();
  Expr *rhs = parseExpr();
  auto *assign = parser_.ctx_.make<AssignExpr>(loc, op, lhs, rhs);
  assign->source = llvm::StringRef(start, static_cast<size_t>(parser_.prevEnd_ - start));

  return assign;
}

Expr *ExprParser::parseConditional() {
  Expr *cond = parseBinary(0);
  if (!parser_.tok_.is(Tok::Question)) {
    return cond;
  }
  llvm::SMLoc loc = parser_.tok_.loc();
  parser_.cursor().advance();
  Expr *thenExpr = parseExpr();
  parser_.cursor().expect(Tok::Colon, "in the conditional expression");
  Expr *elseExpr = parseExpr();

  return parser_.ctx_.make<ConditionalExpr>(loc, cond, thenExpr, elseExpr);
}

Expr *ExprParser::parseBinary(int minPrec) {
  const char *start = parser_.tok_.text.begin();
  // `-x ** 2` is an error in JavaScript: it could mean either `(-x) ** 2` or
  // `-(x ** 2)`.
  llvm::StringRef unary;
  if (parser_.tok_.is(Tok::Minus) || parser_.tok_.is(Tok::Plus) || parser_.tok_.is(Tok::Bang) ||
      parser_.tok_.is(Tok::Tilde) || parser_.tok_.is(Tok::KwTypeof) ||
      parser_.tok_.is(Tok::KwVoid) || parser_.tok_.is(Tok::KwDelete)) {
    unary = parser_.tok_.text;
  }
  Expr *lhs = parseUnary();
  if (!unary.empty() && parser_.tok_.is(Tok::StarStar)) {
    parser_.cursor().report(parser_.tok_.loc(),
                            "the left side of '**' cannot be a unary '" + unary +
                                "' expression; add parentheses, e.g. '(" + unary +
                                (llvm::isAlpha(unary[0]) ? " " : "") + "x) ** 2'");
  }
  for (;;) {
    if (parser_.tok_.is(Tok::KwAs) && kRelationalPrec >= minPrec) {
      llvm::SMLoc loc = parser_.tok_.loc();
      parser_.cursor().advance();
      lhs = parser_.ctx_.make<CastExpr>(loc, lhs, parser_.types().parseType());
      continue;
    }
    // `x satisfies T` checks x against T and leaves its type as it is. tz
    // has no operators on types, as with `as`: the type goes on the variable.
    if (parser_.tok_.is(Tok::Identifier) && parser_.tok_.text == "satisfies" &&
        !parser_.tok_.newlineBefore && kRelationalPrec >= minPrec) {
      llvm::SMLoc loc = parser_.tok_.loc();
      parser_.cursor().advance();
      const char *typeStart = parser_.tok_.text.data();
      parser_.types().parseType();
      llvm::StringRef type(typeStart,
                           static_cast<size_t>(std::max(parser_.prevEnd_, typeStart) - typeStart));
      parser_.cursor().report(
          loc, "'satisfies' is not supported, just as 'as' is not; declare the type of the "
               "variable instead: 'const x: " +
                   type + " = ...'");
      continue;
    }
    if (parser_.tok_.is(Tok::KwIn) && parser_.noIn_) {
      return lhs;
    }
    // `x instanceof C`, a relational operator that lexes as a name.
    if (parser_.tok_.is(Tok::Identifier) && parser_.tok_.text == "instanceof" &&
        kRelationalPrec >= minPrec) {
      llvm::SMLoc loc = parser_.tok_.loc();
      parser_.cursor().advance();
      Expr *rhs = parseBinary(kRelationalPrec + 1);
      auto *bin = parser_.ctx_.make<BinaryExpr>(loc, BinaryOp::InstanceOf, lhs, rhs);
      bin->source = llvm::StringRef(start, static_cast<size_t>(parser_.prevEnd_ - start));
      lhs = bin;
      continue;
    }
    int prec = binaryPrec(parser_.tok_.kind);
    if (prec < 0 || prec < minPrec) {
      return lhs;
    }
    BinaryOp op = binaryOpFor(parser_.tok_.kind);
    llvm::SMLoc loc = parser_.tok_.loc();
    // `==` never converts its operands (both sides must have the same type),
    // so the strict form of JavaScript has nothing left to do.
    if (parser_.tok_.is(Tok::EqEqEq)) {
      parser_.cursor().report(loc,
                              "'===' is not supported; use '==', which does not convert types");
    } else if (parser_.tok_.is(Tok::BangEqEq)) {
      parser_.cursor().report(loc,
                              "'!==' is not supported; use '!=', which does not convert types");
    }
    parser_.cursor().advance();
    Expr *rhs = parseBinary(prec == kPowPrec ? prec : prec + 1);
    auto *bin = parser_.ctx_.make<BinaryExpr>(loc, op, lhs, rhs);
    bin->source = llvm::StringRef(start, static_cast<size_t>(parser_.prevEnd_ - start));
    lhs = bin;
  }
}

Expr *ExprParser::parseUnary() {
  llvm::SMLoc loc = parser_.tok_.loc();
  // `await x`: a file is a module, where `await` is always the operator.
  if (parser_.tok_.is(Tok::Identifier) && parser_.tok_.text == "await") {
    parser_.cursor().advance();
    return parser_.ctx_.make<AwaitExpr>(loc, parseUnary());
  }
  std::optional<UnaryOp> op;
  switch (parser_.tok_.kind) {
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
    bool increment = parser_.tok_.is(Tok::PlusPlus);
    parser_.cursor().advance();
    return parser_.ctx_.make<UpdateExpr>(loc, increment, /*prefix=*/true, parseUnary());
  }
  default:
    return parsePostfix();
  }
  parser_.cursor().advance();

  return parser_.ctx_.make<UnaryExpr>(loc, *op, parseUnary());
}

Expr *ExprParser::parsePostfix() {
  Expr *expr = parser_.calls().parseCallOrMember();
  // A line break before `++`/`--` makes it a prefix operator of the next line.
  if ((parser_.tok_.is(Tok::PlusPlus) || parser_.tok_.is(Tok::MinusMinus)) &&
      !parser_.tok_.newlineBefore) {
    bool increment = parser_.tok_.is(Tok::PlusPlus);
    llvm::SMLoc loc = parser_.tok_.loc();
    parser_.cursor().advance();
    return parser_.ctx_.make<UpdateExpr>(loc, increment, /*prefix=*/false, expr);
  }

  return expr;
}

} // namespace tz
