#include "compiler/load/function_parser/function_parser.h"
#include "compiler/load/class_parser/class_parser.h"
#include "compiler/load/decl_parser/decl_parser.h"
#include "compiler/load/expr_parser/expr_parser.h"
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

FunctionParser::FunctionParser(Parser &parser) : parser_(parser) {}

// A function declaration. TypeScript's overload signatures (`function f(x:
// int): int` without a body, before the implementation, which then works out
// what it was given) are an error: a plain function has one body, and a
// variant with other parameters is a function of its own, with its own name.
// The signatures are skipped and the implementation is kept, so that the rest
// of the program is still checked.
Stmt *FunctionParser::parseFunctionDecl(bool isAsync) {
  FunctionDecl *fn = parseFunction(/*expression=*/false, isAsync);
  if (fn->body) {
    return fn;
  }
  // An intrinsic of a built-in module written in tz: the runtime has its
  // code (see FunctionDecl::external).
  if (parser_.library_ && fn->name.starts_with("__tz_")) {
    fn->external = true;
    fn->body = parser_.ctx_.make<BlockStmt>(fn->loc());
    return fn;
  }
  llvm::StringRef name = fn->name;
  llvm::SMLoc loc = fn->loc();
  while (!fn->body) {
    if (parser_.tok_.is(Tok::Identifier) && parser_.tok_.text == "export" &&
        parser_.cursor().peek().is(Tok::KwFunction)) {
      parser_.cursor().advance();
    }
    if (!parser_.tok_.is(Tok::KwFunction) || parser_.cursor().peek().text != name) {
      parser_.cursor().report(loc, "function '" + name +
                                       "' has no body; add it after the parameters: '{ ... }'");
      return parser_.ctx_.make<EmptyStmt>(loc);
    }
    fn = parseFunction(/*expression=*/false, isAsync);
  }
  parser_.cursor().report(
      loc, "overload signatures are not supported: plain functions are not overloaded; give each "
           "variant its own name and body");

  return fn;
}

FunctionDecl *FunctionParser::parseFunction(bool expression, bool isAsync) {
  const char *start = parser_.tok_.text.data();
  llvm::SMLoc keywordLoc = parser_.tok_.loc();
  parser_.cursor().advance(); // 'function'
  bool generator = parser_.cursor().accept(Tok::Star);

  llvm::StringRef name;
  llvm::SMLoc nameLoc = parser_.tok_.loc();
  if (parser_.tok_.is(Tok::Identifier)) {
    name = parser_.tok_.text;
    parser_.cursor().advance();
  } else if (expression) {
    nameLoc = keywordLoc;
  } else {
    parser_.cursor().error(parser_.tok_.loc(), "expected a function name");
  }
  auto *fn = parser_.ctx_.make<FunctionDecl>(nameLoc, name);
  fn->isExpression = expression;
  fn->isGenerator = generator;
  fn->isAsync = isAsync;
  if (expression) {
    fn->ownName = name;
  }
  if (parser_.tok_.is(Tok::Less)) {
    fn->typeParams = parseTypeParams();
  }
  // `yield` is an operator only in the body of a generator.
  bool outerGenerator = parser_.inGenerator_;
  parser_.inGenerator_ = generator;
  Parser::ParamPatterns patterns;
  parseParams(fn, &patterns);

  if (parser_.cursor().accept(Tok::Colon)) {
    fn->returnType = parser_.types().parseReturnType();
  }

  if (parser_.tok_.is(Tok::LBrace)) {
    // `yield` belongs to the generator whose body it is in.
    llvm::SaveAndRestore<bool> inGenerator(parser_.inGenerator_, generator);
    fn->body = parser_.stmts().parseBlock();
    addPrologue(fn, patterns);
  } else if (!expression && (parser_.cursor().accept(Tok::Semi) || parser_.tok_.newlineBefore ||
                             parser_.tok_.is(Tok::RBrace) || parser_.tok_.is(Tok::Eof))) {
    // No body, as in an overload signature (see parseFunctionDecl).
  } else {
    parser_.cursor().error(parser_.tok_.loc(), "expected '{' to start the function body");
    fn->body = parser_.ctx_.make<BlockStmt>(parser_.tok_.loc());
  }
  parser_.inGenerator_ = outerGenerator;
  fn->source = llvm::StringRef(start, static_cast<size_t>(parser_.prevEnd_ - start));

  return fn;
}

void FunctionParser::parseParams(FunctionDecl *fn, Parser::ParamPatterns *patterns) {
  parser_.cursor().expect(Tok::LParen,
                          fn->isArrow ? "to start the parameters" : "after the function name");
  while (!parser_.tok_.is(Tok::RParen) && !parser_.tok_.is(Tok::Eof)) {
    Param param;
    // `constructor(@Inject("name") name: string)` (docs/decorators.md).
    param.decorators = parser_.classes().parseDecorators();
    if (parser_.recovering_) {
      break;
    }
    // `constructor(public x: int)`: a parameter property.
    while (parser_.tok_.is(Tok::Identifier) &&
           (parser_.tok_.text == "public" || parser_.tok_.text == "private" ||
            parser_.tok_.text == "protected" || parser_.tok_.text == "readonly" ||
            parser_.tok_.text == "override") &&
           parser_.cursor().peek().is(Tok::Identifier)) {
      if (!fn->isConstructor) {
        parser_.cursor().report(parser_.tok_.loc(),
                                "'" + parser_.tok_.text +
                                    "' before a parameter makes it a field, which only "
                                    "a constructor's parameter can be");
      }
      param.property = true;
      if (parser_.tok_.text == "readonly") {
        param.readonly = true;
      } else if (parser_.tok_.text == "private") {
        param.access = Access::Private;
      } else if (parser_.tok_.text == "protected") {
        param.access = Access::Protected;
      }
      parser_.cursor().advance();
    }
    param.rest = parser_.cursor().accept(Tok::Ellipsis);
    param.loc = parser_.tok_.loc();
    if (parser_.tok_.is(Tok::Identifier)) {
      param.name = parser_.tok_.text;
      parser_.cursor().advance();
    } else if ((parser_.tok_.is(Tok::LBrace) || parser_.tok_.is(Tok::LBracket)) && !param.rest) {
      // `({ x, y }: Point)`: the body destructures a hidden parameter.
      const char *start = parser_.tok_.text.data();
      std::unique_ptr<Parser::Pattern> pattern = parser_.patterns().parseBindingPattern();
      param.name = parser_.ctx_.hiddenName();
      param.pattern = parser_.cursor().textFrom(start);
      if (patterns) {
        patterns->emplace_back(param.name, std::move(pattern));
      }
    } else {
      parser_.cursor().error(parser_.tok_.loc(), param.rest
                                                     ? "expected a parameter name after '...'"
                                                     : "expected a parameter name");
      break;
    }
    if (parser_.cursor().accept(Tok::Question)) {
      param.optional = true;
    }
    if (parser_.cursor().accept(Tok::Colon)) {
      param.type = parser_.types().parseType();
    }
    if (parser_.tok_.is(Tok::Assign)) {
      if (param.optional) {
        parser_.cursor().report(
            parser_.tok_.loc(),
            "a parameter with a default value is optional already; remove the '?'");
      }
      parser_.cursor().advance();
      param.defaultValue = parser_.exprs().parseExpr();
    }
    if (param.rest && (param.optional || param.defaultValue)) {
      parser_.cursor().report(param.loc,
                              "a rest parameter cannot be optional or have a default value");
    }
    fn->params.push_back(param);
    if (!parser_.cursor().accept(Tok::Comma)) {
      break;
    }
    if (param.rest && !parser_.tok_.is(Tok::RParen)) {
      parser_.cursor().report(param.loc, "a rest parameter must be the last parameter");
    }
  }
  parser_.cursor().expect(Tok::RParen, "to close the parameter list");
}

// `function f({ x, y }: Point) { body }` is
// `function f(%0: Point) { let x = %0.x, y = %0.y; body }`: as in
// JavaScript, the variables can be assigned to.
void FunctionParser::addPrologue(FunctionDecl *fn, Parser::ParamPatterns &patterns) {
  if (patterns.empty() || !fn->body) {
    return;
  }
  auto *decl = parser_.ctx_.make<VarDeclStmt>(fn->body->loc(), /*isConst=*/false);
  for (auto &[name, pattern] : patterns) {
    parser_.patterns().declareElements(decl, *pattern, name);
  }
  fn->body->body.insert(fn->body->body.begin(), decl);
}

bool FunctionParser::asyncFunctionAhead() {
  return parser_.tok_.is(Tok::Identifier) && parser_.tok_.text == "async" &&
         parser_.cursor().peek().is(Tok::KwFunction) && !parser_.cursor().peek().newlineBefore;
}

// At `async`: an async arrow `async x => ...` or `async (x) => ...`, or an
// async function expression, follows; otherwise `async` is a name, and
// nothing is consumed.
Expr *FunctionParser::tryAsyncFunction() {
  llvm::SMLoc loc = parser_.tok_.loc();
  if (asyncFunctionAhead()) {
    parser_.cursor().advance();
    return parser_.ctx_.make<FunctionExpr>(loc,
                                           parseFunction(/*expression=*/true, /*isAsync=*/true));
  }
  const Token &next = parser_.cursor().peek();
  if (next.newlineBefore || !(next.is(Tok::Identifier) || next.is(Tok::LParen))) {
    return nullptr;
  }
  Parser::Snapshot start = parser_.cursor().snapshot();
  parser_.cursor().advance(); // 'async'
  bool arrow = parser_.tok_.is(Tok::Identifier) ? parser_.cursor().peek().is(Tok::Arrow) &&
                                                      !parser_.cursor().peek().newlineBefore
                                                : arrowAhead();
  if (!arrow) {
    parser_.cursor().rewind(std::move(start));
    return nullptr;
  }
  FunctionExpr *fe = parseArrow();
  fe->fn->isAsync = true;

  return fe;
}

// At `(`: the parameters of an arrow function follow if they parse as such
// and `=>` (or a return type and `=>`) comes after them.
bool FunctionParser::arrowAhead() {
  Parser::Snapshot start = parser_.cursor().snapshot();
  bool muted = parser_.diags_.setMuted(true);
  parser_.recovering_ = false;
  FunctionDecl probe(parser_.tok_.loc(), "");
  probe.isArrow = true;
  parseParams(&probe);
  bool arrow = !parser_.recovering_;
  if (arrow && parser_.cursor().accept(Tok::Colon)) {
    parser_.types().parseType();
    arrow = !parser_.recovering_;
  }
  arrow = arrow && parser_.tok_.is(Tok::Arrow);
  parser_.diags_.setMuted(muted);
  parser_.cursor().rewind(std::move(start));

  return arrow;
}

FunctionExpr *FunctionParser::parseArrow(std::vector<TypeParamDecl> typeParams, const char *start) {
  if (!start) {
    start = parser_.tok_.text.data();
  }
  llvm::SMLoc loc = parser_.tok_.loc();
  auto *fn = parser_.ctx_.make<FunctionDecl>(loc, "");
  fn->isExpression = fn->isArrow = true;
  fn->typeParams = std::move(typeParams);
  // An arrow cannot yield for the generator around it.
  llvm::SaveAndRestore<bool> inGenerator(parser_.inGenerator_, false);
  Parser::ParamPatterns patterns;
  if (parser_.tok_.is(Tok::Identifier)) {
    Param param;
    param.loc = parser_.tok_.loc();
    param.name = parser_.tok_.text;
    parser_.cursor().advance();
    fn->params.push_back(param);
  } else {
    parseParams(fn, &patterns);
    if (parser_.cursor().accept(Tok::Colon)) {
      fn->returnType = parser_.types().parseReturnType();
    }
  }
  if (parser_.tok_.is(Tok::Arrow) && parser_.tok_.newlineBefore) {
    parser_.cursor().report(parser_.tok_.loc(), "'=>' must be on the same line as the parameters");
  }
  parser_.cursor().expect(Tok::Arrow, "after the parameters of the arrow function");
  if (parser_.tok_.is(Tok::LBrace)) {
    fn->body = parser_.stmts().parseBlock();
    // `() => { a: 1 }` is a block with the label `a`, not an object.
    if (fn->body->body.size() == 1) {
      if (auto *labeled = dyn_cast<LabeledStmt>(fn->body->body[0]);
          labeled && isa<ExprStmt>(labeled->body)) {
        parser_.cursor().report(
            labeled->loc(), "'{' after '=>' starts the body of the function, so '" +
                                labeled->label + ":' is a label, and the function returns nothing");
      }
    }
  } else {
    // `x => value` returns the value.
    llvm::SMLoc valueLoc = parser_.tok_.loc();
    Expr *value = parser_.exprs().parseExpr();
    fn->body = parser_.ctx_.make<BlockStmt>(valueLoc);
    fn->body->body.push_back(parser_.ctx_.make<ReturnStmt>(valueLoc, value));
    fn->concise = true;
  }
  addPrologue(fn, patterns);
  fn->source = llvm::StringRef(start, static_cast<size_t>(parser_.prevEnd_ - start));

  return parser_.ctx_.make<FunctionExpr>(loc, fn);
}

// `<T, U extends number = int>`. TypeScript's modifiers of type parameters
// (`const T`, `in T`, `out T`) say nothing a copy for each type needs.
std::vector<TypeParamDecl> FunctionParser::parseTypeParams() {
  std::vector<TypeParamDecl> params;
  parser_.cursor().advance(); // '<'
  while (!parser_.tok_.is(Tok::Greater) && !parser_.tok_.is(Tok::Eof)) {
    if ((parser_.tok_.is(Tok::KwConst) || parser_.tok_.is(Tok::KwIn) ||
         (parser_.tok_.is(Tok::Identifier) && parser_.tok_.text == "out")) &&
        parser_.cursor().peek().is(Tok::Identifier)) {
      parser_.cursor().report(parser_.tok_.loc(),
                              "'" + parser_.tok_.text +
                                  "' before a type parameter is not supported; remove it");
      parser_.cursor().advance();
    }
    TypeParamDecl param;
    param.loc = parser_.tok_.loc();
    if (!parser_.tok_.is(Tok::Identifier)) {
      parser_.cursor().error(parser_.tok_.loc(),
                             llvm::Twine("expected a type parameter name, found ") +
                                 tokSpelling(parser_.tok_.kind));
      return params;
    }
    param.name = parser_.tok_.text;
    parser_.cursor().advance();
    if (parser_.tok_.is(Tok::Identifier) && parser_.tok_.text == "extends") {
      parser_.cursor().advance();
      param.constraint = parser_.types().parseType();
    }
    if (parser_.cursor().accept(Tok::Assign)) {
      param.defaultType = parser_.types().parseType();
    }
    params.push_back(std::move(param));
    if (!parser_.cursor().accept(Tok::Comma)) {
      break;
    }
  }
  parser_.cursor().expect(Tok::Greater, "to close the type parameters");

  return params;
}

} // namespace tz
