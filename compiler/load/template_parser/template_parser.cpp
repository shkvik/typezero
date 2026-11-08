#include "compiler/load/template_parser/template_parser.h"
#include "compiler/load/class_parser/class_parser.h"
#include "compiler/load/decl_parser/decl_parser.h"
#include "compiler/load/expr_parser/expr_parser.h"
#include "compiler/load/function_parser/function_parser.h"
#include "compiler/load/literal_text/literal_text.h"
#include "compiler/load/module_parser/module_parser.h"
#include "compiler/load/pattern_parser/pattern_parser.h"
#include "compiler/load/stmt_parser/stmt_parser.h"
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

TemplateParser::TemplateParser(Parser &parser)
    : parser(parser) {}

TemplateExpr *TemplateParser::parseTemplate(bool raw) {
  auto *tmpl = this->parser.ctx.make<TemplateExpr>(this->parser.tok.loc());
  std::u16string text;
  auto decode = [&] {
    if (raw) {
      LiteralText::decodeTemplateRaw(this->parser.tok, text);
    } else {
      LiteralText::decodeTemplatePart(this->parser.tok, text, this->parser.diags);
    }
  };
  decode();
  tmpl->quasis.push_back(text);
  bool done = this->parser.tok.is(Tok::TemplateString);
  this->parser.cursor().advance();
  while (!done) {
    tmpl->exprs.push_back(this->parser.exprs().parseSequence());
    if (!this->parser.tok.is(Tok::TemplateMiddle) && !this->parser.tok.is(Tok::TemplateTail)) {
      this->parser.cursor().error(this->parser.tok.loc(),
                                  "expected '}' to close the template substitution");
      tmpl->quasis.emplace_back();
      return tmpl;
    }
    decode();
    tmpl->quasis.push_back(text);
    done = this->parser.tok.is(Tok::TemplateTail);
    this->parser.cursor().advance();
  }

  return tmpl;
}

}
