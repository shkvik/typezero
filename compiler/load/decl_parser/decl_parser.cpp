#include "compiler/load/decl_parser/decl_parser.h"
#include "compiler/load/class_parser/class_parser.h"
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

DeclParser::DeclParser(Parser &parser)
    : parser(parser) {}

Stmt *DeclParser::parseTypeAlias() {
  this->parser.cursor().advance();
  llvm::SMLoc loc = this->parser.tok.loc();
  llvm::StringRef name = this->parser.tok.text;
  this->parser.cursor().advance();
  std::vector<TypeParamDecl> typeParams;
  if (this->parser.tok.is(Tok::Less)) {
    typeParams = this->parser.functions().parseTypeParams();
  }
  this->parser.cursor().expect(Tok::Assign, "after the name of the type");
  TypeRef type = this->parser.types().parseType();
  this->parser.cursor().expectSemicolon();
  auto *alias = this->parser.ctx.make<TypeAliasDecl>(loc, name, std::move(type));
  alias->typeParams = std::move(typeParams);

  return alias;
}

Stmt *DeclParser::parseInterface() {
  this->parser.cursor().advance();
  llvm::SMLoc loc = this->parser.tok.loc();
  llvm::StringRef name = this->parser.tok.text;
  this->parser.cursor().advance();
  std::vector<TypeParamDecl> typeParams;
  if (this->parser.tok.is(Tok::Less)) {
    typeParams = this->parser.functions().parseTypeParams();
  }
  std::vector<TypeRef> bases;
  if (this->parser.tok.is(Tok::Identifier) && this->parser.tok.text == "extends") {
    this->parser.cursor().advance();
    do {
      bases.push_back(this->parser.types().parseType());
    } while (this->parser.cursor().accept(Tok::Comma));
  }
  if (!this->parser.tok.is(Tok::LBrace)) {
    this->parser.cursor().error(
        this->parser.tok.loc(),
        llvm::Twine("expected '{' to start the fields of the interface, found ") +
            TokenKinds::tokSpelling(this->parser.tok.kind));
    return this->parser.ctx.make<EmptyStmt>(loc);
  }
  auto *decl =
      this->parser.ctx.make<TypeAliasDecl>(loc, name, this->parser.objectTypes().parseObjectType());
  decl->bases = std::move(bases);
  decl->typeParams = std::move(typeParams);
  this->parser.cursor().accept(Tok::Semi);

  return decl;
}

Stmt *DeclParser::parseEnum() {
  this->parser.cursor().advance();
  llvm::SMLoc loc = this->parser.tok.loc();
  if (!this->parser.tok.is(Tok::Identifier)) {
    this->parser.cursor().error(this->parser.tok.loc(), "expected the name of the enum");
    return this->parser.ctx.make<EmptyStmt>(loc);
  }
  auto *decl = this->parser.ctx.make<EnumDecl>(loc, this->parser.tok.text);
  this->parser.cursor().advance();
  this->parser.cursor().expect(Tok::LBrace, "to start the members of the enum");
  while (!this->parser.tok.is(Tok::RBrace) && !this->parser.tok.is(Tok::Eof)) {
    EnumMember member;
    member.loc = this->parser.tok.loc();
    if (!this->parser.tok.is(Tok::Identifier)) {
      this->parser.cursor().error(this->parser.tok.loc(),
                                  llvm::Twine("expected the name of an enum member, found ") +
                                      TokenKinds::tokSpelling(this->parser.tok.kind));
      break;
    }
    member.name = this->parser.tok.text;
    this->parser.cursor().advance();
    if (this->parser.cursor().accept(Tok::Assign)) {
      member.init = this->parser.exprs().parseExpr();
      this->parser.vars().recordConstant((decl->name + "." + member.name).str(), member.init);
    }
    decl->members.push_back(member);
    if (!this->parser.cursor().accept(Tok::Comma)) {
      break;
    }
  }
  this->parser.cursor().expect(Tok::RBrace, "to close the enum");

  return decl;
}

}
