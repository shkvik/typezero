#pragma once

#include "compiler/program/library/builtin_modules/known_modules/known_modules.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/library/builtin_modules/builtin_module_checker/builtin_module_checker_errors.h"
#include "compiler/semantic/library/builtin_modules/module_export_errors/module_export_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class Scopes;
class FsChecker;
class OsChecker;
class ExprChecker;
class PathChecker;
class ChildChecker;
class TimerChecker;
class CryptoChecker;
class ReadlineChecker;
class FsPromisesChecker;
class BuiltinModuleTypes;
class CryptoFunctionChecker;

class BuiltinModuleChecker {
public:
  using ResultExportFound = Result<NodeModule, BuiltinModuleCheckerExportFoundError>;
  using ResultModuleExport = Result<llvm::StringRef, ModuleExportFoundError>;

  BuiltinModuleChecker(SemanticState &owner, SemanticParts &all);

  void importBuiltinModule(Module *module, const ImportDecl &importDecl);
  const NodeName *nodeName(llvm::StringRef name) const;
  Expr *checkNodeCall(CallExpr *call, const Type *expected = nullptr);
  Expr *checkNodeName(NameExpr *name);
  Expr *checkNodeMember(MemberExpr *member);
  Expr *processAlias(Expr *expr);
  Expr *nodeValue(const NodeName &nodeName, Expr *expr);
  Expr *checkTimersPromises(CallExpr *call, llvm::StringRef functionName, llvm::SMLoc loc,
                            const Type *expected);

private:
  MemberExpr *processMember(AstContext &context, llvm::SMLoc loc, llvm::StringRef name) const;

  llvm::StringRef typicalName(NodeModule module) const;
  bool isPathModule(NodeModule module) const;
  ResultModuleExport checkModuleExport(NodeModule module, llvm::StringRef name,
                                       llvm::StringRef path) const;

  Expr *moduleValue(NodeModule module, llvm::StringRef name, Expr *expr);
  ResultExportFound checkExportFound(NodeModule module, llvm::StringRef name,
                                     llvm::StringRef path) const;
  bool isProcessAlias(const NodeName &nodeName) const;

  struct NodeBinding {
    llvm::StringRef alias;
    llvm::SMLoc loc;
    NodeName node;
    llvm::StringRef name;
  };

  void bindNodeName(ModuleState &here, const ImportDecl &importDecl, const NodeBinding &binding);
  void reportWholeImport(Module *module, const ImportDecl &importDecl, NodeModule which,
                         ModuleState &here);
  std::vector<llvm::StringRef> wholeImportUses(Module *module, llvm::StringRef whole) const;
  std::string wholeImportMessage(const ImportDecl &importDecl, NodeModule which,
                                 const std::string &names, const std::string &form) const;
  void bindNamedImport(const ImportDecl &importDecl, const ModuleName &name, NodeModule which,
                       ModuleState &here);
  NodeName importedNode(NodeModule which, llvm::StringRef name) const;
  Expr *failNodeCall(CallExpr *call);
  Expr *checkNodeNameCall(CallExpr *call, NameExpr *name, const Type *expected);
  Expr *checkNodeMemberCall(CallExpr *call, MemberExpr *member, const Type *expected);
  Expr *checkSchedulerCall(CallExpr *call, MemberExpr *member);
  Expr *dispatchNodeCall(CallExpr *call, const NodeName &target, llvm::SMLoc loc,
                         const Type *expected);

  SemanticState &state;
  Scopes *scopes = nullptr;
  FsChecker *fsChecker = nullptr;
  OsChecker *osChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  PathChecker *pathChecker = nullptr;
  ChildChecker *childChecker = nullptr;
  TimerChecker *timerChecker = nullptr;
  CryptoChecker *cryptoChecker = nullptr;
  ReadlineChecker *readlineChecker = nullptr;
  FsPromisesChecker *fsPromisesChecker = nullptr;
  BuiltinModuleTypes *builtinModuleTypes = nullptr;
  CryptoFunctionChecker *cryptoFunctionChecker = nullptr;
};

}
