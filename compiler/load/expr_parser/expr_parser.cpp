#include "compiler/load/expr_parser/expr_parser.h"
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

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

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
  Expr *expr = parseCallOrMember();
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

// Calls, `.name`, `[index]`, `x!`, and their optional forms `?.(`, `?.name`
// and `?.[`. After a `?.` the rest of them form an optional chain. Also
// tagged templates. A property name may be a keyword, as in `set.delete(x)`.
Expr *ExprParser::parseCallOrMember() {
  Expr *expr = parsePrimary();
  bool chain = false;
  auto finish = [&]() -> Expr * {
    return chain ? parser_.ctx_.make<OptionalChainExpr>(expr->loc(), expr) : expr;
  };
  for (;;) {
    // `f<int>(x)` and `f<int>`: the type arguments of a generic function.
    if (auto *name = dyn_cast<NameExpr>(expr); name && !chain && parser_.tok_.is(Tok::Less) &&
                                               name->typeArgs.empty() &&
                                               parser_.types().tryTypeArgs(name->typeArgs)) {
      continue;
    }
    // `JSON.parse<User>(text)`: type arguments of a method, right before its
    // call.
    if (auto *member = dyn_cast<MemberExpr>(expr);
        member && parser_.tok_.is(Tok::Less) && member->typeArgs.empty() &&
        parser_.types().tryTypeArgs(member->typeArgs, /*callOnly=*/true)) {
      continue;
    }
    bool optional = false;
    if (parser_.tok_.is(Tok::QuestionDot)) {
      optional = chain = true;
      parser_.cursor().advance();
      if (parser_.tok_.is(Tok::Identifier) || isKeyword(parser_.tok_.kind)) {
        auto *member = parser_.ctx_.make<MemberExpr>(parser_.tok_.loc(), expr, parser_.tok_.text);
        member->optional = true;
        expr = member;
        parser_.cursor().advance();
        continue;
      }
      if (!parser_.tok_.is(Tok::LParen) && !parser_.tok_.is(Tok::LBracket)) {
        parser_.cursor().error(parser_.tok_.loc(),
                               "expected a property name, '[' or '(' after '?.'");
        return finish();
      }
    }
    if (parser_.tok_.is(Tok::LParen)) {
      // Diagnostics about a call point at its start, e.g. `f` in `f(x)`.
      llvm::SMLoc loc = expr->loc();
      parser_.cursor().advance();
      llvm::SaveAndRestore<bool> allowIn(parser_.noIn_, false);
      std::vector<Expr *> args;
      std::vector<bool> spread;
      while (!parser_.tok_.is(Tok::RParen) && !parser_.tok_.is(Tok::Eof)) {
        bool spreads = parser_.cursor().accept(Tok::Ellipsis);
        if (spreads || !spread.empty()) {
          spread.resize(args.size(), false);
          spread.push_back(spreads);
        }
        args.push_back(parseExpr());
        if (!parser_.cursor().accept(Tok::Comma)) {
          break;
        }
      }
      parser_.cursor().expect(Tok::RParen, "to close the argument list");
      auto *call = parser_.ctx_.make<CallExpr>(loc, expr, std::move(args));
      call->optional = optional;
      call->spread = std::move(spread);
      expr = call;
    } else if (parser_.tok_.is(Tok::Dot)) {
      parser_.cursor().advance();
      llvm::SMLoc loc = parser_.tok_.loc();
      if (!parser_.tok_.is(Tok::Identifier) && !isKeyword(parser_.tok_.kind)) {
        parser_.cursor().error(parser_.tok_.loc(), "expected a property name after '.'");
        return finish();
      }
      expr = parser_.ctx_.make<MemberExpr>(loc, expr, parser_.tok_.text);
      parser_.cursor().advance();
    } else if (parser_.tok_.is(Tok::LBracket)) {
      llvm::SMLoc loc = parser_.tok_.loc();
      parser_.cursor().advance();
      llvm::SaveAndRestore<bool> allowIn(parser_.noIn_, false);
      Expr *index = parseSequence();
      parser_.cursor().expect(Tok::RBracket, "to close the index");
      auto *access = parser_.ctx_.make<IndexExpr>(loc, expr, index);
      access->optional = optional;
      expr = access;
    } else if (parser_.tok_.is(Tok::TemplateString) || parser_.tok_.is(Tok::TemplateHead)) {
      // tag`a${x}b${y}c` calls the tag with the texts around the values, as
      // one array, and then the values: tag(["a", "b", "c"], x, y). String.raw
      // gets the texts as written, escapes and all.
      if (chain) {
        parser_.cursor().report(
            parser_.tok_.loc(),
            "a tagged template cannot be part of an optional chain; check the tag "
            "before the template");
      }
      auto *member = dyn_cast<MemberExpr>(expr);
      auto *object = member ? dyn_cast<NameExpr>(member->object) : nullptr;
      bool raw = object && object->name == "String" && member->name == "raw";
      llvm::SMLoc loc = expr->loc();
      TemplateExpr *tmpl = parser_.templates().parseTemplate(raw);
      auto *texts = parser_.ctx_.make<ArrayLitExpr>(tmpl->loc());
      for (std::u16string &text : tmpl->quasis) {
        texts->elements.push_back(
            {parser_.ctx_.make<StringLitExpr>(tmpl->loc(), std::move(text)), false});
      }
      std::vector<Expr *> args{texts};
      args.insert(args.end(), tmpl->exprs.begin(), tmpl->exprs.end());
      auto *call = parser_.ctx_.make<CallExpr>(loc, expr, std::move(args));
      call->tagged = true;
      expr = call;
    } else if (parser_.tok_.is(Tok::Bang) && !parser_.tok_.newlineBefore) {
      // `x!`: x is not undefined (checked when the program runs).
      expr = parser_.ctx_.make<UnaryExpr>(parser_.tok_.loc(), UnaryOp::NonNull, expr);
      parser_.cursor().advance();
    } else {
      return finish();
    }
  }
}

Expr *ExprParser::parsePrimary() {
  llvm::SMLoc loc = parser_.tok_.loc();
  // `in` is an operator again inside brackets, even in a loop header.
  llvm::SaveAndRestore<bool> allowIn(parser_.noIn_, false);
  if (parser_.tok_.is(Tok::At)) {
    // `{ @Log m() {} }`, `@Log function () {}`: what follows is still parsed.
    parser_.classes().rejectDecorators(parser_.classes().parseDecorators());
    if (parser_.recovering_) {
      return parser_.ctx_.make<IntLitExpr>(loc, 0);
    }
    return parsePrimary();
  }
  switch (parser_.tok_.kind) {
  case Tok::IntLiteral:
    return parseIntLiteral();
  case Tok::FloatLiteral:
    return parseFloatLiteral();
  case Tok::StringLiteral: {
    std::u16string value;
    decodeStringLiteral(parser_.tok_, value, parser_.diags_);
    parser_.cursor().advance();
    return parser_.ctx_.make<StringLitExpr>(loc, std::move(value));
  }
  case Tok::TemplateString:
  case Tok::TemplateHead:
    return parser_.templates().parseTemplate();
  case Tok::Slash:
  case Tok::SlashAssign: {
    // An expression starts here, so `/` starts a regex literal, which the
    // lexer reads again from there (it took the `/` for division).
    if (parser_.hasPeeked_) {
      parser_.lexer_.restore(parser_.peekState_);
      parser_.hasPeeked_ = false;
    }
    parser_.tok_ = parser_.lexer_.regex(parser_.tok_.text.data(), parser_.tok_.newlineBefore);
    std::u16string pattern, flags;
    decodeRegex(parser_.tok_, pattern, flags);
    auto *lit = parser_.ctx_.make<RegexLitExpr>(loc, std::move(pattern), std::move(flags));
    lit->written = parser_.tok_.text.size();
    parser_.cursor().advance();
    return lit;
  }
  case Tok::KwTrue:
  case Tok::KwFalse: {
    bool value = parser_.tok_.is(Tok::KwTrue);
    parser_.cursor().advance();
    return parser_.ctx_.make<BoolLitExpr>(loc, value);
  }
  case Tok::Identifier: {
    if (parser_.tok_.text == "import") {
      // `import("./x")` and `import.meta`: what follows parses as a call or
      // a member of a placeholder.
      parser_.cursor().report(
          loc, parser_.cursor().peek().is(Tok::Dot)
                   ? "'import.meta' is not supported"
                   : "dynamic 'import()' is not supported: every file the program uses is "
                     "compiled into it; import by name at the top of the file, e.g. 'import "
                     "{ a } from \"./x\"'");
      parser_.cursor().advance();
      return parser_.ctx_.make<IntLitExpr>(loc, 0);
    }
    if (parser_.cursor().peek().is(Tok::Arrow) && !parser_.cursor().peek().newlineBefore) {
      return parser_.functions().parseArrow(); // `x => ...`
    }
    if (parser_.tok_.text == "async") {
      if (Expr *fn = parser_.functions().tryAsyncFunction()) {
        return fn;
      }
    }
    if (parser_.tok_.text == "class" &&
        (parser_.cursor().peek().is(Tok::Identifier) || parser_.cursor().peek().is(Tok::LBrace))) {
      // `const C = class { ... }`, `return class extends Base { ... }`.
      ClassDecl *cls = parser_.classes().parseClass(/*expression=*/true);
      return parser_.ctx_.make<ClassExpr>(loc, cls);
    }
    if (parser_.tok_.text == "yield" && parser_.inGenerator_) {
      // `1 + yield x` is an error in JavaScript: the operand must be in
      // parentheses.
      parser_.cursor().report(loc, "'yield' must be in parentheses here, e.g. '1 + (yield x)'");
      return parseYield();
    }
    llvm::StringRef name = parser_.tok_.text;
    parser_.cursor().advance();
    if (name == "undefined") {
      return parser_.ctx_.make<UndefinedExpr>(loc);
    }
    return parser_.ctx_.make<NameExpr>(loc, name);
  }
  case Tok::KwFunction: {
    FunctionDecl *fn = parser_.functions().parseFunction(/*expression=*/true);
    return parser_.ctx_.make<FunctionExpr>(loc, fn);
  }
  case Tok::LParen: {
    if (parser_.functions().arrowAhead()) {
      return parser_.functions().parseArrow();
    }
    parser_.cursor().advance();
    Expr *inner = parseSequence();
    parser_.cursor().expect(Tok::RParen, "to close the parenthesized expression");
    return inner;
  }
  case Tok::LBracket:
    return parseArrayLiteral();
  case Tok::LBrace:
    return parseObjectLiteral();
  case Tok::KwNew:
    return parseNew();
  case Tok::Less: {
    // `<T>(x: T) => x`, a generic arrow.
    Parser::Snapshot start = parser_.cursor().snapshot();
    const char *text = parser_.tok_.text.data();
    bool muted = parser_.diags_.setMuted(true);
    parser_.recovering_ = false;
    std::vector<TypeParamDecl> typeParams = parser_.functions().parseTypeParams();
    bool arrow =
        !parser_.recovering_ && parser_.tok_.is(Tok::LParen) && parser_.functions().arrowAhead();
    parser_.diags_.setMuted(muted);
    if (arrow) {
      parser_.recovering_ = start.recovering;
      return parser_.functions().parseArrow(std::move(typeParams), text);
    }
    parser_.cursor().rewind(std::move(start));
    parser_.cursor().error(loc, "expected an expression, found '<'");
    return parser_.ctx_.make<IntLitExpr>(loc, 0);
  }
  default:
    parser_.cursor().error(loc, llvm::Twine("expected an expression, found ") +
                                    tokSpelling(parser_.tok_.kind));
    // Placeholder so callers never see null; the source will not compile anyway.
    return parser_.ctx_.make<IntLitExpr>(loc, 0);
  }
}

Expr *ExprParser::parseArrayLiteral() {
  auto *lit = parser_.ctx_.make<ArrayLitExpr>(parser_.tok_.loc());
  parser_.cursor().advance(); // '['
  while (!parser_.tok_.is(Tok::RBracket) && !parser_.tok_.is(Tok::Eof)) {
    if (parser_.tok_.is(Tok::Comma)) {
      // A hole: an error, unless the literal turns out to be a pattern, as in
      // `[, b] = pair`.
      lit->elements.push_back(
          {parser_.ctx_.make<UndefinedExpr>(parser_.tok_.loc()), false, /*hole=*/true});
      parser_.cursor().advance();
      continue;
    }
    bool spread = parser_.cursor().accept(Tok::Ellipsis);
    lit->elements.push_back({parseExpr(), spread});
    if (!parser_.cursor().accept(Tok::Comma)) {
      break;
    }
  }
  parser_.cursor().expect(Tok::RBracket, "to close the array");

  return lit;
}

// `{ x: 1, y }`
Expr *ExprParser::parseObjectLiteral() {
  auto *lit = parser_.ctx_.make<ObjectLitExpr>(parser_.tok_.loc());
  parser_.cursor().advance(); // '{'
  while (!parser_.tok_.is(Tok::RBrace) && !parser_.tok_.is(Tok::Eof)) {
    // `{ @Log m() {} }`: what follows is still parsed.
    if (parser_.tok_.is(Tok::At)) {
      parser_.classes().rejectDecorators(parser_.classes().parseDecorators());
      if (parser_.recovering_) {
        return lit;
      }
    }
    llvm::SMLoc loc = parser_.tok_.loc();
    if (parser_.cursor().accept(Tok::Ellipsis)) {
      // `{ ...o }` copies the fields of o.
      ObjectLitExpr::Property spread;
      spread.loc = loc;
      spread.value = parseExpr();
      spread.spread = true;
      lit->props.push_back(std::move(spread));
      if (!parser_.cursor().accept(Tok::Comma)) {
        break;
      }
      continue;
    }
    if (!parser_.classes().symbolKeyAhead() && parser_.cursor().accept(Tok::LBracket)) {
      // `[key]: value`, where the key must be a constant (see Sema).
      ObjectLitExpr::Property prop;
      prop.loc = loc;
      prop.key = parseExpr();
      parser_.cursor().expect(Tok::RBracket, "to close the key");
      // The field it names, if the key is known by now (see fieldNames).
      if (auto *name = dyn_cast<NameExpr>(prop.key)) {
        for (const std::string &text : parser_.stringConstants_.lookup(name->name)) {
          parser_.fieldNames_.insert(text);
        }
      } else if (auto *text = dyn_cast<StringLitExpr>(prop.key)) {
        parser_.fieldNames_.insert(toUtf8(text->value));
      }
      if (parser_.tok_.is(Tok::LParen) || parser_.tok_.is(Tok::Less)) {
        parser_.cursor().error(
            loc, "a method cannot have a key in brackets; write its name: 'f() { ... }'");
        parser_.cursor().skipToClosingBrace();
        return lit;
      }
      if (!parser_.cursor().expect(Tok::Colon, "after the key in brackets")) {
        parser_.cursor().skipToClosingBrace();
        return lit;
      }
      prop.value = parseExpr();
      lit->props.push_back(std::move(prop));
      if (!parser_.cursor().accept(Tok::Comma)) {
        break;
      }
      continue;
    }
    // `get x() {}`, `set x(v) {}`, `async f() {}`
    bool accessor =
        parser_.tok_.is(Tok::Identifier) &&
        (parser_.tok_.text == "get" || parser_.tok_.text == "set") &&
        (parser_.cursor().peek().is(Tok::Identifier) || isKeyword(parser_.cursor().peek().kind) ||
         parser_.cursor().peek().is(Tok::StringLiteral));
    if (accessor) {
      parser_.cursor().error(
          loc, "getters and setters are not supported: reading or writing a field never runs code; "
               "write a function instead");
      parser_.cursor().skipToClosingBrace();
      return lit;
    }
    // `async f() {}`
    bool isAsync =
        parser_.tok_.is(Tok::Identifier) && parser_.tok_.text == "async" &&
        !parser_.cursor().peek().is(Tok::Colon) && !parser_.cursor().peek().is(Tok::Comma) &&
        !parser_.cursor().peek().is(Tok::RBrace) && !parser_.cursor().peek().is(Tok::LParen) &&
        !parser_.cursor().peek().newlineBefore;
    if (isAsync) {
      parser_.cursor().advance();
    }
    const char *start = parser_.tok_.text.data();
    bool generator = parser_.cursor().accept(Tok::Star); // `*items() { yield ... }`
    bool identifier = parser_.tok_.is(Tok::Identifier);
    llvm::StringRef name;
    if (!parser_.types().parseFieldName(name)) {
      parser_.cursor().skipToClosingBrace();
      return lit;
    }
    Expr *value = nullptr;
    bool defaulted = false, method = false;
    if (parser_.tok_.is(Tok::LParen) || parser_.tok_.is(Tok::Less)) {
      // `f() { ... }`: a method, which sees the object as `this`.
      auto *fn = parser_.ctx_.make<FunctionDecl>(loc, name);
      fn->isExpression = true;
      fn->isGenerator = generator;
      fn->isAsync = isAsync;
      parser_.classes().parseMethodRest(fn, start);
      if (parser_.recovering_ || !fn->body) {
        if (!fn->body) {
          parser_.cursor().error(parser_.tok_.loc(),
                                 "expected '{' to start the body of the method");
        }
        parser_.cursor().skipToClosingBrace();
        return lit;
      }
      value = parser_.ctx_.make<FunctionExpr>(loc, fn);
      method = true;
    } else if (generator || isAsync) {
      parser_.cursor().error(parser_.tok_.loc(),
                             generator ? "expected '(' after the name of the generator method"
                                       : "expected '(' after the name of the async method");
      parser_.cursor().skipToClosingBrace();
      return lit;
    } else if (parser_.cursor().accept(Tok::Colon)) {
      value = parseExpr();
    } else if (identifier && (parser_.tok_.is(Tok::Comma) || parser_.tok_.is(Tok::RBrace))) {
      // `{ x }` is `{ x: x }`.
      value = name == "undefined" ? static_cast<Expr *>(parser_.ctx_.make<UndefinedExpr>(loc))
                                  : parser_.ctx_.make<NameExpr>(loc, name);
    } else if (identifier && parser_.tok_.is(Tok::Assign)) {
      // `{ x = 1 }`: a default value, which only a destructuring pattern
      // such as `({ x = 1 } = o)` may have (Sema reports it otherwise).
      llvm::SMLoc assignLoc = parser_.tok_.loc();
      parser_.cursor().advance();
      value = parser_.ctx_.make<AssignExpr>(assignLoc, std::nullopt,
                                            parser_.ctx_.make<NameExpr>(loc, name), parseExpr());
      defaulted = true;
    } else {
      parser_.cursor().error(parser_.tok_.loc(),
                             llvm::Twine("expected ':' after the field name, found ") +
                                 tokSpelling(parser_.tok_.kind));
      parser_.cursor().skipToClosingBrace();
      return lit;
    }
    ObjectLitExpr::Property prop;
    prop.name = name;
    prop.loc = loc;
    prop.value = value;
    prop.defaulted = defaulted;
    prop.method = method;
    lit->props.push_back(std::move(prop));
    if (!parser_.cursor().accept(Tok::Comma)) {
      break;
    }
  }
  parser_.cursor().expect(Tok::RBrace, "to close the object");

  return lit;
}

// `new Name<T>(args)`
Expr *ExprParser::parseNew() {
  llvm::SMLoc loc = parser_.tok_.loc();
  parser_.cursor().advance(); // 'new'
  // `new (Mixin(User))()`: a class as a value (see NewExpr::callee).
  Expr *callee = nullptr;
  if (parser_.tok_.is(Tok::LParen)) {
    llvm::SMLoc at = parser_.tok_.loc();
    parser_.cursor().advance();
    llvm::SaveAndRestore<bool> allowIn(parser_.noIn_, false);
    callee = parseSequence();
    parser_.cursor().expect(Tok::RParen, "to close the class");
    std::vector<Expr *> args;
    if (parser_.cursor().accept(Tok::LParen)) {
      while (!parser_.tok_.is(Tok::RParen) && !parser_.tok_.is(Tok::Eof)) {
        args.push_back(parseExpr());
        if (!parser_.cursor().accept(Tok::Comma)) {
          break;
        }
      }
      parser_.cursor().expect(Tok::RParen, "to close the argument list");
    }
    auto *create = parser_.ctx_.make<NewExpr>(at, "", std::vector<TypeRef>(), std::move(args));
    create->callee = callee;
    return create;
  }
  if (!parser_.tok_.is(Tok::Identifier)) {
    parser_.cursor().error(parser_.tok_.loc(), "expected a class name after 'new'");
    return parser_.ctx_.make<IntLitExpr>(loc, 0);
  }
  llvm::StringRef name = parser_.tok_.text;
  llvm::SMLoc nameLoc = parser_.tok_.loc();
  parser_.cursor().advance();
  // `new Intl.DateTimeFormat()`: a constructor of a namespace.
  llvm::StringRef member;
  llvm::SMLoc memberLoc;
  if (parser_.cursor().accept(Tok::Dot)) {
    if (parser_.tok_.is(Tok::Identifier)) {
      member = parser_.tok_.text;
      memberLoc = parser_.tok_.loc();
      parser_.cursor().advance();
    } else {
      parser_.cursor().error(parser_.tok_.loc(), "expected a name after '.'");
    }
  }
  // `new this.types[i]()`, `new a.b.c()`: a class as a value.
  if (parser_.tok_.is(Tok::Dot) || parser_.tok_.is(Tok::LBracket)) {
    callee = parser_.ctx_.make<NameExpr>(nameLoc, name);
    if (!member.empty()) {
      callee = parser_.ctx_.make<MemberExpr>(memberLoc, callee, member);
    }
    while (parser_.tok_.is(Tok::Dot) || parser_.tok_.is(Tok::LBracket)) {
      if (parser_.cursor().accept(Tok::Dot)) {
        if (!parser_.tok_.is(Tok::Identifier) && !isKeyword(parser_.tok_.kind)) {
          parser_.cursor().error(parser_.tok_.loc(), "expected a property name after '.'");
          break;
        }
        callee = parser_.ctx_.make<MemberExpr>(parser_.tok_.loc(), callee, parser_.tok_.text);
        parser_.cursor().advance();
        continue;
      }
      llvm::SMLoc at = parser_.tok_.loc();
      parser_.cursor().advance(); // '['
      llvm::SaveAndRestore<bool> allowIn(parser_.noIn_, false);
      Expr *index = parseSequence();
      parser_.cursor().expect(Tok::RBracket, "to close the index");
      callee = parser_.ctx_.make<IndexExpr>(at, callee, index);
    }
    name = member = {};
  }
  std::vector<TypeRef> typeArgs;
  if (parser_.tok_.is(Tok::Less)) {
    typeArgs = parser_.types().parseTypeArgs();
  }
  std::vector<Expr *> args;
  if (parser_.cursor().accept(Tok::LParen)) {
    while (!parser_.tok_.is(Tok::RParen) && !parser_.tok_.is(Tok::Eof)) {
      args.push_back(parseExpr());
      if (!parser_.cursor().accept(Tok::Comma)) {
        break;
      }
    }
    parser_.cursor().expect(Tok::RParen, "to close the argument list");
  }
  auto *create = parser_.ctx_.make<NewExpr>(nameLoc, name, std::move(typeArgs), std::move(args));
  create->member = member;
  create->callee = callee;

  return create;
}

Expr *ExprParser::parseIntLiteral() {
  llvm::SMLoc loc = parser_.tok_.loc();
  llvm::StringRef source = parser_.tok_.text;
  llvm::SmallString<32> digits;
  for (char c : parser_.tok_.text) {
    if (c != '_') {
      digits.push_back(c);
    }
  }
  parser_.cursor().advance();

  llvm::StringRef text = digits;
  unsigned radix = 10;
  if (text.size() > 2 && text[0] == '0') {
    switch (text[1]) {
    case 'x':
    case 'X':
      radix = 16;
      break;
    case 'b':
    case 'B':
      radix = 2;
      break;
    case 'o':
    case 'O':
      radix = 8;
      break;
    default:
      break;
    }
    if (radix != 10) {
      text = text.drop_front(2);
    }
  }

  uint64_t value = 0;
  if (!text.getAsInteger(radix, value)) {
    auto *lit = parser_.ctx_.make<IntLitExpr>(loc, value);
    lit->text = source; // a decimal reads its value from the text
    return lit;
  }
  for (char c : text) {
    unsigned digit = llvm::isDigit(c) ? c - '0' : llvm::isAlpha(c) ? (c | 0x20) - 'a' + 10 : radix;
    if (digit >= radix) {
      parser_.cursor().report(loc, "invalid integer literal");
      return parser_.ctx_.make<IntLitExpr>(loc, 0);
    }
  }
  // Beyond 64 bits: the nearest double, as in JavaScript.
  double rounded = 0;
  if (radix == 10) {
    text.getAsDouble(rounded);
  } else {
    unsigned bits = static_cast<unsigned>(text.size()) * (radix == 16 ? 4 : radix == 8 ? 3 : 1);
    llvm::APFloat number(llvm::APFloat::IEEEdouble());
    number.convertFromAPInt(llvm::APInt(bits, text, static_cast<uint8_t>(radix)),
                            /*IsSigned=*/false, llvm::APFloat::rmNearestTiesToEven);
    rounded = number.convertToDouble();
  }
  auto *lit = parser_.ctx_.make<FloatLitExpr>(loc, rounded);
  lit->text = source;

  return lit;
}

Expr *ExprParser::parseFloatLiteral() {
  llvm::SMLoc loc = parser_.tok_.loc();
  llvm::StringRef text = parser_.tok_.text;
  llvm::SmallString<32> digits;
  for (char c : parser_.tok_.text) {
    if (c != '_') {
      digits.push_back(c);
    }
  }
  parser_.cursor().advance();

  double value = 0;
  if (llvm::StringRef(digits).getAsDouble(value)) {
    parser_.cursor().report(loc, "invalid number literal");
  }
  auto *lit = parser_.ctx_.make<FloatLitExpr>(loc, value);
  lit->text = text;

  return lit;
}

} // namespace tz
