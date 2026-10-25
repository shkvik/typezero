#include "compiler/load/decl_parser/decl_parser.h"
#include "compiler/load/class_parser/class_parser.h"
#include "compiler/load/expr_parser/expr_parser.h"
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

DeclParser::DeclParser(Parser &parser) : parser_(parser) {}

// `type Name = T`, `type Name<T> = ...`
Stmt *DeclParser::parseTypeAlias() {
  parser_.cursor().advance(); // 'type'
  llvm::SMLoc loc = parser_.tok_.loc();
  llvm::StringRef name = parser_.tok_.text;
  parser_.cursor().advance();
  std::vector<TypeParamDecl> typeParams;
  if (parser_.tok_.is(Tok::Less)) {
    typeParams = parser_.functions().parseTypeParams();
  }
  parser_.cursor().expect(Tok::Assign, "after the name of the type");
  TypeRef type = parser_.types().parseType();
  parser_.cursor().expectSemicolon();
  auto *alias = parser_.ctx_.make<TypeAliasDecl>(loc, name, std::move(type));
  alias->typeParams = std::move(typeParams);

  return alias;
}

// `interface Name { fields }`: the name of an object type, as with `type`.
Stmt *DeclParser::parseInterface() {
  parser_.cursor().advance(); // 'interface'
  llvm::SMLoc loc = parser_.tok_.loc();
  llvm::StringRef name = parser_.tok_.text;
  parser_.cursor().advance();
  std::vector<TypeParamDecl> typeParams;
  if (parser_.tok_.is(Tok::Less)) {
    typeParams = parser_.functions().parseTypeParams();
  }
  std::vector<TypeRef> bases;
  if (parser_.tok_.is(Tok::Identifier) && parser_.tok_.text == "extends") {
    parser_.cursor().advance();
    do {
      bases.push_back(parser_.types().parseType());
    } while (parser_.cursor().accept(Tok::Comma));
  }
  if (!parser_.tok_.is(Tok::LBrace)) {
    parser_.cursor().error(
        parser_.tok_.loc(),
        llvm::Twine("expected '{' to start the fields of the interface, found ") +
            tokSpelling(parser_.tok_.kind));
    return parser_.ctx_.make<EmptyStmt>(loc);
  }
  auto *decl = parser_.ctx_.make<TypeAliasDecl>(loc, name, parser_.types().parseObjectType());
  decl->bases = std::move(bases);
  decl->typeParams = std::move(typeParams);
  parser_.cursor().accept(Tok::Semi);

  return decl;
}

// `enum Name { A, B = value, ... }`
Stmt *DeclParser::parseEnum() {
  parser_.cursor().advance(); // 'enum'
  llvm::SMLoc loc = parser_.tok_.loc();
  if (!parser_.tok_.is(Tok::Identifier)) {
    parser_.cursor().error(parser_.tok_.loc(), "expected the name of the enum");
    return parser_.ctx_.make<EmptyStmt>(loc);
  }
  auto *decl = parser_.ctx_.make<EnumDecl>(loc, parser_.tok_.text);
  parser_.cursor().advance();
  parser_.cursor().expect(Tok::LBrace, "to start the members of the enum");
  while (!parser_.tok_.is(Tok::RBrace) && !parser_.tok_.is(Tok::Eof)) {
    EnumMember member;
    member.loc = parser_.tok_.loc();
    if (!parser_.tok_.is(Tok::Identifier)) {
      parser_.cursor().error(parser_.tok_.loc(),
                             llvm::Twine("expected the name of an enum member, found ") +
                                 tokSpelling(parser_.tok_.kind));
      break;
    }
    member.name = parser_.tok_.text;
    parser_.cursor().advance();
    if (parser_.cursor().accept(Tok::Assign)) {
      member.init = parser_.exprs().parseExpr();
      parser_.vars().recordConstant((decl->name + "." + member.name).str(), member.init);
    }
    decl->members.push_back(member);
    if (!parser_.cursor().accept(Tok::Comma)) {
      break;
    }
  }
  parser_.cursor().expect(Tok::RBrace, "to close the enum");

  return decl;
}

} // namespace tz
