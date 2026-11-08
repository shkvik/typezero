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

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

ClassParser::ClassParser(Parser &parser)
    : parser(parser) {}

ClassDecl *ClassParser::parseClass(bool expression) {
  const char *start = this->parser.tok.text.data();
  llvm::SMLoc loc = this->parser.tok.loc();
  this->parser.cursor().advance();
  bool named = this->parser.tok.is(Tok::Identifier) &&
               !(expression &&
                 (this->parser.tok.text == "extends" || this->parser.tok.text == "implements"));
  auto *cls = this->parser.ctx.make<ClassDecl>(named ? this->parser.tok.loc() : loc,
                                               named ? this->parser.tok.text : llvm::StringRef());
  if (expression) {
    cls->syntax = ExpressionClass{};
  }
  if (named) {
    this->parser.cursor().advance();
  }
  if (this->parser.tok.is(Tok::Less)) {
    cls->typeParams = this->parser.functions().parseTypeParams();
  }
  if (!cls->typeParams.empty()) {
    cls->generics = GenericTemplate{};
  }
  if (this->parser.tok.is(Tok::Identifier) && this->parser.tok.text == "extends") {
    this->parser.cursor().advance();
    const Token &next = this->parser.cursor().peek();
    bool value = !this->parser.tok.is(Tok::Identifier) || next.is(Tok::LParen) ||
                 next.is(Tok::Dot) || next.is(Tok::LBracket);
    if (value) {
      cls->baseExpr = this->parser.calls().parseCallOrMember();
    } else {
      llvm::SMLoc baseLoc = this->parser.tok.loc();
      llvm::StringRef baseName = this->parser.tok.text;
      cls->base = this->parser.types().parsePrimaryType();
      if (expression && cls->base->kind() == TypeRefKind::NAME_REF) {
        cls->baseExpr = this->parser.ctx.make<NameExpr>(baseLoc, baseName);
      }
    }
  }
  if (this->parser.tok.is(Tok::Identifier) && this->parser.tok.text == "implements") {
    this->parser.cursor().advance();
    do {
      cls->interfaces.push_back(this->parser.types().parsePrimaryType());
    } while (this->parser.cursor().accept(Tok::Comma));
  }
  if (!this->parser.cursor().expect(Tok::LBrace, "to start the members of the class")) {
    return cls;
  }
  while (!this->parser.tok.is(Tok::RBrace) && !this->parser.tok.is(Tok::Eof)) {
    parseClassMember(cls);
    if (this->parser.recovering) {
      skipClassMember();
      this->parser.recovering = false;
    }
  }
  this->parser.cursor().expect(Tok::RBrace, "to close the class");
  cls->source = this->parser.cursor().textFrom(start);

  return cls;
}

bool ClassParser::memberModifierAhead() {
  static const char *const modifiers[] = {"public",   "private",  "protected", "readonly", "static",
                                          "override", "abstract", "declare",   "accessor", "async"};
  if (!this->parser.tok.is(Tok::Identifier) ||
      !llvm::is_contained(modifiers, this->parser.tok.text)) {
    return false;
  }
  const Token &next = this->parser.cursor().peek();
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
  while (!this->parser.tok.is(Tok::Eof) && !this->parser.tok.is(Tok::RBrace)) {
    if (moved && this->parser.tok.newlineBefore) {
      return;
    }
    if (this->parser.cursor().accept(Tok::Semi)) {
      return;
    }
    if (this->parser.tok.is(Tok::LBrace)) {
      this->parser.cursor().advance();
      this->parser.cursor().skipToClosingBrace();
    } else {
      this->parser.cursor().advance();
    }
    moved = true;
  }
}

std::vector<Decorator> ClassParser::parseDecorators() {
  std::vector<Decorator> decorators;
  while (this->parser.tok.is(Tok::At)) {
    llvm::SMLoc loc = this->parser.tok.loc();
    this->parser.cursor().advance();
    if (Expr *expr = parseDecoratorExpr()) {
      decorators.push_back({loc, expr});
    }
    if (this->parser.recovering) {
      break;
    }
  }

  return decorators;
}

Expr *ClassParser::parseDecoratorExpr() {
  if (this->parser.cursor().accept(Tok::LParen)) {
    Expr *expr = this->parser.exprs().parseSequence();
    this->parser.cursor().expect(Tok::RParen, "to close the decorator");
    return expr;
  }
  if (!this->parser.tok.is(Tok::Identifier)) {
    this->parser.cursor().error(this->parser.tok.loc(),
                                llvm::Twine("expected the name of a decorator after '@', found ") +
                                    TokenKinds::tokSpelling(this->parser.tok.kind));
    return nullptr;
  }
  Expr *expr = this->parser.ctx.make<NameExpr>(this->parser.tok.loc(), this->parser.tok.text);
  this->parser.cursor().advance();
  while (this->parser.tok.is(Tok::Dot)) {
    this->parser.cursor().advance();
    if (!this->parser.tok.is(Tok::Identifier) && !TokenKinds::isKeyword(this->parser.tok.kind)) {
      this->parser.cursor().error(this->parser.tok.loc(), "expected a property name after '.'");
      return expr;
    }
    expr = this->parser.ctx.make<MemberExpr>(this->parser.tok.loc(), expr, this->parser.tok.text);
    this->parser.cursor().advance();
  }
  if (!this->parser.tok.is(Tok::LParen)) {
    return expr;
  }
  llvm::SMLoc loc = expr->loc();
  this->parser.cursor().advance();
  llvm::SaveAndRestore<bool> allowIn(this->parser.noIn, false);
  std::vector<Expr *> args;
  std::vector<bool> spread;
  while (!this->parser.tok.is(Tok::RParen) && !this->parser.tok.is(Tok::Eof)) {
    bool spreads = this->parser.cursor().accept(Tok::Ellipsis);
    if (spreads || !spread.empty()) {
      spread.resize(args.size(), false);
      spread.push_back(spreads);
    }
    args.push_back(this->parser.exprs().parseExpr());
    if (!this->parser.cursor().accept(Tok::Comma)) {
      break;
    }
  }
  this->parser.cursor().expect(Tok::RParen, "to close the argument list");
  auto *call = this->parser.ctx.make<CallExpr>(loc, expr, std::move(args));
  call->spread = std::move(spread);

  return call;
}

void ClassParser::rejectDecorators(llvm::ArrayRef<Decorator> decorators) {
  if (!decorators.empty()) {
    this->parser.cursor().report(decorators[0].loc,
                                 "a decorator can decorate only a class, a member of a class or a "
                                 "parameter of a method or a constructor");
  }
}

void ClassParser::skipTypeParams() {
  int depth = 0;
  do {
    if (this->parser.tok.is(Tok::Less)) {
      ++depth;
    } else if (this->parser.tok.is(Tok::Greater)) {
      --depth;
    } else if (this->parser.tok.is(Tok::Shr)) {
      depth -= 2;
    } else if (this->parser.tok.is(Tok::Eof) || this->parser.tok.is(Tok::LBrace) ||
               this->parser.tok.is(Tok::LParen)) {
      return;
    }
    this->parser.cursor().advance();
  } while (depth > 0);
}

void ClassParser::parseClassMember(ClassDecl *cls) {
  if (this->parser.cursor().accept(Tok::Semi)) {
    return;
  }
  std::vector<Decorator> decorators = parseDecorators();
  if (this->parser.recovering) {
    return;
  }
  llvm::SMLoc loc = this->parser.tok.loc();
  bool isStatic = false, readonly = false, isAbstract = false, isOverride = false;
  bool isAsync = false;
  bool accessor = false;
  Access access = Access::Public;
  while (memberModifierAhead()) {
    llvm::StringRef word = this->parser.tok.text;
    llvm::SMLoc at = this->parser.tok.loc();
    this->parser.cursor().advance();
    if (word == "static") {
      if (this->parser.tok.is(Tok::LBrace)) {
        this->parser.cursor().report(
            at, "static blocks are not supported; give each static field its value in its "
                "declaration");
        this->parser.cursor().advance();
        this->parser.cursor().skipToClosingBrace();
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
      this->parser.cursor().report(
          at, "'declare' is not supported: a field gets its value in its declaration or in the "
              "constructor");
    } else if (word == "accessor") {
      accessor = true;
    } else {
      isAsync = true;
    }
  }
  bool accessorAhead = this->parser.tok.is(Tok::Identifier) &&
                       (this->parser.tok.text == "get" || this->parser.tok.text == "set") &&
                       !this->parser.cursor().peek().newlineBefore &&
                       (this->parser.cursor().peek().is(Tok::Identifier) ||
                        TokenKinds::isKeyword(this->parser.cursor().peek().kind) ||
                        this->parser.cursor().peek().is(Tok::StringLiteral));
  if (accessorAhead) {
    bool getter = this->parser.tok.text == "get";
    this->parser.cursor().advance();
    std::string name = this->parser.tok.text.str();
    std::string setter =
        name.empty() ? "set" : "set" + std::string(1, llvm::toUpper(name[0])) + name.substr(1);
    if (getter) {
      this->parser.cursor().report(
          loc, "getters are not supported: reading a field never runs code; write a method "
               "instead, e.g. '" +
                   name + "()'");
    } else {
      this->parser.cursor().report(
          loc, "setters are not supported: writing a field never runs code; write a method "
               "instead, e.g. '" +
                   setter + "(value)'");
    }
    skipClassMember();
    return;
  }
  if (this->parser.tok.is(Tok::LBracket) && !symbolKeyAhead()) {
    this->parser.cursor().report(
        loc, "a member name in brackets is not supported: an object has a fixed set of fields; "
             "keep a dictionary in a field, e.g. 'items: Record<string, T> = {}' or "
             "'items = new Map<string, T>()'");
    skipClassMember();
    return;
  }
  const char *start = this->parser.tok.text.data();
  bool generator = this->parser.cursor().accept(Tok::Star);
  llvm::SMLoc nameLoc = this->parser.tok.loc();
  bool plainName = this->parser.tok.is(Tok::Identifier);
  bool hash = this->parser.tok.hash;
  llvm::StringRef name;
  if (!this->parser.objectTypes().parseFieldName(name)) {
    return;
  }
  if (hash) {
    this->parser.cursor().report(
        nameLoc, "there is no '#" + name + "' in tz: a private member is 'private " + name + "'");
    access = Access::Private;
  }

  if (this->parser.tok.is(Tok::LParen) || this->parser.tok.is(Tok::Less)) {
    auto *fn = this->parser.ctx.make<FunctionDecl>(nameLoc, name);
    bool constructor = plainName && name == "constructor" && !isStatic;
    bool abstractMethod = isAbstract && !constructor;
    if (accessor) {
      this->parser.cursor().report(loc, "'accessor' goes before a field, not a method");
    }
    ClassMember member{.cls = cls, .access = access, .isOverride = isOverride && !constructor};
    if (!decorators.empty() && constructor) {
      this->parser.cursor().report(decorators[0].loc,
                                   "a decorator cannot decorate a constructor; decorate the class, "
                                   "or a parameter of the constructor");
    } else {
      member.decorators = std::move(decorators);
    }
    if (constructor) {
      member.method = Constructor{};
    } else if (isStatic) {
      member.method = StaticMethod{};
    } else if (abstractMethod) {
      member.method = AbstractMethod{};
    }
    fn->role = std::move(member);
    fn->isGenerator = generator && !constructor;
    fn->isAsync = isAsync && !constructor;
    if (generator && fn->isConstructor()) {
      this->parser.cursor().report(loc,
                                   "a constructor cannot be a generator: 'new' gives the object");
    }
    if (isAsync && fn->isConstructor()) {
      this->parser.cursor().report(
          loc, "a constructor cannot be 'async': 'new' gives the object, not a Promise");
    }
    if (readonly) {
      this->parser.cursor().report(
          loc, "a method cannot be 'readonly': a method cannot be replaced anyway");
    }
    if ((isOverride || isAbstract) && fn->isConstructor()) {
      this->parser.cursor().report(loc, std::string("a constructor cannot be '") +
                                            (isOverride ? "override" : "abstract") + "'");
    }
    if (isAbstract && isStatic) {
      this->parser.cursor().report(
          loc, "a static method cannot be 'abstract': it belongs to the class itself");
    }
    parseMethodRest(fn, start);
    if (this->parser.recovering) {
      return;
    }
    if (abstractMethod) {
      if (fn->body) {
        this->parser.cursor().report(
            nameLoc, "abstract method '" + name +
                         "' cannot have a body: the classes that "
                         "extend '" +
                         cls->name + "' give it one; to give it one here, drop 'abstract'");
      }
      fn->body = this->parser.ctx.make<BlockStmt>(nameLoc);
      cls->methods.push_back(fn);
      return;
    }
    if (!fn->body) {
      std::string what = fn->isConstructor() ? "the constructor" : "method '" + name.str() + "'";
      this->parser.cursor().report(nameLoc,
                                   what + " has no body; overload signatures over one body are not "
                                          "supported: give each variant a body of its own");
      return;
    }
    if (!fn->isConstructor()) {
      cls->methods.push_back(fn);
      return;
    }
    if (fn->returnType) {
      this->parser.cursor().report(fn->returnType->loc, "a constructor cannot have a return type");
    }
    std::vector<Stmt *> sets;
    size_t properties = 0;
    for (const ClassField &f : cls->fields) {
      properties += f.parameter();
    }
    for (const Param &param : fn->params) {
      if (!param.property) {
        continue;
      }
      this->parser.fieldNames.insert(param.name);
      if (!llvm::any_of(cls->fields, [&](const ClassField &f) {
            return f.parameter() && f.name == param.name;
          })) {
        ClassField field;
        field.name = param.name;
        field.loc = param.loc;
        field.type = param.type;
        field.readonly = param.property->readonly;
        field.access = param.property->access;
        field.form = ParameterField{};
        cls->fields.insert(cls->fields.begin() + std::ptrdiff_t(properties++), field);
      }
      auto *target = this->parser.ctx.make<MemberExpr>(
          param.loc, this->parser.ctx.make<NameExpr>(param.loc, "this"), param.name);
      auto *set = this->parser.ctx.make<AssignExpr>(
          param.loc, std::nullopt, target, this->parser.ctx.make<NameExpr>(param.loc, param.name));
      sets.push_back(this->parser.ctx.make<ExprStmt>(param.loc, set));
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
    this->parser.cursor().error(this->parser.tok.loc(),
                                generator ? "expected '(' after the name of the generator method"
                                          : "expected '(' after the name of the async method");
    return;
  }
  ClassField field;
  field.name = name;
  field.loc = nameLoc;
  if (isStatic) {
    field.form = StaticField{};
  }
  field.readonly = readonly;
  field.access = access;
  field.isOverride = isOverride;
  field.accessor = accessor;
  field.decorators = std::move(decorators);
  if (isAbstract) {
    this->parser.cursor().report(
        loc, "abstract fields are not supported: declare the field in the class that sets it, "
             "or give it a value here");
  }
  if (this->parser.cursor().accept(Tok::Question)) {
    field.presence = FieldPresence::OPTIONAL;
  } else if (this->parser.tok.is(Tok::Bang) && !this->parser.tok.newlineBefore) {
    this->parser.cursor().advance();
    field.presence = FieldPresence::DEFINITE;
  }
  if (this->parser.cursor().accept(Tok::Colon)) {
    field.type = this->parser.types().parseType();
  }
  if (this->parser.cursor().accept(Tok::Assign)) {
    field.init = this->parser.exprs().parseExpr();
  }
  if (!this->parser.cursor().accept(Tok::Semi) && !this->parser.cursor().accept(Tok::Comma) &&
      !this->parser.tok.is(Tok::RBrace) && !this->parser.tok.newlineBefore) {
    this->parser.cursor().error(this->parser.tok.loc(),
                                llvm::Twine("expected ';' after the field, found ") +
                                    TokenKinds::tokSpelling(this->parser.tok.kind));
    return;
  }
  cls->fields.push_back(std::move(field));
}

void ClassParser::parseMethodRest(FunctionDecl *fn, const char *start) {
  llvm::SaveAndRestore<bool> inGenerator(this->parser.inGenerator, fn->isGenerator);
  if (this->parser.tok.is(Tok::Less)) {
    fn->typeParams = this->parser.functions().parseTypeParams();
  }
  Parser::ParamPatterns patterns;
  this->parser.functions().parseParams(fn, &patterns);
  if (this->parser.cursor().accept(Tok::Colon)) {
    fn->returnType = this->parser.types().parseReturnType();
  }
  if (this->parser.tok.is(Tok::LBrace)) {
    fn->body = this->parser.stmts().parseBlock();
    this->parser.functions().addPrologue(fn, patterns);
  } else if (!this->parser.cursor().accept(Tok::Semi) && !this->parser.tok.newlineBefore &&
             !this->parser.tok.is(Tok::RBrace) && !this->parser.tok.is(Tok::Eof)) {
    this->parser.cursor().error(this->parser.tok.loc(),
                                "expected '{' to start the body of the method");
  }
  fn->source = this->parser.cursor().textFrom(start);
}

bool ClassParser::symbolKeyAhead() {
  return this->parser.tok.is(Tok::LBracket) && this->parser.cursor().peek().is(Tok::Identifier) &&
         this->parser.cursor().peek().text == "Symbol";
}

}
