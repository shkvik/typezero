#include "compiler/load/module_parser/module_parser.h"
#include "compiler/load/class_parser/class_parser.h"
#include "compiler/load/decl_parser/decl_parser.h"
#include "compiler/load/expr_parser/expr_parser.h"
#include "compiler/load/function_parser/function_parser.h"
#include "compiler/load/json_module/json_module.h"
#include "compiler/load/literal_text/literal_text.h"
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

ModuleParser::ModuleParser(Parser &parser)
    : parser(parser) {}

Stmt *ModuleParser::parseImport(bool topLevel) {
  llvm::SMLoc loc = this->parser.tok.loc();
  this->parser.cursor().advance();
  if (!topLevel) {
    this->parser.cursor().report(loc, "'import' is allowed only at the top level of a file");
  }
  ImportDecl decl;
  decl.loc = loc;
  if (this->parser.tok.is(Tok::StringLiteral)) {
    std::string path;
    parseFromPath(path, decl.pathLoc, false);
    this->parser.cursor().report(
        loc, "an import must name what it takes: importing a file only to run it is not "
             "supported; import the names you need, e.g. 'import { a } from \"" +
                 path + "\"'");
    this->parser.cursor().expectSemicolon();
    return this->parser.ctx.make<EmptyStmt>(loc);
  }
  if (this->parser.tok.is(Tok::Identifier) && this->parser.tok.text == "type" &&
      !this->parser.cursor().peek().is(Tok::Comma) &&
      !(this->parser.cursor().peek().is(Tok::Identifier) &&
        this->parser.cursor().peek().text == "from")) {
    this->parser.cursor().report(
        this->parser.tok.loc(),
        "'import type' is not needed: types are imported like everything else; drop "
        "'type'");
    this->parser.cursor().advance();
  }
  enum class Form { Named, Namespace, Default } form = Form::Named;
  llvm::SMLoc formLoc = this->parser.tok.loc();
  llvm::StringRef defaultName;
  if (this->parser.tok.is(Tok::Star)) {
    form = Form::Namespace;
    this->parser.cursor().advance();
    if (this->parser.cursor().expect(Tok::KwAs, "after 'import *'")) {
      if (this->parser.tok.is(Tok::Identifier)) {
        defaultName = this->parser.tok.text;
      }
      this->parser.cursor().expect(Tok::Identifier, "after 'import * as'");
    }
  } else if (this->parser.tok.is(Tok::Identifier) &&
             !(this->parser.tok.text == "from" &&
               this->parser.cursor().peek().is(Tok::StringLiteral))) {
    form = Form::Default;
    defaultName = this->parser.tok.text;
    this->parser.cursor().advance();
    if (this->parser.tok.is(Tok::Assign)) {
      this->parser.cursor().error(formLoc, "'import " + defaultName +
                                               " = ...' is not supported; import by name, e.g. "
                                               "'import { a } from \"./x\"'");
      return this->parser.ctx.make<EmptyStmt>(loc);
    }
    if (this->parser.cursor().accept(Tok::Comma) && !this->parser.tok.is(Tok::LBrace)) {
      this->parser.cursor().error(this->parser.tok.loc(),
                                  llvm::Twine("expected '{' after ',', found ") +
                                      TokenKinds::tokSpelling(this->parser.tok.kind));
    }
  }
  if (this->parser.tok.is(Tok::LBrace)) {
    parseModuleNames(decl.names, true);
  } else if (form == Form::Named) {
    this->parser.cursor().error(this->parser.tok.loc(),
                                llvm::Twine("expected '{' and the names to import, found ") +
                                    TokenKinds::tokSpelling(this->parser.tok.kind));
  }
  if (this->parser.recovering || !parseFromPath(decl.path, decl.pathLoc)) {
    return this->parser.ctx.make<EmptyStmt>(loc);
  }
  bool json = llvm::StringRef(decl.path).ends_with(".json");
  bool jsonType = false;
  if (this->parser.tok.is(Tok::Identifier) &&
      (this->parser.tok.text == "with" || this->parser.tok.text == "assert") &&
      !this->parser.tok.newlineBefore) {
    jsonType = parseImportAttributes(json);
  }
  this->parser.cursor().expectSemicolon();
  if (json) {
    std::string name = defaultName.empty() ? "data" : defaultName.str();
    std::string example =
        "'import " + name + " from \"" + decl.path + "\" with { type: \"json\" }'";
    if (form != Form::Default) {
      this->parser.cursor().report(formLoc,
                                   "a JSON file is one value: import it whole, e.g. " + example);
      return this->parser.ctx.make<EmptyStmt>(loc);
    }
    if (!jsonType) {
      this->parser.cursor().report(decl.pathLoc, "a JSON file is imported with 'with { type: "
                                                 "\"json\" }', as in node and TypeScript: " +
                                                     example);
    }
    decl.names.push_back({kJsonExport, formLoc, defaultName, formLoc});
    this->parser.imports.push_back(std::move(decl));
    return this->parser.ctx.make<EmptyStmt>(loc);
  }
  if (form != Form::Named && BuiltinModulePaths::isBuiltinModule(decl.path) &&
      !defaultName.empty()) {
    decl.whole = defaultName;
    decl.wholeLoc = formLoc;
    decl.wholeIsDefault = form == Form::Default;
    this->parser.imports.push_back(std::move(decl));
    return this->parser.ctx.make<EmptyStmt>(loc);
  }
  if (form == Form::Namespace) {
    this->parser.cursor().report(
        formLoc, "'import * as' is not supported; list the names you need, e.g. 'import { a, "
                 "b } from \"" +
                     decl.path + "\"'");
  } else if (form == Form::Default) {
    this->parser.cursor().report(
        formLoc, "default imports are not supported: there is no 'export default'; import by "
                 "name, e.g. 'import { " +
                     defaultName + " } from \"" + decl.path + "\"'");
  } else if (decl.names.empty()) {
    this->parser.cursor().report(
        loc, "an import must name what it takes: importing a file only to run it is not "
             "supported; import the names you need, e.g. 'import { a } from \"" +
                 decl.path + "\"'");
  }
  if (!decl.names.empty()) {
    this->parser.imports.push_back(std::move(decl));
  }

  return this->parser.ctx.make<EmptyStmt>(loc);
}

Stmt *ModuleParser::parseExport(bool topLevel) {
  llvm::SMLoc loc = this->parser.tok.loc();
  this->parser.cursor().advance();
  if (!topLevel) {
    this->parser.cursor().report(loc, "'export' is allowed only at the top level");
  }
  if (this->parser.tok.is(Tok::At)) {
    std::vector<Decorator> decorators = this->parser.classes().parseDecorators();
    Stmt *stmt = this->parser.stmts().parseStatement(topLevel);
    if (auto *cls = dyn_cast<ClassDecl>(stmt)) {
      if (topLevel) {
        exportDeclared(cls);
      }
      cls->decorators.insert(cls->decorators.begin(), decorators.begin(), decorators.end());
    } else {
      this->parser.classes().rejectDecorators(decorators);
    }
    return stmt;
  }
  const Token &next = this->parser.cursor().peek();
  bool declaration = this->parser.tok.is(Tok::KwFunction) || this->parser.tok.is(Tok::KwConst) ||
                     this->parser.tok.is(Tok::KwLet) || this->parser.tok.is(Tok::KwVar) ||
                     this->parser.tok.is(Tok::KwEnum) ||
                     this->parser.functions().asyncFunctionAhead() ||
                     (this->parser.tok.is(Tok::Identifier) &&
                      (this->parser.tok.text == "type" || this->parser.tok.text == "interface" ||
                       this->parser.tok.text == "class" ||
                       (this->parser.tok.text == "abstract" && next.text == "class")) &&
                      next.is(Tok::Identifier) && !next.newlineBefore) ||
                     (this->parser.library_ && this->parser.tok.is(Tok::Identifier) &&
                      this->parser.tok.text == "declare" && next.is(Tok::KwFunction));
  if (declaration) {
    Stmt *stmt = this->parser.stmts().parseStatement(topLevel);
    if (topLevel) {
      exportDeclared(stmt);
    }
    return stmt;
  }
  if (this->parser.tok.is(Tok::KwDefault)) {
    this->parser.cursor().report(
        loc, "'export default' is not supported: everything is exported by name; export a "
             "declaration, e.g. 'export function f() {...}', and import it with 'import { f } "
             "from ...'");
    this->parser.cursor().advance();
    if (this->parser.tok.is(Tok::KwFunction) && this->parser.cursor().peek().is(Tok::Identifier)) {
      return this->parser.stmts().parseStatement(topLevel);
    }
    llvm::SMLoc exprLoc = this->parser.tok.loc();
    Expr *expr = this->parser.tok.is(Tok::KwFunction)
                     ? this->parser.ctx.make<FunctionExpr>(
                           exprLoc, this->parser.functions().parseFunction(true))
                     : this->parser.exprs().parseExpr();
    this->parser.cursor().expectSemicolon();
    return this->parser.ctx.make<ExprStmt>(exprLoc, expr);
  }
  if (this->parser.tok.is(Tok::Assign)) {
    this->parser.cursor().report(
        loc, "'export =' is not supported; export by name, e.g. 'export function f() {...}'");
    this->parser.cursor().advance();
    this->parser.exprs().parseExpr();
    this->parser.cursor().expectSemicolon();
    return this->parser.ctx.make<EmptyStmt>(loc);
  }
  ExportDecl decl;
  decl.loc = loc;
  if (this->parser.tok.is(Tok::Star)) {
    this->parser.cursor().advance();
    if (this->parser.tok.is(Tok::KwAs)) {
      llvm::SMLoc asLoc = this->parser.tok.loc();
      this->parser.cursor().advance();
      this->parser.cursor().expect(Tok::Identifier, "after 'export * as'");
      ExportSource source;
      if (!this->parser.recovering && parseFromPath(source.path, source.pathLoc)) {
        this->parser.cursor().report(
            asLoc, "'export * as' is not supported; re-export the names, e.g. 'export { a, b } "
                   "from \"" +
                       source.path + "\"', or all of them with 'export * from \"" + source.path +
                       "\"'");
        this->parser.cursor().expectSemicolon();
      }
      return this->parser.ctx.make<EmptyStmt>(loc);
    }
    ExportSource source;
    if (parseFromPath(source.path, source.pathLoc)) {
      this->parser.cursor().expectSemicolon();
      decl.form = ExportAll{.source = std::move(source)};
      this->parser.exports.push_back(std::move(decl));
    }
    return this->parser.ctx.make<EmptyStmt>(loc);
  }
  if (this->parser.tok.is(Tok::Identifier) && this->parser.tok.text == "type" &&
      next.is(Tok::LBrace)) {
    this->parser.cursor().report(
        this->parser.tok.loc(),
        "'export type' is not needed: types are exported like everything else; drop "
        "'type'");
    this->parser.cursor().advance();
  }
  if (!this->parser.tok.is(Tok::LBrace)) {
    this->parser.cursor().error(
        this->parser.tok.loc(),
        llvm::Twine("expected a declaration or '{' after 'export', found ") +
            TokenKinds::tokSpelling(this->parser.tok.kind));
    return this->parser.ctx.make<EmptyStmt>(loc);
  }
  std::vector<ModuleName> names;
  parseModuleNames(names, false);
  bool reExport = !this->parser.recovering && this->parser.tok.is(Tok::Identifier) &&
                  this->parser.tok.text == "from";
  ExportSource source;
  if (reExport && !parseFromPath(source.path, source.pathLoc)) {
    return this->parser.ctx.make<EmptyStmt>(loc);
  }
  this->parser.cursor().expectSemicolon();
  if (reExport) {
    decl.form = ReExport{.names = std::move(names), .source = std::move(source)};
  } else {
    decl.form = LocalExport{.names = std::move(names)};
  }
  if (!this->parser.recovering) {
    this->parser.exports.push_back(std::move(decl));
  }

  return this->parser.ctx.make<EmptyStmt>(loc);
}

void ModuleParser::parseModuleNames(std::vector<ModuleName> &names, bool import) {
  this->parser.cursor().advance();
  const char *keyword = import ? "import" : "export";
  while (!this->parser.tok.is(Tok::RBrace) && !this->parser.tok.is(Tok::Eof)) {
    if (this->parser.tok.is(Tok::Identifier) && this->parser.tok.text == "type" &&
        this->parser.cursor().peek().is(Tok::Identifier) &&
        this->parser.cursor().peek().text != "as") {
      this->parser.cursor().report(this->parser.tok.loc(),
                                   llvm::Twine("'type' is not needed in an ") + keyword +
                                       ": types are " + keyword +
                                       "ed like everything else; drop it");
      this->parser.cursor().advance();
    }
    ModuleName name;
    name.loc = this->parser.tok.loc();
    if (this->parser.tok.is(Tok::KwDefault)) {
      this->parser.cursor().error(
          this->parser.tok.loc(),
          "there is no 'export default'; everything is exported and imported by name");
      return;
    }
    if (!this->parser.tok.is(Tok::Identifier)) {
      this->parser.cursor().error(this->parser.tok.loc(),
                                  llvm::Twine("expected a name to ") + keyword + ", found " +
                                      TokenKinds::tokSpelling(this->parser.tok.kind));
      return;
    }
    name.name = this->parser.tok.text;
    this->parser.cursor().advance();
    name.as = name.name;
    name.asLoc = name.loc;
    if (this->parser.cursor().accept(Tok::KwAs)) {
      name.asLoc = this->parser.tok.loc();
      if (this->parser.tok.is(Tok::KwDefault)) {
        this->parser.cursor().error(
            this->parser.tok.loc(),
            "there is no 'export default'; everything is exported and imported by name");
        return;
      }
      if (!this->parser.tok.is(Tok::Identifier)) {
        this->parser.cursor().error(this->parser.tok.loc(),
                                    llvm::Twine("expected a name after 'as', found ") +
                                        TokenKinds::tokSpelling(this->parser.tok.kind));
        return;
      }
      name.as = this->parser.tok.text;
      this->parser.cursor().advance();
    }
    names.push_back(name);
    if (!this->parser.cursor().accept(Tok::Comma)) {
      break;
    }
  }
  this->parser.cursor().expect(Tok::RBrace, import ? "to close the names to import"
                                                   : "to close the names to export");
}

bool ModuleParser::parseImportAttributes(bool json) {
  llvm::SMLoc at = this->parser.tok.loc();
  bool old = this->parser.tok.text == "assert";
  this->parser.cursor().advance();
  if (!this->parser.cursor().expect(Tok::LBrace, "after 'with'")) {
    return false;
  }
  bool typeJson = false;
  bool other = false;
  while (!this->parser.tok.is(Tok::RBrace) && !this->parser.tok.is(Tok::Eof)) {
    std::string key;
    llvm::SMLoc keyLoc;
    if (this->parser.tok.is(Tok::Identifier)) {
      key = this->parser.tok.text.str();
      this->parser.cursor().advance();
    } else if (!parseFromPath(key, keyLoc, false)) {
      this->parser.cursor().skipToClosingBrace();
      return false;
    }
    std::string value;
    llvm::SMLoc valueLoc;
    if (!this->parser.cursor().expect(Tok::Colon, "after the name of an import attribute") ||
        !parseFromPath(value, valueLoc, false)) {
      this->parser.cursor().skipToClosingBrace();
      return false;
    }
    if (key == "type" && value == "json") {
      typeJson = true;
    } else {
      other = true;
    }
    if (!this->parser.cursor().accept(Tok::Comma)) {
      break;
    }
  }
  this->parser.cursor().expect(Tok::RBrace, "after the import attributes");
  if (!json) {
    this->parser.cursor().report(at,
                                 "import attributes are only for JSON files, e.g. 'import config "
                                 "from \"./config.json\" with { type: \"json\" }'");
    return false;
  }
  if (old) {
    this->parser.cursor().report(at, "'assert' is the old form of import attributes; write 'with { "
                                     "type: \"json\" }'");
  } else if (other) {
    this->parser.cursor().report(at,
                                 "a JSON file takes one import attribute: 'with { type: \"json\" "
                                 "}'");
  }

  return typeJson || old || other;
}

bool ModuleParser::parseFromPath(std::string &path, llvm::SMLoc &loc, bool from) {
  if (from && !(this->parser.tok.is(Tok::Identifier) && this->parser.tok.text == "from")) {
    this->parser.cursor().error(this->parser.tok.loc(),
                                llvm::Twine("expected 'from' and the path of the file, e.g. 'from "
                                            "\"./math\"', found ") +
                                    TokenKinds::tokSpelling(this->parser.tok.kind));
    return false;
  }
  if (from) {
    this->parser.cursor().advance();
  }
  loc = this->parser.tok.loc();
  if (!this->parser.tok.is(Tok::StringLiteral)) {
    this->parser.cursor().error(
        this->parser.tok.loc(),
        llvm::Twine("expected the path of the file as a string, e.g. '\"./math\"', found ") +
            TokenKinds::tokSpelling(this->parser.tok.kind));
    return false;
  }
  std::u16string text;
  LiteralText::decodeStringLiteral(this->parser.tok, text, this->parser.diags);
  path = ToUtf8::toUtf8(text);
  this->parser.cursor().advance();

  return true;
}

void ModuleParser::exportDeclared(const Stmt *stmt) {
  std::vector<ModuleName> names;
  auto add = [&](llvm::StringRef name, llvm::SMLoc loc) {
    names.push_back({name, loc, name, loc});
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
  if (names.empty()) {
    return;
  }
  ExportDecl decl;
  decl.loc = stmt->loc();
  decl.form = LocalExport{.names = std::move(names)};
  this->parser.exports.push_back(std::move(decl));
}

}
