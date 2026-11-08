#include "compiler/load/var_parser/var_parser.h"
#include "compiler/load/class_parser/class_parser.h"
#include "compiler/load/decl_parser/decl_parser.h"
#include "compiler/load/expr_parser/expr_parser.h"
#include "compiler/load/function_parser/function_parser.h"
#include "compiler/load/module_parser/module_parser.h"
#include "compiler/load/pattern_parser/pattern_parser.h"
#include "compiler/load/stmt_parser/stmt_parser.h"
#include "compiler/load/template_parser/template_parser.h"
#include "compiler/load/token_cursor/token_cursor.h"
#include "compiler/load/type_parser/type_parser.h"
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

VarParser::VarParser(Parser &parser)
    : parser(parser) {}

VarDeclStmt *VarParser::parseVarDecl() {
  auto *decl =
      this->parser.ctx.make<VarDeclStmt>(this->parser.tok.loc(), this->parser.tok.is(Tok::KwConst));
  this->parser.cursor().advance();
  parseDeclarators(decl);

  return decl;
}

void VarParser::parseDeclarators(VarDeclStmt *decl) {
  do {
    if (this->parser.tok.is(Tok::LBracket) || this->parser.tok.is(Tok::LBrace)) {
      std::unique_ptr<Parser::Pattern> pattern = this->parser.patterns().parseBindingPattern();
      std::optional<TypeRef> typeAnn;
      if (this->parser.cursor().accept(Tok::Colon)) {
        typeAnn = this->parser.types().parseType();
      }
      if (!this->parser.tok.is(Tok::Assign)) {
        this->parser.cursor().error(
            pattern->loc, pattern->object
                              ? "a destructuring declaration needs a value: '{ a, b } = ...'"
                              : "a destructuring declaration needs a value: '[a, b] = ...'");
        return;
      }
      this->parser.cursor().advance();
      this->parser.patterns().declarePattern(decl, *pattern, this->parser.exprs().parseExpr(),
                                             std::move(typeAnn));
      continue;
    }
    VarDeclarator d;
    if (!parseBinding(d)) {
      return;
    }
    if (this->parser.cursor().accept(Tok::Assign)) {
      d.init = this->parser.exprs().parseExpr();
    }
    if (auto *ce = llvm::dyn_cast_or_null<ClassExpr>(d.init); ce && ce->decl->name.empty()) {
      ce->decl->name = d.name;
    }
    if (decl->isConst && d.init) {
      recordConstant(d.name, d.init);
    }
    decl->decls.push_back(d);
  } while (this->parser.cursor().accept(Tok::Comma));
}

void VarParser::recordConstant(llvm::StringRef name, const Expr *init) {
  std::vector<std::string> values;
  if (auto *lit = dyn_cast<StringLitExpr>(init)) {
    values.push_back(ToUtf8::toUtf8(lit->value));
  } else if (auto *tmpl = dyn_cast<TemplateExpr>(init); tmpl && tmpl->exprs.empty()) {
    values.push_back(ToUtf8::toUtf8(tmpl->quasis[0]));
  } else if (auto *other = dyn_cast<NameExpr>(init)) {
    values = this->parser.stringConstants.lookup(other->name);
  }
  std::vector<std::string> &all = this->parser.stringConstants[name];
  all.insert(all.end(), values.begin(), values.end());
}

bool VarParser::parseBinding(VarDeclarator &d) {
  d.loc = this->parser.tok.loc();
  if (!this->parser.tok.is(Tok::Identifier)) {
    this->parser.cursor().error(this->parser.tok.loc(), "expected a variable name");
    return false;
  }
  d.name = this->parser.tok.text;
  this->parser.cursor().advance();
  if (this->parser.cursor().accept(Tok::Colon)) {
    d.typeAnn = this->parser.types().parseType();
  }

  return true;
}

}
