#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/library/builtin_modules/known_modules/known_modules.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/library_v2/dispatch/library_dispatch_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <string>
#include <vector>

namespace tz {

class LibraryDispatchRegistry {
public:
  explicit LibraryDispatchRegistry(SemanticState &owner);

  void declareErrorTypes();
  void finishAhead(const FinishAhead &task);
  void prepareLibraries(SourceFile &program);
  void checkLibCall(const CheckLibCall &task);
  void checkNodeCall(const CheckNodeCall &task);
  void checkNodeName(const CheckNodeName &task);
  void finishAsClass(const FinishAsClass &task);
  void afterFsConstant(const AfterFsConstant &task);
  void checkNodeMember(const CheckNodeMember &task);
  void namedModuleType(const NamedModuleType &task);
  void afterModuleValue(const AfterModuleValue &task);
  void processAliasSlot(const ProcessAliasSlot &task);
  void finishWithContext(const FinishWithContext &task);
  void afterIntrinsicType(const AfterIntrinsicType &task);
  void afterQuestionQuery(const AfterQuestionQuery &task);
  void checkRuntimeMember(const CheckRuntimeMember &task);
  void checkRuntimeMethod(const CheckRuntimeMethod &task);
  void checkRuntimeTarget(const CheckRuntimeTarget &task);
  void importBuiltinModule(const ImportBuiltinModule &task);
  void finishQuestionPromise(const FinishQuestionPromise &task);
  void finishQuestionCallback(const FinishQuestionCallback &task);
  void afterCryptoModuleMember(const AfterCryptoModuleMember &task);
  void finishListenerIntrinsic(const FinishListenerIntrinsic &task);

private:
  using ResultIntrinsicShape = Result<const CallExpr *, LibraryDispatchRegistryIntrinsicShapeError>;

  struct NodeBinding {
    llvm::StringRef alias;
    llvm::SMLoc loc;
    NodeName node;
    llvm::StringRef name;
  };

  void declare(BuiltinText builtin);
  void failNodeCall(CallExpr *call);
  void pushLooseArgs(CallExpr *call);
  void prepareLibrary(Module *module);
  void startQuestion(CallExpr *call, MemberExpr *member);
  void reportNoMember(CallExpr *call, MemberExpr *member);
  void exportFrom(NodeModule module, llvm::StringRef name);
  void rememberLibraryItem(Stmt *item, bool hasIntrinsics);
  void startNodeValue(const NodeName &nodeName, Expr *expr);
  void checkReadlineClose(CallExpr *call, MemberExpr *member);
  void startReadlineMethod(CallExpr *call, MemberExpr *member);
  void reportReadlineEvents(CallExpr *call, MemberExpr *member);
  void startAsyncIteratorMethod(CallExpr *call, MemberExpr *member);
  void startIntrinsic(const IntrinsicDraft &draft, const Type *typeArg);
  void startQuestionCallback(CallExpr *call, MemberExpr *member, Expr *query);
  void rememberLibraryFunction(FunctionDecl *functionDecl, bool hasIntrinsics);
  void startNodeNameCall(CallExpr *call, NameExpr *name, const Type *expected);
  void startListenerIntrinsic(const IntrinsicDraft &draft, const Type *typeArg);
  void startNodeMemberCall(CallExpr *call, MemberExpr *member, const Type *expected);
  void bindNodeName(ModuleState &here, const ImportDecl &importDecl, const NodeBinding &binding);
  void dispatchNodeCall(CallExpr *call, const NodeName &target, llvm::SMLoc loc,
                        const Type *expected);
  void reportWholeImport(Module *module, const ImportDecl &importDecl, NodeModule which,
                         ModuleState &here);
  void bindNamedImport(const ImportDecl &importDecl, const ModuleName &name, NodeModule which,
                       ModuleState &here);


  bool isAwaitable(const Type *type) const;
  bool isPathModule(NodeModule module) const;
  bool isProcessAlias(const NodeName &nodeName) const;
  bool hasWrongArity(CallExpr *call, MemberExpr *member, unsigned min, unsigned max);


  Expr *processAlias(Expr *expr);
  const Type *flatListenerType(const Type *typeArg) const;
  NodeName importedNode(NodeModule which, llvm::StringRef name) const;
  std::vector<llvm::StringRef> wholeImportUses(Module *module, llvm::StringRef whole) const;
  MemberExpr *processMember(AstContext &context, llvm::SMLoc loc, llvm::StringRef name) const;
  std::string wholeImportMessage(const ImportDecl &importDecl, NodeModule which,
                                 const std::string &names, const std::string &form) const;
  ResultIntrinsicShape checkIntrinsicShape(const CallExpr *call, const NameExpr *name,
                                           llvm::StringRef intrinsicName, bool known, unsigned args,
                                           unsigned typeArgs) const;

  SemanticState &state;
};

}
