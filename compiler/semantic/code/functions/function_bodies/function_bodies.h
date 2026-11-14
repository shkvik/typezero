#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/code/functions/code_functions_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include "llvm/ADT/StringSet.h"
#include <vector>

namespace tz {

class FunctionBodies {
public:
  explicit FunctionBodies(SemanticState &owner);

  void enterErrorWatch();
  void leaveErrorWatch();
  void afterBodyThis(const AfterBodyThis &task);
  void nextBodyParam(const NextBodyParam &task);
  void walkTopFunction(const WalkTopFunction &task);
  void walkFunctionBody(const WalkFunctionBody &task);
  void walkInferredBody(const WalkInferredBody &task);
  void afterPredicateEnd(const AfterPredicateEnd &task);
  void startModuleBodies(const StartModuleBodies &task);
  void finishFunctionBody(const FinishFunctionBody &task);
  void afterTopReturnType(const AfterTopReturnType &task);
  void afterBodyTypeParams(const AfterBodyTypeParams &task);
  void afterDefaultedParam(const AfterDefaultedParam &task);
  void afterBodyStatements(const AfterBodyStatements &task);
  void declareArgumentList(const DeclareArgumentList &task);
  void startLiftedFunction(const StartLiftedFunction &task);
  void rememberArgumentList(const RememberArgumentList &task);
  void walkFunctionFromScratch(const WalkFunctionFromScratch &task);

private:
  using ResultListIsSpread = Result<const FunctionDecl *, CodeFunctionsRegistryRestListValueError>;
  using ResultConstructorSuper =
      Result<const FunctionDecl *, CodeFunctionsRegistrySuperCalledError>;
  using ResultDefaultUsesPrior =
      Result<const FunctionDecl *, CodeFunctionsRegistryDefaultOrderError>;
  using ResultEveryPathReturns =
      Result<const FunctionDecl *, CodeFunctionsRegistryReturnsEverywhereError>;

  void pushItemBody(Stmt *item);
  void walkRootSupers(FunctionDecl *functionDecl);
  void startBodyStatements(const BodyDraft &draft);
  void walkBodyReturns(const FunctionDecl *functionDecl);
  void enterInitializer(FunctionDecl *functionDecl, VarSymbol *self);
  void addFixedUses(const Expr *expr, llvm::SmallPtrSetImpl<const Expr *> &uses) const;
  void addSpreadUses(const Expr *expr, llvm::SmallPtrSetImpl<const Expr *> &uses) const;
  void markBrokenOwners(const FunctionDecl *functionDecl,
                        const std::vector<Diagnostic> &bodyDiagnostics);
  void declareListValue(FunctionDecl *functionDecl, llvm::SMLoc loc,
                        llvm::ArrayRef<llvm::StringRef> parts,
                        llvm::ArrayRef<const Type *> elements);


  bool isUsedAsValue(const Stmt *body, llvm::StringRef name, bool isFixed) const;
  bool isPresetField(const FunctionDecl *functionDecl, const ClassField &field,
                     bool isDerived) const;


  ResultConstructorSuper checkSuperCalled(const FunctionDecl *functionDecl, unsigned supers) const;
  ResultEveryPathReturns checkReturnsEverywhere(const FunctionDecl *functionDecl,
                                                const Type *returnType) const;
  ResultDefaultUsesPrior checkDefaultOrder(const FunctionDecl *functionDecl, size_t index,
                                           const llvm::StringSet<> &names) const;


  CallExpr *superCallOf(Stmt *stmt) const;
  const Type *bodyReturnType(const FunctionDecl *functionDecl) const;
  ResultListIsSpread restListValue(const FunctionDecl *functionDecl) const;
  llvm::SmallPtrSet<const Expr *, 4> listUses(const PlacedNodes &nodes, bool isFixed) const;

  SemanticState &state;
};

}
