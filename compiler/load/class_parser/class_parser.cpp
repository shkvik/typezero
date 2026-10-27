#include "compiler/load/class_parser/class_parser.h"
#include "compiler/load/call_parser/call_parser.h"
#include "compiler/load/decl_parser/decl_parser.h"
#include "compiler/load/expr_parser/expr_parser.h"
#include "compiler/load/function_parser/function_parser.h"
#include "compiler/load/module_parser/module_parser.h"
#include "compiler/load/object_type_parser/object_type_parser.h"
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

ClassParser::ClassParser(Parser &parser) : parser_(parser) {}

ClassDecl *ClassParser::parseClass(bool expression) {
  const char *start = parser_.tok_.text.data();
  llvm::SMLoc loc = parser_.tok_.loc();
  parser_.cursor().advance();
  bool named =
      parser_.tok_.is(Tok::Identifier) &&
      !(expression && (parser_.tok_.text == "extends" || parser_.tok_.text == "implements"));
  auto *cls = parser_.ctx_.make<ClassDecl>(named ? parser_.tok_.loc() : loc,
                                           named ? parser_.tok_.text : llvm::StringRef());
  cls->expression = expression;
  if (named) {
    parser_.cursor().advance();
  }
  if (parser_.tok_.is(Tok::Less)) {
    cls->typeParams = parser_.functions().parseTypeParams();
  }
  if (parser_.tok_.is(Tok::Identifier) && parser_.tok_.text == "extends") {
    parser_.cursor().advance();
    const Token &next = parser_.cursor().peek();
    bool value = !parser_.tok_.is(Tok::Identifier) || next.is(Tok::LParen) || next.is(Tok::Dot) ||
                 next.is(Tok::LBracket);
    if (value) {
      cls->baseExpr = parser_.calls().parseCallOrMember();
    } else {
      llvm::SMLoc baseLoc = parser_.tok_.loc();
      llvm::StringRef baseName = parser_.tok_.text;
      cls->base = parser_.types().parsePrimaryType();
      if (expression && cls->base->kind == TypeRef::Kind::Name) {
        cls->baseExpr = parser_.ctx_.make<NameExpr>(baseLoc, baseName);
      }
    }
  }
  if (parser_.tok_.is(Tok::Identifier) && parser_.tok_.text == "implements") {
    parser_.cursor().advance();
    do {
      cls->interfaces.push_back(parser_.types().parsePrimaryType());
    } while (parser_.cursor().accept(Tok::Comma));
  }
  if (!parser_.cursor().expect(Tok::LBrace, "to start the members of the class")) {
    return cls;
  }
  while (!parser_.tok_.is(Tok::RBrace) && !parser_.tok_.is(Tok::Eof)) {
    parseClassMember(cls);
    if (parser_.recovering_) {
      skipClassMember();
      parser_.recovering_ = false;
    }
  }
  parser_.cursor().expect(Tok::RBrace, "to close the class");
  if (!cls->typeParams.empty()) {
    cls->source = parser_.cursor().textFrom(start);
  }

  return cls;
}

bool ClassParser::memberModifierAhead() {
  static const char *const modifiers[] = {"public",   "private",  "protected", "readonly", "static",
                                          "override", "abstract", "declare",   "accessor", "async"};
  if (!parser_.tok_.is(Tok::Identifier) || !llvm::is_contained(modifiers, parser_.tok_.text)) {
    return false;
  }
  const Token &next = parser_.cursor().peek();
  if (next.newlineBefore && !next.is(Tok::LBrace)) {
    return false;
  }
  switch (next.kind) {
  case Tok::LParen:
  case Tok::Colon:
  case Tok::Assign:
  case Tok::Semi:
  case Tok::Question:
  case Tok::Bang:
  case Tok::Less:
  case Tok::RBrace:
  case Tok::Eof:
    return false;
  default:
    return true;
  }
}

void ClassParser::skipClassMember() {
  bool moved = false;
  while (!parser_.tok_.is(Tok::Eof) && !parser_.tok_.is(Tok::RBrace)) {
    if (moved && parser_.tok_.newlineBefore) {
      return;
    }
    if (parser_.cursor().accept(Tok::Semi)) {
      return;
    }
    if (parser_.tok_.is(Tok::LBrace)) {
      parser_.cursor().advance();
      parser_.cursor().skipToClosingBrace();
    } else {
      parser_.cursor().advance();
    }
    moved = true;
  }
}

std::vector<Decorator> ClassParser::parseDecorators() {
  std::vector<Decorator> decorators;
  while (parser_.tok_.is(Tok::At)) {
    llvm::SMLoc loc = parser_.tok_.loc();
    parser_.cursor().advance();
    if (Expr *expr = parseDecoratorExpr()) {
      decorators.push_back({loc, expr});
    }
    if (parser_.recovering_) {
      break;
    }
  }

  return decorators;
}

Expr *ClassParser::parseDecoratorExpr() {
  if (parser_.cursor().accept(Tok::LParen)) {
    Expr *expr = parser_.exprs().parseSequence();
    parser_.cursor().expect(Tok::RParen, "to close the decorator");
    return expr;
  }
  if (!parser_.tok_.is(Tok::Identifier)) {
    parser_.cursor().error(parser_.tok_.loc(),
                           llvm::Twine("expected the name of a decorator after '@', found ") +
                               tokSpelling(parser_.tok_.kind));
    return nullptr;
  }
  Expr *expr = parser_.ctx_.make<NameExpr>(parser_.tok_.loc(), parser_.tok_.text);
  parser_.cursor().advance();
  while (parser_.tok_.is(Tok::Dot)) {
    parser_.cursor().advance();
    if (!parser_.tok_.is(Tok::Identifier) && !isKeyword(parser_.tok_.kind)) {
      parser_.cursor().error(parser_.tok_.loc(), "expected a property name after '.'");
      return expr;
    }
    expr = parser_.ctx_.make<MemberExpr>(parser_.tok_.loc(), expr, parser_.tok_.text);
    parser_.cursor().advance();
  }
  if (!parser_.tok_.is(Tok::LParen)) {
    return expr;
  }
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
    args.push_back(parser_.exprs().parseExpr());
    if (!parser_.cursor().accept(Tok::Comma)) {
      break;
    }
  }
  parser_.cursor().expect(Tok::RParen, "to close the argument list");
  auto *call = parser_.ctx_.make<CallExpr>(loc, expr, std::move(args));
  call->spread = std::move(spread);

  return call;
}

void ClassParser::rejectDecorators(llvm::ArrayRef<Decorator> decorators) {
  if (!decorators.empty()) {
    parser_.cursor().report(decorators[0].loc,
                            "a decorator can decorate only a class, a member of a class or a "
                            "parameter of a method or a constructor");
  }
}

void ClassParser::skipTypeParams() {
  int depth = 0;
  do {
    if (parser_.tok_.is(Tok::Less)) {
      ++depth;
    } else if (parser_.tok_.is(Tok::Greater)) {
      --depth;
    } else if (parser_.tok_.is(Tok::Shr)) {
      depth -= 2;
    } else if (parser_.tok_.is(Tok::Eof) || parser_.tok_.is(Tok::LBrace) ||
               parser_.tok_.is(Tok::LParen)) {
      return;
    }
    parser_.cursor().advance();
  } while (depth > 0);
}

void ClassParser::parseClassMember(ClassDecl *cls) {
  if (parser_.cursor().accept(Tok::Semi)) {
    return;
  }
  std::vector<Decorator> decorators = parseDecorators();
  if (parser_.recovering_) {
    return;
  }
  llvm::SMLoc loc = parser_.tok_.loc();
  bool isStatic = false, readonly = false, isAbstract = false, isOverride = false;
  bool isAsync = false;
  bool accessor = false;
  Access access = Access::Public;
  while (memberModifierAhead()) {
    llvm::StringRef word = parser_.tok_.text;
    llvm::SMLoc at = parser_.tok_.loc();
    parser_.cursor().advance();
    if (word == "static") {
      if (parser_.tok_.is(Tok::LBrace)) {
        parser_.cursor().report(
            at, "static blocks are not supported; give each static field its value in its "
                "declaration");
        parser_.cursor().advance();
        parser_.cursor().skipToClosingBrace();
        return;
      }
      isStatic = true;
    } else if (word == "public") {
      access = Access::Public;
    } else if (word == "private") {
      access = Access::Private;
    } else if (word == "protected") {
      access = Access::Protected;
    } else if (word == "readonly") {
      readonly = true;
    } else if (word == "override") {
      isOverride = true;
    } else if (word == "abstract") {
      isAbstract = true;
    } else if (word == "declare") {
      parser_.cursor().report(
          at, "'declare' is not supported: a field gets its value in its declaration or in the "
              "constructor");
    } else if (word == "accessor") {
      accessor = true;
    } else {
      isAsync = true;
    }
  }
  bool accessorAhead =
      parser_.tok_.is(Tok::Identifier) &&
      (parser_.tok_.text == "get" || parser_.tok_.text == "set") &&
      !parser_.cursor().peek().newlineBefore &&
      (parser_.cursor().peek().is(Tok::Identifier) || isKeyword(parser_.cursor().peek().kind) ||
       parser_.cursor().peek().is(Tok::StringLiteral));
  if (accessorAhead) {
    bool getter = parser_.tok_.text == "get";
    parser_.cursor().advance();
    std::string name = parser_.tok_.text.str();
    std::string setter =
        name.empty() ? "set" : "set" + std::string(1, llvm::toUpper(name[0])) + name.substr(1);
    if (getter) {
      parser_.cursor().report(
          loc, "getters are not supported: reading a field never runs code; write a method "
               "instead, e.g. '" +
                   name + "()'");
    } else {
      parser_.cursor().report(
          loc, "setters are not supported: writing a field never runs code; write a method "
               "instead, e.g. '" +
                   setter + "(value)'");
    }
    skipClassMember();
    return;
  }
  if (parser_.tok_.is(Tok::LBracket) && !symbolKeyAhead()) {
    parser_.cursor().report(
        loc, "a member name in brackets is not supported: an object has a fixed set of fields; "
             "keep a dictionary in a field, e.g. 'items: Record<string, T> = {}' or "
             "'items = new Map<string, T>()'");
    skipClassMember();
    return;
  }
  const char *start = parser_.tok_.text.data();
  bool generator = parser_.cursor().accept(Tok::Star);
  llvm::SMLoc nameLoc = parser_.tok_.loc();
  bool plainName = parser_.tok_.is(Tok::Identifier);
  bool hash = parser_.tok_.hash;
  llvm::StringRef name;
  if (!parser_.objectTypes().parseFieldName(name)) {
    return;
  }
  if (hash) {
    parser_.cursor().report(nameLoc, "there is no '#" + name +
                                         "' in tz: a private member is 'private " + name + "'");
    access = Access::Private;
  }

  if (parser_.tok_.is(Tok::LParen) || parser_.tok_.is(Tok::Less)) {
    auto *fn = parser_.ctx_.make<FunctionDecl>(nameLoc, name);
    fn->cls = cls;
    fn->isStatic = isStatic;
    if (accessor) {
      parser_.cursor().report(loc, "'accessor' goes before a field, not a method");
    }
    if (!decorators.empty() && plainName && name == "constructor" && !isStatic) {
      parser_.cursor().report(decorators[0].loc,
                              "a decorator cannot decorate a constructor; decorate the class, "
                              "or a parameter of the constructor");
    } else {
      fn->decorators = std::move(decorators);
    }
    fn->access = access;
    fn->isConstructor = plainName && name == "constructor" && !isStatic;
    fn->isGenerator = generator && !fn->isConstructor;
    fn->isAsync = isAsync && !fn->isConstructor;
    fn->isOverride = isOverride && !fn->isConstructor;
    fn->isAbstract = isAbstract && !fn->isConstructor;
    if (generator && fn->isConstructor) {
      parser_.cursor().report(loc, "a constructor cannot be a generator: 'new' gives the object");
    }
    if (isAsync && fn->isConstructor) {
      parser_.cursor().report(
          loc, "a constructor cannot be 'async': 'new' gives the object, not a Promise");
    }
    if (readonly) {
      parser_.cursor().report(loc,
                              "a method cannot be 'readonly': a method cannot be replaced anyway");
    }
    if ((isOverride || isAbstract) && fn->isConstructor) {
      parser_.cursor().report(loc, std::string("a constructor cannot be '") +
                                       (isOverride ? "override" : "abstract") + "'");
    }
    if (isAbstract && isStatic) {
      parser_.cursor().report(
          loc, "a static method cannot be 'abstract': it belongs to the class itself");
    }
    parseMethodRest(fn, start);
    if (parser_.recovering_) {
      return;
    }
    if (fn->isAbstract) {
      if (fn->body) {
        parser_.cursor().report(nameLoc, "abstract method '" + name +
                                             "' cannot have a body: the classes that "
                                             "extend '" +
                                             cls->name +
                                             "' give it one; to give it one here, drop 'abstract'");
      }
      fn->body = parser_.ctx_.make<BlockStmt>(nameLoc);
      cls->methods.push_back(fn);
      return;
    }
    if (!fn->body) {
      std::string what = fn->isConstructor ? "the constructor" : "method '" + name.str() + "'";
      parser_.cursor().report(nameLoc,
                              what + " has no body; overload signatures over one body are not "
                                     "supported: give each variant a body of its own");
      return;
    }
    if (!fn->isConstructor) {
      cls->methods.push_back(fn);
      return;
    }
    if (fn->returnType) {
      parser_.cursor().report(fn->returnType->loc, "a constructor cannot have a return type");
    }
    std::vector<Stmt *> sets;
    size_t properties = 0;
    for (const ClassField &f : cls->fields) {
      properties += f.parameter;
    }
    for (const Param &param : fn->params) {
      if (!param.property) {
        continue;
      }
      parser_.fieldNames_.insert(param.name);
      if (!llvm::any_of(cls->fields,
                        [&](const ClassField &f) { return f.parameter && f.name == param.name; })) {
        ClassField field;
        field.name = param.name;
        field.loc = param.loc;
        field.type = param.type;
        field.readonly = param.readonly;
        field.access = param.access;
        field.parameter = true;
        cls->fields.insert(cls->fields.begin() + std::ptrdiff_t(properties++), field);
      }
      auto *target = parser_.ctx_.make<MemberExpr>(
          param.loc, parser_.ctx_.make<NameExpr>(param.loc, "this"), param.name);
      auto *set = parser_.ctx_.make<AssignExpr>(param.loc, std::nullopt, target,
                                                parser_.ctx_.make<NameExpr>(param.loc, param.name));
      sets.push_back(parser_.ctx_.make<ExprStmt>(param.loc, set));
    }
    auto at = fn->body->body.begin();
    if (cls->base) {
      for (auto it = fn->body->body.begin(); it != fn->body->body.end(); ++it) {
        auto *stmt = dyn_cast<ExprStmt>(*it);
        auto *call = stmt ? dyn_cast<CallExpr>(stmt->expr) : nullptr;
        auto *callee = call ? dyn_cast<NameExpr>(call->callee) : nullptr;
        if (callee && callee->name == "super") {
          at = it + 1;
          break;
        }
      }
    }
    fn->body->body.insert(at, sets.begin(), sets.end());
    cls->constructors.push_back(fn);
    return;
  }

  if (generator || isAsync) {
    parser_.cursor().error(parser_.tok_.loc(),
                           generator ? "expected '(' after the name of the generator method"
                                     : "expected '(' after the name of the async method");
    return;
  }
  ClassField field;
  field.name = name;
  field.loc = nameLoc;
  field.isStatic = isStatic;
  field.readonly = readonly;
  field.access = access;
  field.isOverride = isOverride;
  field.accessor = accessor;
  field.decorators = std::move(decorators);
  if (isAbstract) {
    parser_.cursor().report(
        loc, "abstract fields are not supported: declare the field in the class that sets it, "
             "or give it a value here");
  }
  if (parser_.cursor().accept(Tok::Question)) {
    field.optional = true;
  } else if (parser_.tok_.is(Tok::Bang) && !parser_.tok_.newlineBefore) {
    parser_.cursor().advance();
    field.definite = true;
  }
  if (parser_.cursor().accept(Tok::Colon)) {
    field.type = parser_.types().parseType();
  }
  if (parser_.cursor().accept(Tok::Assign)) {
    field.init = parser_.exprs().parseExpr();
  }
  if (!parser_.cursor().accept(Tok::Semi) && !parser_.cursor().accept(Tok::Comma) &&
      !parser_.tok_.is(Tok::RBrace) && !parser_.tok_.newlineBefore) {
    parser_.cursor().error(parser_.tok_.loc(), llvm::Twine("expected ';' after the field, found ") +
                                                   tokSpelling(parser_.tok_.kind));
    return;
  }
  cls->fields.push_back(std::move(field));
}

void ClassParser::parseMethodRest(FunctionDecl *fn, const char *start) {
  llvm::SaveAndRestore<bool> inGenerator(parser_.inGenerator_, fn->isGenerator);
  if (parser_.tok_.is(Tok::Less)) {
    fn->typeParams = parser_.functions().parseTypeParams();
  }
  Parser::ParamPatterns patterns;
  parser_.functions().parseParams(fn, &patterns);
  if (parser_.cursor().accept(Tok::Colon)) {
    fn->returnType = parser_.types().parseReturnType();
  }
  if (parser_.tok_.is(Tok::LBrace)) {
    fn->body = parser_.stmts().parseBlock();
    parser_.functions().addPrologue(fn, patterns);
  } else if (!parser_.cursor().accept(Tok::Semi) && !parser_.tok_.newlineBefore &&
             !parser_.tok_.is(Tok::RBrace) && !parser_.tok_.is(Tok::Eof)) {
    parser_.cursor().error(parser_.tok_.loc(), "expected '{' to start the body of the method");
  }
  fn->source = parser_.cursor().textFrom(start);
}

bool ClassParser::symbolKeyAhead() {
  return parser_.tok_.is(Tok::LBracket) && parser_.cursor().peek().is(Tok::Identifier) &&
         parser_.cursor().peek().text == "Symbol";
}

}
