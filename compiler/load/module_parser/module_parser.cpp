#include "compiler/load/module_parser/module_parser.h"
#include "compiler/load/class_parser/class_parser.h"
#include "compiler/load/decl_parser/decl_parser.h"
#include "compiler/load/expr_parser/expr_parser.h"
#include "compiler/load/function_parser/function_parser.h"
#include "compiler/load/literal_text/literal_text.h"
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

ModuleParser::ModuleParser(Parser &parser) : parser_(parser) {}

Stmt *ModuleParser::parseImport(bool topLevel) {
  llvm::SMLoc loc = parser_.tok_.loc();
  parser_.cursor().advance();
  if (!topLevel) {
    parser_.cursor().report(loc, "'import' is allowed only at the top level of a file");
  }
  ImportDecl decl;
  decl.loc = loc;
  if (parser_.tok_.is(Tok::StringLiteral)) {
    std::string path;
    parseFromPath(path, decl.pathLoc, false);
    parser_.cursor().report(
        loc, "an import must name what it takes: importing a file only to run it is not "
             "supported; import the names you need, e.g. 'import { a } from \"" +
                 path + "\"'");
    parser_.cursor().expectSemicolon();
    return parser_.ctx_.make<EmptyStmt>(loc);
  }
  if (parser_.tok_.is(Tok::Identifier) && parser_.tok_.text == "type" &&
      !parser_.cursor().peek().is(Tok::Comma) &&
      !(parser_.cursor().peek().is(Tok::Identifier) && parser_.cursor().peek().text == "from")) {
    parser_.cursor().report(
        parser_.tok_.loc(),
        "'import type' is not needed: types are imported like everything else; drop "
        "'type'");
    parser_.cursor().advance();
  }
  enum class Form { Named, Namespace, Default } form = Form::Named;
  llvm::SMLoc formLoc = parser_.tok_.loc();
  llvm::StringRef defaultName;
  if (parser_.tok_.is(Tok::Star)) {
    form = Form::Namespace;
    parser_.cursor().advance();
    if (parser_.cursor().expect(Tok::KwAs, "after 'import *'")) {
      if (parser_.tok_.is(Tok::Identifier)) {
        defaultName = parser_.tok_.text;
      }
      parser_.cursor().expect(Tok::Identifier, "after 'import * as'");
    }
  } else if (parser_.tok_.is(Tok::Identifier) &&
             !(parser_.tok_.text == "from" && parser_.cursor().peek().is(Tok::StringLiteral))) {
    form = Form::Default;
    defaultName = parser_.tok_.text;
    parser_.cursor().advance();
    if (parser_.tok_.is(Tok::Assign)) {
      parser_.cursor().error(formLoc, "'import " + defaultName +
                                          " = ...' is not supported; import by name, e.g. "
                                          "'import { a } from \"./x\"'");
      return parser_.ctx_.make<EmptyStmt>(loc);
    }
    if (parser_.cursor().accept(Tok::Comma) && !parser_.tok_.is(Tok::LBrace)) {
      parser_.cursor().error(parser_.tok_.loc(), llvm::Twine("expected '{' after ',', found ") +
                                                     tokSpelling(parser_.tok_.kind));
    }
  }
  if (parser_.tok_.is(Tok::LBrace)) {
    parseModuleNames(decl.names, true);
  } else if (form == Form::Named) {
    parser_.cursor().error(parser_.tok_.loc(),
                           llvm::Twine("expected '{' and the names to import, found ") +
                               tokSpelling(parser_.tok_.kind));
  }
  if (parser_.recovering_ || !parseFromPath(decl.path, decl.pathLoc)) {
    return parser_.ctx_.make<EmptyStmt>(loc);
  }
  if (parser_.tok_.is(Tok::Identifier) &&
      (parser_.tok_.text == "with" || parser_.tok_.text == "assert") &&
      !parser_.tok_.newlineBefore) {
    parser_.cursor().report(parser_.tok_.loc(),
                            "import attributes are not supported; data files will be embedded with "
                            "embedJson and embedText");
    parser_.cursor().advance();
    if (parser_.tok_.is(Tok::LBrace)) {
      parser_.cursor().advance();
      parser_.cursor().skipToClosingBrace();
    }
  }
  parser_.cursor().expectSemicolon();
  if (form != Form::Named && isBuiltinModule(decl.path) && !defaultName.empty()) {
    decl.whole = defaultName;
    decl.wholeLoc = formLoc;
    decl.wholeIsDefault = form == Form::Default;
    parser_.imports_.push_back(std::move(decl));
    return parser_.ctx_.make<EmptyStmt>(loc);
  }
  if (form == Form::Namespace) {
    parser_.cursor().report(
        formLoc, "'import * as' is not supported; list the names you need, e.g. 'import { a, "
                 "b } from \"" +
                     decl.path + "\"'");
  } else if (form == Form::Default) {
    parser_.cursor().report(
        formLoc, "default imports are not supported: there is no 'export default'; import by "
                 "name, e.g. 'import { " +
                     defaultName + " } from \"" + decl.path + "\"'");
  } else if (decl.names.empty()) {
    parser_.cursor().report(
        loc, "an import must name what it takes: importing a file only to run it is not "
             "supported; import the names you need, e.g. 'import { a } from \"" +
                 decl.path + "\"'");
  }
  if (!decl.names.empty()) {
    parser_.imports_.push_back(std::move(decl));
  }

  return parser_.ctx_.make<EmptyStmt>(loc);
}

Stmt *ModuleParser::parseExport(bool topLevel) {
  llvm::SMLoc loc = parser_.tok_.loc();
  parser_.cursor().advance();
  if (!topLevel) {
    parser_.cursor().report(loc, "'export' is allowed only at the top level");
  }
  if (parser_.tok_.is(Tok::At)) {
    std::vector<Decorator> decorators = parser_.classes().parseDecorators();
    Stmt *stmt = parser_.stmts().parseStatement(topLevel);
    if (auto *cls = dyn_cast<ClassDecl>(stmt)) {
      if (topLevel) {
        exportDeclared(cls);
      }
      cls->decorators.insert(cls->decorators.begin(), decorators.begin(), decorators.end());
    } else {
      parser_.classes().rejectDecorators(decorators);
    }
    return stmt;
  }
  const Token &next = parser_.cursor().peek();
  bool declaration = parser_.tok_.is(Tok::KwFunction) || parser_.tok_.is(Tok::KwConst) ||
                     parser_.tok_.is(Tok::KwLet) || parser_.tok_.is(Tok::KwVar) ||
                     parser_.tok_.is(Tok::KwEnum) || parser_.functions().asyncFunctionAhead() ||
                     (parser_.tok_.is(Tok::Identifier) &&
                      (parser_.tok_.text == "type" || parser_.tok_.text == "interface" ||
                       parser_.tok_.text == "class" ||
                       (parser_.tok_.text == "abstract" && next.text == "class")) &&
                      next.is(Tok::Identifier) && !next.newlineBefore) ||
                     (parser_.library_ && parser_.tok_.is(Tok::Identifier) &&
                      parser_.tok_.text == "declare" && next.is(Tok::KwFunction));
  if (declaration) {
    Stmt *stmt = parser_.stmts().parseStatement(topLevel);
    if (topLevel) {
      exportDeclared(stmt);
    }
    return stmt;
  }
  if (parser_.tok_.is(Tok::KwDefault)) {
    parser_.cursor().report(
        loc, "'export default' is not supported: everything is exported by name; export a "
             "declaration, e.g. 'export function f() {...}', and import it with 'import { f } "
             "from ...'");
    parser_.cursor().advance();
    if (parser_.tok_.is(Tok::KwFunction) && parser_.cursor().peek().is(Tok::Identifier)) {
      return parser_.stmts().parseStatement(topLevel);
    }
    llvm::SMLoc exprLoc = parser_.tok_.loc();
    Expr *expr =
        parser_.tok_.is(Tok::KwFunction)
            ? parser_.ctx_.make<FunctionExpr>(exprLoc, parser_.functions().parseFunction(true))
            : parser_.exprs().parseExpr();
    parser_.cursor().expectSemicolon();
    return parser_.ctx_.make<ExprStmt>(exprLoc, expr);
  }
  if (parser_.tok_.is(Tok::Assign)) {
    parser_.cursor().report(
        loc, "'export =' is not supported; export by name, e.g. 'export function f() {...}'");
    parser_.cursor().advance();
    parser_.exprs().parseExpr();
    parser_.cursor().expectSemicolon();
    return parser_.ctx_.make<EmptyStmt>(loc);
  }
  ExportDecl decl;
  decl.loc = loc;
  if (parser_.tok_.is(Tok::Star)) {
    parser_.cursor().advance();
    if (parser_.tok_.is(Tok::KwAs)) {
      llvm::SMLoc asLoc = parser_.tok_.loc();
      parser_.cursor().advance();
      parser_.cursor().expect(Tok::Identifier, "after 'export * as'");
      if (!parser_.recovering_ && parseFromPath(decl.path, decl.pathLoc)) {
        parser_.cursor().report(
            asLoc, "'export * as' is not supported; re-export the names, e.g. 'export { a, b } "
                   "from \"" +
                       decl.path + "\"', or all of them with 'export * from \"" + decl.path +
                       "\"'");
        parser_.cursor().expectSemicolon();
      }
      return parser_.ctx_.make<EmptyStmt>(loc);
    }
    decl.all = true;
    if (parseFromPath(decl.path, decl.pathLoc)) {
      parser_.cursor().expectSemicolon();
      parser_.exports_.push_back(std::move(decl));
    }
    return parser_.ctx_.make<EmptyStmt>(loc);
  }
  if (parser_.tok_.is(Tok::Identifier) && parser_.tok_.text == "type" && next.is(Tok::LBrace)) {
    parser_.cursor().report(
        parser_.tok_.loc(),
        "'export type' is not needed: types are exported like everything else; drop "
        "'type'");
    parser_.cursor().advance();
  }
  if (!parser_.tok_.is(Tok::LBrace)) {
    parser_.cursor().error(parser_.tok_.loc(),
                           llvm::Twine("expected a declaration or '{' after 'export', found ") +
                               tokSpelling(parser_.tok_.kind));
    return parser_.ctx_.make<EmptyStmt>(loc);
  }
  parseModuleNames(decl.names, false);
  if (!parser_.recovering_ && parser_.tok_.is(Tok::Identifier) && parser_.tok_.text == "from" &&
      !parseFromPath(decl.path, decl.pathLoc)) {
    return parser_.ctx_.make<EmptyStmt>(loc);
  }
  parser_.cursor().expectSemicolon();
  if (!parser_.recovering_) {
    parser_.exports_.push_back(std::move(decl));
  }

  return parser_.ctx_.make<EmptyStmt>(loc);
}

void ModuleParser::parseModuleNames(std::vector<ModuleName> &names, bool import) {
  parser_.cursor().advance();
  const char *keyword = import ? "import" : "export";
  while (!parser_.tok_.is(Tok::RBrace) && !parser_.tok_.is(Tok::Eof)) {
    if (parser_.tok_.is(Tok::Identifier) && parser_.tok_.text == "type" &&
        parser_.cursor().peek().is(Tok::Identifier) && parser_.cursor().peek().text != "as") {
      parser_.cursor().report(parser_.tok_.loc(), llvm::Twine("'type' is not needed in an ") +
                                                      keyword + ": types are " + keyword +
                                                      "ed like everything else; drop it");
      parser_.cursor().advance();
    }
    ModuleName name;
    name.loc = parser_.tok_.loc();
    if (parser_.tok_.is(Tok::KwDefault)) {
      parser_.cursor().error(
          parser_.tok_.loc(),
          "there is no 'export default'; everything is exported and imported by name");
      return;
    }
    if (!parser_.tok_.is(Tok::Identifier)) {
      parser_.cursor().error(parser_.tok_.loc(), llvm::Twine("expected a name to ") + keyword +
                                                     ", found " + tokSpelling(parser_.tok_.kind));
      return;
    }
    name.name = parser_.tok_.text;
    parser_.cursor().advance();
    name.as = name.name;
    name.asLoc = name.loc;
    if (parser_.cursor().accept(Tok::KwAs)) {
      name.asLoc = parser_.tok_.loc();
      if (parser_.tok_.is(Tok::KwDefault)) {
        parser_.cursor().error(
            parser_.tok_.loc(),
            "there is no 'export default'; everything is exported and imported by name");
        return;
      }
      if (!parser_.tok_.is(Tok::Identifier)) {
        parser_.cursor().error(parser_.tok_.loc(),
                               llvm::Twine("expected a name after 'as', found ") +
                                   tokSpelling(parser_.tok_.kind));
        return;
      }
      name.as = parser_.tok_.text;
      parser_.cursor().advance();
    }
    names.push_back(name);
    if (!parser_.cursor().accept(Tok::Comma)) {
      break;
    }
  }
  parser_.cursor().expect(Tok::RBrace,
                          import ? "to close the names to import" : "to close the names to export");
}

bool ModuleParser::parseFromPath(std::string &path, llvm::SMLoc &loc, bool from) {
  if (from && !(parser_.tok_.is(Tok::Identifier) && parser_.tok_.text == "from")) {
    parser_.cursor().error(parser_.tok_.loc(),
                           llvm::Twine("expected 'from' and the path of the file, e.g. 'from "
                                       "\"./math\"', found ") +
                               tokSpelling(parser_.tok_.kind));
    return false;
  }
  if (from) {
    parser_.cursor().advance();
  }
  loc = parser_.tok_.loc();
  if (!parser_.tok_.is(Tok::StringLiteral)) {
    parser_.cursor().error(
        parser_.tok_.loc(),
        llvm::Twine("expected the path of the file as a string, e.g. '\"./math\"', found ") +
            tokSpelling(parser_.tok_.kind));
    return false;
  }
  std::u16string text;
  decodeStringLiteral(parser_.tok_, text, parser_.diags_);
  llvm::convertUTF16ToUTF8String(
      llvm::ArrayRef<llvm::UTF16>(reinterpret_cast<const llvm::UTF16 *>(text.data()), text.size()),
      path);
  parser_.cursor().advance();

  return true;
}

void ModuleParser::exportDeclared(const Stmt *stmt) {
  ExportDecl decl;
  decl.loc = stmt->loc();
  auto add = [&](llvm::StringRef name, llvm::SMLoc loc) {
    decl.names.push_back({name, loc, name, loc});
  };
  if (auto *fn = dyn_cast<FunctionDecl>(stmt)) {
    add(fn->name, fn->loc());
  } else if (auto *alias = dyn_cast<TypeAliasDecl>(stmt)) {
    add(alias->name, alias->loc());
  } else if (auto *e = dyn_cast<EnumDecl>(stmt)) {
    add(e->name, e->loc());
  } else if (auto *cls = dyn_cast<ClassDecl>(stmt)) {
    add(cls->name, cls->loc());
  } else if (auto *var = dyn_cast<VarDeclStmt>(stmt)) {
    for (const VarDeclarator &d : var->decls) {
      if (!d.name.starts_with("%")) {
        add(d.name, d.loc);
      }
    }
  }
  if (!decl.names.empty()) {
    parser_.exports_.push_back(std::move(decl));
  }
}

}
