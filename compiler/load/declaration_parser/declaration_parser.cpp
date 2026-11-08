#include "compiler/load/declaration_parser/declaration_parser.h"
#include "compiler/load/class_parser/class_parser.h"
#include "compiler/load/function_parser/function_parser.h"
#include "compiler/load/module_parser/module_parser.h"
#include "compiler/load/object_type_parser/object_type_parser.h"
#include "compiler/load/stmt_parser/stmt_parser.h"
#include "compiler/load/token_cursor/token_cursor.h"
#include "compiler/load/type_parser/type_parser.h"
#include "compiler/program/ast/ast.h"

namespace tz {

DeclarationParser::DeclarationParser(Parser &parser)
    : parser(parser) {}

DeclarationFile DeclarationParser::parse() {
  DeclarationFile file;
  while (!this->parser.tok.is(Tok::Eof)) {
    parseItem(file);
  }

  return file;
}

void DeclarationParser::skipItem() {
  int depth = 0;
  while (!this->parser.tok.is(Tok::Eof)) {
    if (this->parser.tok.is(Tok::LBrace)) {
      ++depth;
    } else if (this->parser.tok.is(Tok::RBrace)) {
      --depth;
      if (depth <= 0) {
        this->parser.cursor().advance();
        return;
      }
    } else if (this->parser.tok.is(Tok::Semi) && depth == 0) {
      this->parser.cursor().advance();
      return;
    }
    this->parser.cursor().advance();
  }
}

void DeclarationParser::parseItem(DeclarationFile &file) {
  llvm::SMLoc loc = this->parser.tok.loc();
  if (this->parser.tok.is(Tok::Identifier) && this->parser.tok.text == "import") {
    this->parser.modules().parseImport(true);
    return;
  }
  if (!this->parser.tok.is(Tok::Identifier) || this->parser.tok.text != "export") {
    this->parser.cursor().report(loc,
                                 "a declaration file holds only what its module exports: 'export "
                                 "type', 'export declare function', 'export declare class' and "
                                 "imports of the types they name");
    skipItem();
    return;
  }
  const Token &next = this->parser.cursor().peek();
  if (next.is(Tok::LBrace) || next.is(Tok::Star)) {
    this->parser.modules().parseExport(true);
    return;
  }
  this->parser.cursor().advance();
  if (this->parser.tok.is(Tok::Identifier) &&
      (this->parser.tok.text == "type" || this->parser.tok.text == "interface")) {
    if (Stmt *stmt = this->parser.stmts().parseStatement(true)) {
      file.items.push_back(stmt);
    }
    return;
  }
  if (!this->parser.tok.is(Tok::Identifier) || this->parser.tok.text != "declare") {
    this->parser.cursor().report(
        this->parser.tok.loc(),
        "a declaration file has no code: write 'export declare function' or "
        "'export declare class', with signatures and no bodies");
    skipItem();
    return;
  }
  this->parser.cursor().advance();
  if (this->parser.tok.is(Tok::KwFunction)) {
    FunctionDecl *fn = this->parser.functions().parseFunction(false, false);
    if (fn->body) {
      this->parser.cursor().report(fn->loc(),
                                   "a function of a declaration file has no body: its code "
                                   "is in the module");
    }
    file.items.push_back(fn);
    return;
  }
  bool isAbstract = this->parser.tok.is(Tok::Identifier) && this->parser.tok.text == "abstract";
  if (isAbstract) {
    this->parser.cursor().advance();
  }
  if (this->parser.tok.is(Tok::Identifier) && this->parser.tok.text == "class") {
    parseClass(file, isAbstract);
    return;
  }
  this->parser.cursor().report(this->parser.tok.loc(),
                               "expected 'function' or 'class' after 'export declare'");
  skipItem();
}

void DeclarationParser::parseClass(DeclarationFile &file, bool isAbstract) {
  this->parser.cursor().advance();
  DeclaredClass cls;
  cls.isAbstract = isAbstract;
  cls.loc = this->parser.tok.loc();
  if (this->parser.tok.is(Tok::Identifier)) {
    cls.name = this->parser.tok.text;
    this->parser.cursor().advance();
  } else {
    this->parser.cursor().error(this->parser.tok.loc(), "expected the name of the class");
  }
  if (this->parser.tok.is(Tok::Less)) {
    cls.typeParams = this->parser.functions().parseTypeParams();
  }
  if (!this->parser.tok.is(Tok::LBrace)) {
    this->parser.cursor().report(this->parser.tok.loc(),
                                 "a class of a declaration file lists its members only; what it "
                                 "extends and implements is in the module");
    while (!this->parser.tok.is(Tok::LBrace) && !this->parser.tok.is(Tok::Eof)) {
      this->parser.cursor().advance();
    }
  }
  this->parser.cursor().expect(Tok::LBrace, "to start the members of the class");
  while (!this->parser.tok.is(Tok::RBrace) && !this->parser.tok.is(Tok::Eof)) {
    parseMember(cls);
  }
  this->parser.cursor().expect(Tok::RBrace, "to end the class");
  file.classes.push_back(std::move(cls));
}

bool DeclarationParser::memberWordAhead() const {
  const Token &next = this->parser.cursor().peek();

  return next.is(Tok::Identifier) || TokenKinds::isKeyword(next.kind) ||
         next.is(Tok::StringLiteral) || next.is(Tok::LBracket);
}

void DeclarationParser::parseMember(DeclaredClass &cls) {
  DeclaredMember member;
  DeclaredField field;
  for (;;) {
    if (!this->parser.tok.is(Tok::Identifier) || !memberWordAhead()) {
      break;
    }
    llvm::StringRef word = this->parser.tok.text;
    if (word == "public" || word == "private" || word == "protected") {
      member.access = word == "public"    ? Access::Public
                      : word == "private" ? Access::Private
                                          : Access::Protected;
      member.accessWritten = true;
    } else if (word == "static") {
      member.isStatic = true;
    } else if (word == "readonly") {
      field.readonly = true;
    } else if (word != "async" && word != "abstract" && word != "override") {
      break;
    }
    this->parser.cursor().advance();
  }
  member.loc = this->parser.tok.loc();
  if (this->parser.classes().symbolKeyAhead()) {
    if (!this->parser.objectTypes().parseFieldName(member.name)) {
      skipItem();
      return;
    }
  } else if (this->parser.tok.is(Tok::Identifier) || TokenKinds::isKeyword(this->parser.tok.kind)) {
    member.name = this->parser.tok.text;
    this->parser.cursor().advance();
  } else {
    this->parser.cursor().error(this->parser.tok.loc(), "expected the name of a member");
    skipItem();
    return;
  }
  bool constructor = member.name == "constructor";
  if (this->parser.cursor().accept(Tok::Question)) {
    field.optional = true;
  }
  if (this->parser.tok.is(Tok::LParen) || this->parser.tok.is(Tok::Less)) {
    DeclaredSignature signature;
    if (this->parser.tok.is(Tok::Less)) {
      signature.typeParams = this->parser.functions().parseTypeParams();
    }
    auto *fn = this->parser.ctx.make<FunctionDecl>(member.loc, member.name);
    this->parser.functions().parseParams(fn);
    signature.params = fn->params;
    if (constructor) {
      member.form = DeclaredConstructor{.signature = std::move(signature)};
    } else {
      member.form = DeclaredMethod{.signature = std::move(signature)};
    }
    if (this->parser.cursor().accept(Tok::Colon)) {
      member.type = this->parser.types().parseReturnType();
    }
    if (this->parser.tok.is(Tok::LBrace)) {
      this->parser.cursor().report(this->parser.tok.loc(),
                                   "a method of a declaration file has no body: "
                                   "its code is in the module");
      skipItem();
    } else {
      this->parser.cursor().expectSemicolon();
    }
    cls.members.push_back(std::move(member));
    return;
  }
  if (constructor) {
    member.form = DeclaredConstructor{};
  } else {
    member.form = field;
  }
  if (this->parser.cursor().accept(Tok::Colon)) {
    member.type = this->parser.types().parseType();
  } else {
    this->parser.cursor().error(this->parser.tok.loc(), "expected ':' and the type of the field");
  }
  this->parser.cursor().expectSemicolon();
  cls.members.push_back(std::move(member));
}

DeclarationFile Parser::parseDeclarationFile() { return DeclarationParser(*this).parse(); }

}
