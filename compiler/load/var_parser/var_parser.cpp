#include "compiler/load/var_parser/var_parser.h"
#include "compiler/load/class_parser/class_parser.h"
#include "compiler/load/decl_parser/decl_parser.h"
#include "compiler/load/expr_parser/expr_parser.h"
#include "compiler/load/function_parser/function_parser.h"
#include "compiler/load/module_parser/module_parser.h"
#include "compiler/load/pattern_parser/pattern_parser.h"
#include "compiler/load/stmt_parser/stmt_parser.h"
#include "compiler/load/template_parser/template_parser.h"
#include "compiler/load/to_utf8/to_utf8.h"
#include "compiler/load/token_cursor/token_cursor.h"
#include "compiler/load/type_parser/type_parser.h"
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

VarParser::VarParser(Parser &parser) : parser_(parser) {}

// Parses `let`/`const` declarators without the trailing semicolon, so that it
// also serves `for (let i = 0; ...)`.
VarDeclStmt *VarParser::parseVarDecl() {
  auto *decl = parser_.ctx_.make<VarDeclStmt>(parser_.tok_.loc(), parser_.tok_.is(Tok::KwConst));
  parser_.cursor().advance(); // 'let' / 'const' / 'var'
  parseDeclarators(decl);

  return decl;
}

void VarParser::parseDeclarators(VarDeclStmt *decl) {
  do {
    if (parser_.tok_.is(Tok::LBracket) || parser_.tok_.is(Tok::LBrace)) {
      // `const [a, b]: int[] = value`, `const { x, y }: Point = value`
      std::unique_ptr<Parser::Pattern> pattern = parser_.patterns().parseBindingPattern();
      std::optional<TypeRef> typeAnn;
      if (parser_.cursor().accept(Tok::Colon)) {
        typeAnn = parser_.types().parseType();
      }
      if (!parser_.tok_.is(Tok::Assign)) {
        parser_.cursor().error(pattern->loc,
                               pattern->object
                                   ? "a destructuring declaration needs a value: '{ a, b } = ...'"
                                   : "a destructuring declaration needs a value: '[a, b] = ...'");
        return;
      }
      parser_.cursor().advance();
      parser_.patterns().declarePattern(decl, *pattern, parser_.exprs().parseExpr(),
                                        std::move(typeAnn));
      continue;
    }
    VarDeclarator d;
    if (!parseBinding(d)) {
      return;
    }
    if (parser_.cursor().accept(Tok::Assign)) {
      d.init = parser_.exprs().parseExpr();
    }
    // `const Point = class { ... }` names the class, as in JavaScript.
    if (auto *ce = llvm::dyn_cast_or_null<ClassExpr>(d.init); ce && ce->decl->name.empty()) {
      ce->decl->name = d.name;
    }
    if (decl->isConst && d.init) {
      recordConstant(d.name, d.init);
    }
    decl->decls.push_back(d);
  } while (parser_.cursor().accept(Tok::Comma));
}

void VarParser::recordConstant(llvm::StringRef name, const Expr *init) {
  std::vector<std::string> values;
  if (auto *lit = dyn_cast<StringLitExpr>(init)) {
    values.push_back(toUtf8(lit->value));
  } else if (auto *tmpl = dyn_cast<TemplateExpr>(init); tmpl && tmpl->exprs.empty()) {
    values.push_back(toUtf8(tmpl->quasis[0]));
  } else if (auto *other = dyn_cast<NameExpr>(init)) {
    values = parser_.stringConstants_.lookup(other->name); // declared earlier
  }
  std::vector<std::string> &all = parser_.stringConstants_[name];
  all.insert(all.end(), values.begin(), values.end());
}

// A variable name with an optional type annotation.
bool VarParser::parseBinding(VarDeclarator &d) {
  d.loc = parser_.tok_.loc();
  if (!parser_.tok_.is(Tok::Identifier)) {
    parser_.cursor().error(parser_.tok_.loc(), "expected a variable name");
    return false;
  }
  d.name = parser_.tok_.text;
  parser_.cursor().advance();
  if (parser_.cursor().accept(Tok::Colon)) {
    d.typeAnn = parser_.types().parseType();
  }

  return true;
}

} // namespace tz
