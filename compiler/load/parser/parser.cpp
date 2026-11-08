#include "compiler/load/parser/parser.h"
#include "compiler/load/call_parser/call_parser.h"
#include "compiler/load/class_parser/class_parser.h"
#include "compiler/load/decl_parser/decl_parser.h"
#include "compiler/load/expr_parser/expr_parser.h"
#include "compiler/load/function_parser/function_parser.h"
#include "compiler/load/function_type_parser/function_type_parser.h"
#include "compiler/load/module_parser/module_parser.h"
#include "compiler/load/object_type_parser/object_type_parser.h"
#include "compiler/load/pattern_parser/pattern_parser.h"
#include "compiler/load/primary_parser/primary_parser.h"
#include "compiler/load/stmt_parser/stmt_parser.h"
#include "compiler/load/template_parser/template_parser.h"
#include "compiler/load/token_cursor/token_cursor.h"
#include "compiler/load/type_parser/type_parser.h"
#include "compiler/load/var_parser/var_parser.h"
#include "compiler/program/ast/ast_cloner/ast_cloner.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/SaveAndRestore.h"

namespace tz {

using llvm::cast;

using llvm::dyn_cast;

using llvm::isa;

Parser::Parser(Lexer &lexer, AstContext &ctx, Diagnostics &diags, bool library)
    : lexer(lexer),
      ctx(ctx),
      diags(diags),
      library_(library) {
  cursor_ = std::make_unique<TokenCursor>(*this);
  stmts_ = std::make_unique<StmtParser>(*this);
  vars_ = std::make_unique<VarParser>(*this);
  patterns_ = std::make_unique<PatternParser>(*this);
  exprs_ = std::make_unique<ExprParser>(*this);
  calls_ = std::make_unique<CallParser>(*this);
  primaries_ = std::make_unique<PrimaryParser>(*this);
  templates_ = std::make_unique<TemplateParser>(*this);
  functions_ = std::make_unique<FunctionParser>(*this);
  classes_ = std::make_unique<ClassParser>(*this);
  decls_ = std::make_unique<DeclParser>(*this);
  modules_ = std::make_unique<ModuleParser>(*this);
  types_ = std::make_unique<TypeParser>(*this);
  objectTypes_ = std::make_unique<ObjectTypeParser>(*this);
  functionTypes_ = std::make_unique<FunctionTypeParser>(*this);
  cursor().advance();
}

SourceFile Parser::parseSourceFile() {
  SourceFile file;
  while (!this->tok.is(Tok::Eof)) {
    if (this->tok.is(Tok::RBrace)) {
      cursor().error(this->tok.loc(), "unexpected '}'");
      cursor().advance();
      this->recovering = false;
      continue;
    }
    file.items.push_back(stmts().parseStatement(true));
    if (this->recovering) {
      cursor().synchronize();
    }
  }
  file.fieldNames = std::move(this->fieldNames);
  file.imports = std::move(this->imports);
  file.exports = std::move(this->exports);
  file.stringConstants = std::move(this->stringConstants);
  AstCloner cloner(this->ctx);
  cloner.keepPristine(file.items);

  return file;
}

bool Parser::Pattern::declares(llvm::StringRef name) const {
  for (const Element &e : elements) {
    if (e.name == name || (e.nested && e.nested->declares(name))) {
      return true;
    }
  }

  return false;
}


Parser::~Parser() = default;
}
