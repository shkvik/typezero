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

DeclarationParser::DeclarationParser(Parser &parser) : parser_(parser) {}

DeclarationFile DeclarationParser::parse() {
  DeclarationFile file;
  while (!parser_.tok_.is(Tok::Eof)) {
    parseItem(file);
  }

  return file;
}

void DeclarationParser::skipItem() {
  int depth = 0;
  while (!parser_.tok_.is(Tok::Eof)) {
    if (parser_.tok_.is(Tok::LBrace)) {
      ++depth;
    } else if (parser_.tok_.is(Tok::RBrace)) {
      --depth;
      if (depth <= 0) {
        parser_.cursor().advance();
        return;
      }
    } else if (parser_.tok_.is(Tok::Semi) && depth == 0) {
      parser_.cursor().advance();
      return;
    }
    parser_.cursor().advance();
  }
}

void DeclarationParser::parseItem(DeclarationFile &file) {
  llvm::SMLoc loc = parser_.tok_.loc();
  if (parser_.tok_.is(Tok::Identifier) && parser_.tok_.text == "import") {
    parser_.modules().parseImport(true);
    return;
  }
  if (!parser_.tok_.is(Tok::Identifier) || parser_.tok_.text != "export") {
    parser_.cursor().report(loc, "a declaration file holds only what its module exports: 'export "
                                 "type', 'export declare function', 'export declare class' and "
                                 "imports of the types they name");
    skipItem();
    return;
  }
  const Token &next = parser_.cursor().peek();
  if (next.is(Tok::LBrace) || next.is(Tok::Star)) {
    parser_.modules().parseExport(true);
    return;
  }
  parser_.cursor().advance();
  if (parser_.tok_.is(Tok::Identifier) &&
      (parser_.tok_.text == "type" || parser_.tok_.text == "interface")) {
    if (Stmt *stmt = parser_.stmts().parseStatement(true)) {
      file.items.push_back(stmt);
    }
    return;
  }
  if (!parser_.tok_.is(Tok::Identifier) || parser_.tok_.text != "declare") {
    parser_.cursor().report(parser_.tok_.loc(),
                            "a declaration file has no code: write 'export declare function' or "
                            "'export declare class', with signatures and no bodies");
    skipItem();
    return;
  }
  parser_.cursor().advance();
  if (parser_.tok_.is(Tok::KwFunction)) {
    FunctionDecl *fn = parser_.functions().parseFunction(false, false);
    if (fn->body) {
      parser_.cursor().report(fn->loc(), "a function of a declaration file has no body: its code "
                                         "is in the module");
    }
    file.items.push_back(fn);
    return;
  }
  bool isAbstract = parser_.tok_.is(Tok::Identifier) && parser_.tok_.text == "abstract";
  if (isAbstract) {
    parser_.cursor().advance();
  }
  if (parser_.tok_.is(Tok::Identifier) && parser_.tok_.text == "class") {
    parseClass(file, isAbstract);
    return;
  }
  parser_.cursor().report(parser_.tok_.loc(),
                          "expected 'function' or 'class' after 'export declare'");
  skipItem();
}

void DeclarationParser::parseClass(DeclarationFile &file, bool isAbstract) {
  parser_.cursor().advance();
  DeclaredClass cls;
  cls.isAbstract = isAbstract;
  cls.loc = parser_.tok_.loc();
  if (parser_.tok_.is(Tok::Identifier)) {
    cls.name = parser_.tok_.text;
    parser_.cursor().advance();
  } else {
    parser_.cursor().error(parser_.tok_.loc(), "expected the name of the class");
  }
  if (parser_.tok_.is(Tok::Less)) {
    cls.typeParams = parser_.functions().parseTypeParams();
  }
  if (!parser_.tok_.is(Tok::LBrace)) {
    parser_.cursor().report(parser_.tok_.loc(),
                            "a class of a declaration file lists its members only; what it "
                            "extends and implements is in the module");
    while (!parser_.tok_.is(Tok::LBrace) && !parser_.tok_.is(Tok::Eof)) {
      parser_.cursor().advance();
    }
  }
  parser_.cursor().expect(Tok::LBrace, "to start the members of the class");
  while (!parser_.tok_.is(Tok::RBrace) && !parser_.tok_.is(Tok::Eof)) {
    parseMember(cls);
  }
  parser_.cursor().expect(Tok::RBrace, "to end the class");
  file.classes.push_back(std::move(cls));
}

bool DeclarationParser::memberWordAhead() const {
  const Token &next = parser_.cursor().peek();

  return next.is(Tok::Identifier) || isKeyword(next.kind) || next.is(Tok::StringLiteral) ||
         next.is(Tok::LBracket);
}

void DeclarationParser::parseMember(DeclaredClass &cls) {
  DeclaredMember member;
  for (;;) {
    if (!parser_.tok_.is(Tok::Identifier) || !memberWordAhead()) {
      break;
    }
    llvm::StringRef word = parser_.tok_.text;
    if (word == "public" || word == "private" || word == "protected") {
      member.access = word == "public"    ? Access::Public
                      : word == "private" ? Access::Private
                                          : Access::Protected;
      member.accessWritten = true;
    } else if (word == "static") {
      member.isStatic = true;
    } else if (word == "readonly") {
      member.readonly = true;
    } else if (word == "async") {
      member.isAsync = true;
    } else if (word != "abstract" && word != "override") {
      break;
    }
    parser_.cursor().advance();
  }
  member.loc = parser_.tok_.loc();
  if (parser_.classes().symbolKeyAhead()) {
    if (!parser_.objectTypes().parseFieldName(member.name)) {
      skipItem();
      return;
    }
  } else if (parser_.tok_.is(Tok::Identifier) || isKeyword(parser_.tok_.kind)) {
    member.name = parser_.tok_.text;
    parser_.cursor().advance();
  } else {
    parser_.cursor().error(parser_.tok_.loc(), "expected the name of a member");
    skipItem();
    return;
  }
  member.constructor = member.name == "constructor";
  if (parser_.cursor().accept(Tok::Question)) {
    member.optional = true;
  }
  if (parser_.tok_.is(Tok::LParen) || parser_.tok_.is(Tok::Less)) {
    member.method = !member.constructor;
    if (parser_.tok_.is(Tok::Less)) {
      member.typeParams = parser_.functions().parseTypeParams();
    }
    auto *fn = parser_.ctx_.make<FunctionDecl>(member.loc, member.name);
    parser_.functions().parseParams(fn);
    member.params = fn->params;
    if (parser_.cursor().accept(Tok::Colon)) {
      member.type = parser_.types().parseReturnType();
    }
    if (parser_.tok_.is(Tok::LBrace)) {
      parser_.cursor().report(parser_.tok_.loc(), "a method of a declaration file has no body: "
                                                  "its code is in the module");
      skipItem();
    } else {
      parser_.cursor().expectSemicolon();
    }
    cls.members.push_back(std::move(member));
    return;
  }
  if (parser_.cursor().accept(Tok::Colon)) {
    member.type = parser_.types().parseType();
  } else {
    parser_.cursor().error(parser_.tok_.loc(), "expected ':' and the type of the field");
  }
  parser_.cursor().expectSemicolon();
  cls.members.push_back(std::move(member));
}

DeclarationFile Parser::parseDeclarationFile() { return DeclarationParser(*this).parse(); }

}
