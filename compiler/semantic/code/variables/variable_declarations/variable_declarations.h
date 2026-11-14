#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/stmt/var_checker/var_checker_types.h"
#include "compiler/semantic/code/variables/code_variables_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <vector>

namespace tz {

class VariableDeclarations {
public:
  explicit VariableDeclarations(SemanticState &owner);

  void afterInit(const AfterInit &task);
  void walkVarDecl(const WalkVarDecl &task);
  void nextDeclarator(const NextDeclarator &task);
  void afterEarlyTimer(const AfterEarlyTimer &task);
  void findDestination(const FindDestination &task);
  void destructureValue(const DestructureValue &task);
  void afterDeclaredType(const AfterDeclaredType &task);
  void finishConstFunction(const FinishConstFunction &task);
  void afterConstFunctionSymbol(const AfterConstFunctionSymbol &task);
  void afterDestructuredIterator(const AfterDestructuredIterator &task);

private:
  using ResultInitHasType = Result<const Type *, CodeVariablesRegistryInitHasTypeError>;
  using ResultDestructure = Result<const Type *, CodeVariablesRegistryDestructurableError>;
  using ResultConstValued = Result<const VarDeclarator *, CodeVariablesRegistryConstValueError>;

  void startInit(DeclaratorDraft draft);
  void startValueOrTimer(DeclaratorDraft draft);
  void startVariableValue(DeclaratorDraft draft);
  void declareValueVariable(DeclaratorDraft draft);
  void spreadTextIntoArray(DeclaratorDraft &draft);
  void startDeclarator(const DeclaratorDraft &draft);
  void nameFunctionValue(VarDeclarator &declarator) const;
  void startValueDeclarator(const DeclaratorDraft &start);
  void declareGenericAlias(const DeclaratorDraft &draft, FuncSymbol *generic);
  void startConstFunction(const DeclaratorDraft &draft, FunctionExpr *functionExpr);
  void liftGenericFunction(const DeclaratorDraft &draft, FunctionDecl *functionDecl);


  bool isSeekingDestination(const DeclaratorDraft &draft) const;


  ResultDestructure checkDestructurable(const VarDeclarator &declarator, const Type *type) const;
  ResultConstValued checkConstValue(const VarDeclStmt *varDecl,
                                    const VarDeclarator &declarator) const;
  ResultInitHasType checkInitHasType(const VarDeclStmt *varDecl, const VarDeclarator &declarator,
                                     const Type *type) const;


  Signature signatureOf(const Type *functionType) const;
  DeclaratorForm declaratorForm(const DeclaratorDraft &draft) const;
  std::vector<const Stmt *> followingStatements(llvm::ArrayRef<Stmt *> stmts, size_t index) const;

  SemanticState &state;
};

}
