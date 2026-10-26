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

namespace tz {

using llvm::cast;

using llvm::dyn_cast;

using llvm::isa;

const char *opSpelling(UnaryOp op) {
  switch (op) {
  case UnaryOp::Neg:
    return "-";
  case UnaryOp::Plus:
    return "+";
  case UnaryOp::Not:
    return "!";
  case UnaryOp::BitNot:
    return "~";
  case UnaryOp::TypeOf:
    return "typeof";
  case UnaryOp::Void:
    return "void";
  case UnaryOp::Delete:
    return "delete";
  case UnaryOp::NonNull:
    return "!";
  }

  return "?";
}

const char *opSpelling(BinaryOp op) {
  switch (op) {
  case BinaryOp::Add:
    return "+";
  case BinaryOp::Sub:
    return "-";
  case BinaryOp::Mul:
    return "*";
  case BinaryOp::Div:
    return "/";
  case BinaryOp::Rem:
    return "%";
  case BinaryOp::Pow:
    return "**";
  case BinaryOp::BitAnd:
    return "&";
  case BinaryOp::BitOr:
    return "|";
  case BinaryOp::BitXor:
    return "^";
  case BinaryOp::Shl:
    return "<<";
  case BinaryOp::Shr:
    return ">>";
  case BinaryOp::UShr:
    return ">>>";
  case BinaryOp::Lt:
    return "<";
  case BinaryOp::Le:
    return "<=";
  case BinaryOp::Gt:
    return ">";
  case BinaryOp::Ge:
    return ">=";
  case BinaryOp::Eq:
    return "==";
  case BinaryOp::Ne:
    return "!=";
  case BinaryOp::LogAnd:
    return "&&";
  case BinaryOp::LogOr:
    return "||";
  case BinaryOp::Nullish:
    return "??";
  case BinaryOp::In:
    return "in";
  case BinaryOp::InstanceOf:
    return "instanceof";
  }

  return "?";
}

Parser::Parser(Lexer &lexer, AstContext &ctx, Diagnostics &diags, bool library)
    : lexer_(lexer), ctx_(ctx), diags_(diags), library_(library) {
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
  while (!tok_.is(Tok::Eof)) {
    if (tok_.is(Tok::RBrace)) {
      cursor().error(tok_.loc(), "unexpected '}'");
      cursor().advance();
      recovering_ = false;
      continue;
    }
    file.items.push_back(stmts().parseStatement(true));
    if (recovering_) {
      cursor().synchronize();
    }
  }
  file.fieldNames = std::move(fieldNames_);
  file.imports = std::move(imports_);
  file.exports = std::move(exports_);
  file.stringConstants = std::move(stringConstants_);

  return file;
}

ClassDecl *Parser::parseClassCopy() { return classes().parseClass(); }

FunctionDecl *Parser::parseMethodCopy() {
  const char *start = tok_.text.data();
  bool generator = cursor().accept(Tok::Star);
  llvm::SMLoc loc = tok_.loc();
  llvm::StringRef name = tok_.text;
  bool constructor = tok_.is(Tok::Identifier) && name == "constructor";
  cursor().advance();
  auto *fn = ctx_.make<FunctionDecl>(loc, name);
  fn->isConstructor = constructor;
  fn->isGenerator = generator;
  classes().parseMethodRest(fn, start);

  return fn;
}

FunctionDecl *Parser::parseFunctionExprCopy() {
  if (tok_.is(Tok::KwFunction)) {
    return functions().parseFunction(true);
  }
  if (tok_.is(Tok::Less)) {
    const char *start = tok_.text.data();
    std::vector<TypeParamDecl> typeParams = functions().parseTypeParams();
    return functions().parseArrow(std::move(typeParams), start)->fn;
  }

  return functions().parseArrow()->fn;
}

bool Parser::Pattern::declares(llvm::StringRef name) const {
  for (const Element &e : elements) {
    if (e.name == name || (e.nested && e.nested->declares(name))) {
      return true;
    }
  }

  return false;
}

FunctionDecl *Parser::parseFunctionCopy() { return functions().parseFunction(false); }

Parser::~Parser() = default;
}
