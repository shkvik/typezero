#pragma once

#include "compiler/cli/syntax/ast_dump/dump_text/dump_text.h"
#include "compiler/program/ast/ast.h"
#include "compiler/program/source_file/source_file.h"
#include "llvm/Support/raw_ostream.h"
#include <optional>
#include <string>
#include <vector>

namespace tz {

class AstDump {
public:
  AstDump(llvm::raw_ostream &target, const char *begin);

  void dumpFile(const SourceFile &file);

private:
  llvm::raw_ostream &out;
  DumpText text;
  int depth = 0;

  llvm::raw_ostream &open();
  void missing();
  void dumpModules(const SourceFile &file);
  void dumpModuleNames(llvm::ArrayRef<ModuleName> names);
  void dumpTables(const SourceFile &file);
  void dumpStmt(const Stmt *stmt);
  void dumpOptionalStmt(const Stmt *stmt);
  void dumpBlock(const BlockStmt *block);
  void dumpVarDecl(const VarDeclStmt *decl);
  void dumpDeclarator(const VarDeclarator &d);
  void dumpFor(const ForStmt *loop);
  void dumpSwitch(const SwitchStmt *stmt);
  void dumpTypeAlias(const TypeAliasDecl *alias);
  void dumpEnum(const EnumDecl *decl);
  void dumpFunction(const FunctionDecl *fn);
  void dumpParam(const Param &param);
  void dumpTypeParams(const std::vector<TypeParamDecl> &params);
  void dumpDecorators(const std::vector<Decorator> &decorators);
  void dumpClass(const ClassDecl *cls);
  void dumpField(const ClassField &field);
  void dumpExpr(const Expr *expr);
  void dumpOptionalExpr(const Expr *expr);
  void dumpLiteral(const Expr *expr);
  void dumpTemplate(const TemplateExpr *tmpl);
  void dumpArray(const ArrayLitExpr *lit);
  void dumpObject(const ObjectLitExpr *lit);
  std::string propertyForm(const ObjectLitExpr::Property &prop) const;
  void dumpAccess(const Expr *expr);
  void dumpOperation(const Expr *expr);
  void dumpCall(const CallExpr *call);
  void dumpNew(const NewExpr *create);
  void dumpChildren(llvm::ArrayRef<Expr *> exprs);
  void dumpType(const TypeRef &ref);
  void dumpOptionalType(const std::optional<TypeRef> &ref);
  void dumpTypes(const std::vector<TypeRef> &refs);
};

}
