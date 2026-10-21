#include "parse/Parser.h"

#include "llvm/ADT/SmallString.h"

namespace tz {

const char *opSpelling(UnaryOp op) {
  switch (op) {
  case UnaryOp::Neg: return "-";
  case UnaryOp::Plus: return "+";
  case UnaryOp::Not: return "!";
  case UnaryOp::BitNot: return "~";
  }
  return "?";
}

const char *opSpelling(BinaryOp op) {
  switch (op) {
  case BinaryOp::Add: return "+";
  case BinaryOp::Sub: return "-";
  case BinaryOp::Mul: return "*";
  case BinaryOp::Div: return "/";
  case BinaryOp::Rem: return "%";
  case BinaryOp::BitAnd: return "&";
  case BinaryOp::BitOr: return "|";
  case BinaryOp::BitXor: return "^";
  case BinaryOp::Shl: return "<<";
  case BinaryOp::Shr: return ">>";
  case BinaryOp::UShr: return ">>>";
  case BinaryOp::Lt: return "<";
  case BinaryOp::Le: return "<=";
  case BinaryOp::Gt: return ">";
  case BinaryOp::Ge: return ">=";
  case BinaryOp::Eq: return "===";
  case BinaryOp::Ne: return "!==";
  case BinaryOp::LogAnd: return "&&";
  case BinaryOp::LogOr: return "||";
  }
  return "?";
}

namespace {

// Binding strength of binary operators, following JavaScript. `as` binds like
// the relational operators.
constexpr int kRelationalPrec = 7;

int binaryPrec(Tok kind) {
  switch (kind) {
  case Tok::PipePipe: return 1;
  case Tok::AmpAmp: return 2;
  case Tok::Pipe: return 3;
  case Tok::Caret: return 4;
  case Tok::Amp: return 5;
  case Tok::EqEq:
  case Tok::EqEqEq:
  case Tok::BangEq:
  case Tok::BangEqEq: return 6;
  case Tok::Less:
  case Tok::LessEq:
  case Tok::Greater:
  case Tok::GreaterEq: return kRelationalPrec;
  case Tok::Shl:
  case Tok::Shr:
  case Tok::UShr: return 8;
  case Tok::Plus:
  case Tok::Minus: return 9;
  case Tok::Star:
  case Tok::Slash:
  case Tok::Percent: return 10;
  default: return -1;
  }
}

BinaryOp binaryOpFor(Tok kind) {
  switch (kind) {
  case Tok::PipePipe: return BinaryOp::LogOr;
  case Tok::AmpAmp: return BinaryOp::LogAnd;
  case Tok::Pipe: return BinaryOp::BitOr;
  case Tok::Caret: return BinaryOp::BitXor;
  case Tok::Amp: return BinaryOp::BitAnd;
  case Tok::EqEq:
  case Tok::EqEqEq: return BinaryOp::Eq;
  case Tok::BangEq:
  case Tok::BangEqEq: return BinaryOp::Ne;
  case Tok::Less: return BinaryOp::Lt;
  case Tok::LessEq: return BinaryOp::Le;
  case Tok::Greater: return BinaryOp::Gt;
  case Tok::GreaterEq: return BinaryOp::Ge;
  case Tok::Shl: return BinaryOp::Shl;
  case Tok::Shr: return BinaryOp::Shr;
  case Tok::UShr: return BinaryOp::UShr;
  case Tok::Plus: return BinaryOp::Add;
  case Tok::Minus: return BinaryOp::Sub;
  case Tok::Star: return BinaryOp::Mul;
  case Tok::Slash: return BinaryOp::Div;
  default: return BinaryOp::Rem;
  }
}

// Returns true for assignment operators; `op` is set for compound ones.
bool assignmentOp(Tok kind, std::optional<BinaryOp> &op) {
  switch (kind) {
  case Tok::Assign: op.reset(); return true;
  case Tok::PlusAssign: op = BinaryOp::Add; return true;
  case Tok::MinusAssign: op = BinaryOp::Sub; return true;
  case Tok::StarAssign: op = BinaryOp::Mul; return true;
  case Tok::SlashAssign: op = BinaryOp::Div; return true;
  case Tok::PercentAssign: op = BinaryOp::Rem; return true;
  case Tok::AmpAssign: op = BinaryOp::BitAnd; return true;
  case Tok::PipeAssign: op = BinaryOp::BitOr; return true;
  case Tok::CaretAssign: op = BinaryOp::BitXor; return true;
  case Tok::ShlAssign: op = BinaryOp::Shl; return true;
  case Tok::ShrAssign: op = BinaryOp::Shr; return true;
  case Tok::UShrAssign: op = BinaryOp::UShr; return true;
  default: return false;
  }
}

bool startsStatement(Tok kind) {
  switch (kind) {
  case Tok::KwLet:
  case Tok::KwConst:
  case Tok::KwVar:
  case Tok::KwFunction:
  case Tok::KwIf:
  case Tok::KwWhile:
  case Tok::KwDo:
  case Tok::KwFor:
  case Tok::KwReturn:
  case Tok::KwBreak:
  case Tok::KwContinue:
    return true;
  default:
    return false;
  }
}

} // namespace

Parser::Parser(Lexer &lexer, AstContext &ctx, Diagnostics &diags)
    : lexer_(lexer), ctx_(ctx), diags_(diags) {
  advance();
}

//===----------------------------------------------------------------------===//
// Tokens and error recovery
//===----------------------------------------------------------------------===//

void Parser::advance() { tok_ = lexer_.next(); }

bool Parser::accept(Tok kind) {
  if (!tok_.is(kind))
    return false;
  advance();
  return true;
}

bool Parser::expect(Tok kind, llvm::StringRef context) {
  if (accept(kind))
    return true;
  std::string message = std::string("expected ") + tokSpelling(kind);
  if (!context.empty())
    message += " " + context.str();
  error(tok_.loc(), message + ", found " + tokSpelling(tok_.kind));
  return false;
}

// A statement ends with `;`, or, by automatic semicolon insertion, before `}`,
// at the end of the file, or at a line break.
void Parser::expectSemicolon() {
  if (accept(Tok::Semi))
    return;
  if (tok_.is(Tok::RBrace) || tok_.is(Tok::Eof) || tok_.newlineBefore)
    return;
  error(tok_.loc(), llvm::Twine("expected ';', found ") + tokSpelling(tok_.kind));
}

void Parser::error(llvm::SMLoc loc, const llvm::Twine &message) {
  if (recovering_)
    return;
  recovering_ = true;
  diags_.error(loc, message);
}

void Parser::report(llvm::SMLoc loc, const llvm::Twine &message) {
  if (!recovering_)
    diags_.error(loc, message);
}

// Skips to a likely statement boundary after a syntax error.
void Parser::synchronize() {
  recovering_ = false;
  for (;;) {
    if (tok_.is(Tok::Eof) || tok_.is(Tok::RBrace))
      return;
    if (tok_.is(Tok::Semi)) {
      advance();
      return;
    }
    if (tok_.newlineBefore && startsStatement(tok_.kind))
      return;
    advance();
  }
}

//===----------------------------------------------------------------------===//
// Statements
//===----------------------------------------------------------------------===//

SourceFile Parser::parseSourceFile() {
  SourceFile file;
  while (!tok_.is(Tok::Eof)) {
    if (tok_.is(Tok::RBrace)) {
      error(tok_.loc(), "unexpected '}'");
      advance();
      recovering_ = false;
      continue;
    }
    file.items.push_back(parseStatement());
    if (recovering_)
      synchronize();
  }
  return file;
}

Stmt *Parser::parseStatement() {
  llvm::SMLoc loc = tok_.loc();
  switch (tok_.kind) {
  case Tok::LBrace:
    return parseBlock();
  case Tok::KwFunction:
    return parseFunction();
  case Tok::KwVar:
    report(loc, "'var' is not supported; use 'let' or 'const'");
    [[fallthrough]];
  case Tok::KwLet:
  case Tok::KwConst: {
    VarDeclStmt *decl = parseVarDecl();
    expectSemicolon();
    return decl;
  }
  case Tok::KwIf:
    return parseIf();
  case Tok::KwWhile:
    return parseWhile();
  case Tok::KwDo:
    return parseDoWhile();
  case Tok::KwFor:
    return parseFor();
  case Tok::KwReturn:
    return parseReturn();
  case Tok::KwBreak:
  case Tok::KwContinue: {
    bool isBreak = tok_.is(Tok::KwBreak);
    advance();
    if (tok_.is(Tok::Identifier) && !tok_.newlineBefore) {
      report(tok_.loc(), "labels are not supported yet");
      advance();
    }
    expectSemicolon();
    if (isBreak)
      return ctx_.make<BreakStmt>(loc);
    return ctx_.make<ContinueStmt>(loc);
  }
  case Tok::Semi:
    advance();
    return ctx_.make<EmptyStmt>(loc);
  default: {
    Expr *expr = parseExpr();
    expectSemicolon();
    return ctx_.make<ExprStmt>(loc, expr);
  }
  }
}

BlockStmt *Parser::parseBlock() {
  auto *block = ctx_.make<BlockStmt>(tok_.loc());
  expect(Tok::LBrace);
  while (!tok_.is(Tok::RBrace) && !tok_.is(Tok::Eof)) {
    block->body.push_back(parseStatement());
    if (recovering_)
      synchronize();
  }
  expect(Tok::RBrace, "to close the block");
  return block;
}

FunctionDecl *Parser::parseFunction() {
  advance(); // 'function'

  llvm::StringRef name;
  llvm::SMLoc nameLoc = tok_.loc();
  if (tok_.is(Tok::Identifier)) {
    name = tok_.text;
    advance();
  } else {
    error(tok_.loc(), "expected a function name");
  }
  auto *fn = ctx_.make<FunctionDecl>(nameLoc, name);

  expect(Tok::LParen, "after the function name");
  while (!tok_.is(Tok::RParen) && !tok_.is(Tok::Eof)) {
    Param param;
    param.loc = tok_.loc();
    if (tok_.is(Tok::Identifier)) {
      param.name = tok_.text;
      advance();
    } else {
      error(tok_.loc(), "expected a parameter name");
      break;
    }
    if (tok_.is(Tok::Question)) {
      report(tok_.loc(), "optional parameters are not supported yet");
      advance();
    }
    if (accept(Tok::Colon))
      param.type = parseType();
    if (tok_.is(Tok::Assign)) {
      report(tok_.loc(), "default parameter values are not supported yet");
      advance();
      parseExpr();
    }
    fn->params.push_back(param);
    if (!accept(Tok::Comma))
      break;
  }
  expect(Tok::RParen, "to close the parameter list");

  if (accept(Tok::Colon))
    fn->returnType = parseType();

  if (tok_.is(Tok::LBrace))
    fn->body = parseBlock();
  else {
    error(tok_.loc(), "expected '{' to start the function body");
    fn->body = ctx_.make<BlockStmt>(tok_.loc());
  }
  return fn;
}

// Parses `let`/`const` declarators without the trailing semicolon, so that it
// also serves `for (let i = 0; ...)`.
VarDeclStmt *Parser::parseVarDecl() {
  auto *decl = ctx_.make<VarDeclStmt>(tok_.loc(), tok_.is(Tok::KwConst));
  advance(); // 'let' / 'const' / 'var'

  do {
    VarDeclarator d;
    d.loc = tok_.loc();
    if (!tok_.is(Tok::Identifier)) {
      error(tok_.loc(), "expected a variable name");
      return decl;
    }
    d.name = tok_.text;
    advance();
    if (accept(Tok::Colon))
      d.typeAnn = parseType();
    if (accept(Tok::Assign))
      d.init = parseExpr();
    decl->decls.push_back(d);
  } while (accept(Tok::Comma));
  return decl;
}

Stmt *Parser::parseIf() {
  llvm::SMLoc loc = tok_.loc();
  advance(); // 'if'
  expect(Tok::LParen, "after 'if'");
  Expr *cond = parseExpr();
  expect(Tok::RParen, "after the condition");
  Stmt *thenStmt = parseStatement();
  Stmt *elseStmt = nullptr;
  if (accept(Tok::KwElse))
    elseStmt = parseStatement();
  return ctx_.make<IfStmt>(loc, cond, thenStmt, elseStmt);
}

Stmt *Parser::parseWhile() {
  llvm::SMLoc loc = tok_.loc();
  advance(); // 'while'
  expect(Tok::LParen, "after 'while'");
  Expr *cond = parseExpr();
  expect(Tok::RParen, "after the condition");
  Stmt *body = parseStatement();
  return ctx_.make<WhileStmt>(loc, cond, body);
}

Stmt *Parser::parseDoWhile() {
  llvm::SMLoc loc = tok_.loc();
  advance(); // 'do'
  Stmt *body = parseStatement();
  expect(Tok::KwWhile, "after the body of 'do'");
  expect(Tok::LParen, "after 'while'");
  Expr *cond = parseExpr();
  expect(Tok::RParen, "after the condition");
  // A semicolon after `do ... while (...)` is always optional.
  accept(Tok::Semi);
  return ctx_.make<DoWhileStmt>(loc, body, cond);
}

Stmt *Parser::parseFor() {
  auto *loop = ctx_.make<ForStmt>(tok_.loc());
  advance(); // 'for'
  expect(Tok::LParen, "after 'for'");

  if (tok_.is(Tok::KwLet) || tok_.is(Tok::KwConst) || tok_.is(Tok::KwVar)) {
    if (tok_.is(Tok::KwVar))
      error(tok_.loc(), "'var' is not supported; use 'let' or 'const'");
    loop->init = parseVarDecl();
  } else if (!tok_.is(Tok::Semi)) {
    llvm::SMLoc loc = tok_.loc();
    loop->init = ctx_.make<ExprStmt>(loc, parseExpr());
  }
  if (tok_.is(Tok::Identifier) && (tok_.text == "of" || tok_.text == "in")) {
    error(tok_.loc(), "'for...of' and 'for...in' loops are not supported yet");
    return loop;
  }
  expect(Tok::Semi, "after the loop initializer");

  if (!tok_.is(Tok::Semi))
    loop->cond = parseExpr();
  expect(Tok::Semi, "after the loop condition");

  if (!tok_.is(Tok::RParen))
    loop->step = parseExpr();
  expect(Tok::RParen, "to close the loop header");

  loop->body = parseStatement();
  return loop;
}

Stmt *Parser::parseReturn() {
  llvm::SMLoc loc = tok_.loc();
  advance(); // 'return'
  Expr *value = nullptr;
  // `return` followed by a line break returns nothing, as in JavaScript.
  if (!tok_.is(Tok::Semi) && !tok_.is(Tok::RBrace) && !tok_.is(Tok::Eof) && !tok_.newlineBefore)
    value = parseExpr();
  expectSemicolon();
  return ctx_.make<ReturnStmt>(loc, value);
}

//===----------------------------------------------------------------------===//
// Types
//===----------------------------------------------------------------------===//

TypeRef Parser::parseType() {
  TypeRef ref;
  ref.loc = tok_.loc();
  if (tok_.is(Tok::Identifier)) {
    ref.name = tok_.text;
    advance();
  } else {
    error(tok_.loc(), llvm::Twine("expected a type, found ") + tokSpelling(tok_.kind));
    return ref;
  }

  if (tok_.is(Tok::LBracket)) {
    report(tok_.loc(), "array types are not supported yet");
    advance();
    expect(Tok::RBracket);
  } else if (tok_.is(Tok::Pipe)) {
    report(tok_.loc(), "union types are not supported yet");
    advance();
    parseType();
  } else if (tok_.is(Tok::Less)) {
    error(tok_.loc(), "generic types are not supported yet");
  }
  return ref;
}

//===----------------------------------------------------------------------===//
// Expressions
//===----------------------------------------------------------------------===//

// Assignment is right-associative and has the lowest precedence.
Expr *Parser::parseExpr() {
  Expr *lhs = parseConditional();
  std::optional<BinaryOp> op;
  if (!assignmentOp(tok_.kind, op))
    return lhs;
  llvm::SMLoc loc = tok_.loc();
  advance();
  Expr *rhs = parseExpr();
  return ctx_.make<AssignExpr>(loc, op, lhs, rhs);
}

Expr *Parser::parseConditional() {
  Expr *cond = parseBinary(0);
  if (!tok_.is(Tok::Question))
    return cond;
  llvm::SMLoc loc = tok_.loc();
  advance();
  Expr *thenExpr = parseExpr();
  expect(Tok::Colon, "in the conditional expression");
  Expr *elseExpr = parseExpr();
  return ctx_.make<ConditionalExpr>(loc, cond, thenExpr, elseExpr);
}

Expr *Parser::parseBinary(int minPrec) {
  Expr *lhs = parseUnary();
  for (;;) {
    if (tok_.is(Tok::KwAs) && kRelationalPrec >= minPrec) {
      llvm::SMLoc loc = tok_.loc();
      advance();
      lhs = ctx_.make<CastExpr>(loc, lhs, parseType());
      continue;
    }
    int prec = binaryPrec(tok_.kind);
    if (prec < 0 || prec < minPrec)
      return lhs;
    BinaryOp op = binaryOpFor(tok_.kind);
    llvm::SMLoc loc = tok_.loc();
    advance();
    Expr *rhs = parseBinary(prec + 1);
    lhs = ctx_.make<BinaryExpr>(loc, op, lhs, rhs);
  }
}

Expr *Parser::parseUnary() {
  llvm::SMLoc loc = tok_.loc();
  std::optional<UnaryOp> op;
  switch (tok_.kind) {
  case Tok::Minus: op = UnaryOp::Neg; break;
  case Tok::Plus: op = UnaryOp::Plus; break;
  case Tok::Bang: op = UnaryOp::Not; break;
  case Tok::Tilde: op = UnaryOp::BitNot; break;
  case Tok::PlusPlus:
  case Tok::MinusMinus: {
    bool increment = tok_.is(Tok::PlusPlus);
    advance();
    return ctx_.make<UpdateExpr>(loc, increment, /*prefix=*/true, parseUnary());
  }
  default:
    return parsePostfix();
  }
  advance();
  return ctx_.make<UnaryExpr>(loc, *op, parseUnary());
}

Expr *Parser::parsePostfix() {
  Expr *expr = parseCallOrMember();
  // A line break before `++`/`--` makes it a prefix operator of the next line.
  if ((tok_.is(Tok::PlusPlus) || tok_.is(Tok::MinusMinus)) && !tok_.newlineBefore) {
    bool increment = tok_.is(Tok::PlusPlus);
    llvm::SMLoc loc = tok_.loc();
    advance();
    return ctx_.make<UpdateExpr>(loc, increment, /*prefix=*/false, expr);
  }
  return expr;
}

Expr *Parser::parseCallOrMember() {
  Expr *expr = parsePrimary();
  for (;;) {
    if (tok_.is(Tok::LParen)) {
      // Diagnostics about a call point at its start, e.g. `f` in `f(x)`.
      llvm::SMLoc loc = expr->loc();
      advance();
      std::vector<Expr *> args;
      while (!tok_.is(Tok::RParen) && !tok_.is(Tok::Eof)) {
        args.push_back(parseExpr());
        if (!accept(Tok::Comma))
          break;
      }
      expect(Tok::RParen, "to close the argument list");
      expr = ctx_.make<CallExpr>(loc, expr, std::move(args));
    } else if (tok_.is(Tok::Dot)) {
      advance();
      llvm::SMLoc loc = tok_.loc();
      if (!tok_.is(Tok::Identifier)) {
        error(tok_.loc(), "expected a property name after '.'");
        return expr;
      }
      expr = ctx_.make<MemberExpr>(loc, expr, tok_.text);
      advance();
    } else if (tok_.is(Tok::LBracket)) {
      error(tok_.loc(), "indexing is not supported yet");
      return expr;
    } else {
      return expr;
    }
  }
}

Expr *Parser::parsePrimary() {
  llvm::SMLoc loc = tok_.loc();
  switch (tok_.kind) {
  case Tok::IntLiteral:
    return parseIntLiteral();
  case Tok::FloatLiteral:
    return parseFloatLiteral();
  case Tok::StringLiteral: {
    std::string value;
    decodeStringLiteral(tok_, value, diags_);
    advance();
    return ctx_.make<StringLitExpr>(loc, std::move(value));
  }
  case Tok::KwTrue:
  case Tok::KwFalse: {
    bool value = tok_.is(Tok::KwTrue);
    advance();
    return ctx_.make<BoolLitExpr>(loc, value);
  }
  case Tok::Identifier: {
    llvm::StringRef name = tok_.text;
    advance();
    return ctx_.make<NameExpr>(loc, name);
  }
  case Tok::LParen: {
    advance();
    Expr *inner = parseExpr();
    expect(Tok::RParen, "to close the parenthesized expression");
    return inner;
  }
  default:
    error(loc, llvm::Twine("expected an expression, found ") + tokSpelling(tok_.kind));
    // Placeholder so callers never see null; the source will not compile anyway.
    return ctx_.make<IntLitExpr>(loc, 0);
  }
}

Expr *Parser::parseIntLiteral() {
  llvm::SMLoc loc = tok_.loc();
  llvm::SmallString<32> digits;
  for (char c : tok_.text)
    if (c != '_')
      digits.push_back(c);
  advance();

  llvm::StringRef text = digits;
  unsigned radix = 10;
  if (text.size() > 2 && text[0] == '0') {
    switch (text[1]) {
    case 'x': case 'X': radix = 16; break;
    case 'b': case 'B': radix = 2; break;
    case 'o': case 'O': radix = 8; break;
    default: break;
    }
    if (radix != 10)
      text = text.drop_front(2);
  }

  uint64_t value = 0;
  if (text.getAsInteger(radix, value))
    report(loc, "invalid integer literal, or too large for 64 bits");
  return ctx_.make<IntLitExpr>(loc, value);
}

Expr *Parser::parseFloatLiteral() {
  llvm::SMLoc loc = tok_.loc();
  llvm::SmallString<32> digits;
  for (char c : tok_.text)
    if (c != '_')
      digits.push_back(c);
  advance();

  double value = 0;
  if (llvm::StringRef(digits).getAsDouble(value))
    report(loc, "invalid number literal");
  return ctx_.make<FloatLitExpr>(loc, value);
}

} // namespace tz
