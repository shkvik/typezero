#include "compiler/load/function_type_parser/function_type_parser.h"
#include "compiler/load/class_parser/class_parser.h"
#include "compiler/load/decl_parser/decl_parser.h"
#include "compiler/load/expr_parser/expr_parser.h"
#include "compiler/load/function_parser/function_parser.h"
#include "compiler/load/module_parser/module_parser.h"
#include "compiler/load/pattern_parser/pattern_parser.h"
#include "compiler/load/primary_parser/primary_parser.h"
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

namespace tz {

FunctionTypeParser::FunctionTypeParser(Parser &parser)
    : parser(parser) {}

bool FunctionTypeParser::functionTypeAhead() {
  Parser::Snapshot start = this->parser.cursor().snapshot();
  this->parser.diags.pushList(DiagnosticRoute::DISCARD);
  this->parser.recovering = false;
  this->parser.cursor().advance();
  bool ok = true;
  while (ok && !this->parser.tok.is(Tok::RParen)) {
    this->parser.cursor().accept(Tok::Ellipsis);
    ok = this->parser.cursor().accept(Tok::Identifier);
    this->parser.cursor().accept(Tok::Question);
    if (ok && this->parser.cursor().accept(Tok::Colon)) {
      this->parser.types().parseType();
      ok = !this->parser.recovering;
    }
    if (ok && !this->parser.cursor().accept(Tok::Comma)) {
      break;
    }
  }
  ok = ok && this->parser.cursor().accept(Tok::RParen) && this->parser.tok.is(Tok::Arrow);
  this->parser.diags.popList();
  this->parser.cursor().rewind(std::move(start));

  return ok;
}

TypeRef FunctionTypeParser::parseFunctionType() {
  TypeRef ref;
  if (!parseSignatureParams(ref)) {
    return ref;
  }
  this->parser.cursor().expect(Tok::Arrow, "after the parameters of the function type");
  ref.args.push_back(this->parser.types().parseReturnType());

  return ref;
}

bool FunctionTypeParser::parseSignatureParams(TypeRef &ref) {
  ref.form = FunctionTypeRef{};
  FunctionTypeRef *signature = ref.form.as<FunctionTypeRef>();
  ref.loc = this->parser.tok.loc();
  this->parser.cursor().advance();
  while (!this->parser.tok.is(Tok::RParen) && !this->parser.tok.is(Tok::Eof)) {
    bool rest = this->parser.cursor().accept(Tok::Ellipsis);
    llvm::SMLoc loc = this->parser.tok.loc();
    llvm::StringRef name = this->parser.tok.text;
    if (!this->parser.cursor().expect(Tok::Identifier, "for a parameter of the function type")) {
      return false;
    }
    bool optional = this->parser.cursor().accept(Tok::Question);
    TypeRef param;
    param.loc = loc;
    if (this->parser.cursor().accept(Tok::Colon)) {
      param = this->parser.types().parseType();
    } else {
      this->parser.cursor().report(loc, "parameter '" + name +
                                            "' of the function type needs a type, e.g. '" + name +
                                            ": int'");
    }
    if (signature->rest) {
      this->parser.cursor().report(loc, "a rest parameter must be the last parameter");
    }
    ref.args.push_back(std::move(param));
    signature->optional.push_back(optional);
    signature->names.push_back(name);
    signature->rest |= rest;
    if (!this->parser.cursor().accept(Tok::Comma)) {
      break;
    }
  }
  this->parser.cursor().expect(Tok::RParen, "to close the parameters of the function type");

  return true;
}

}
