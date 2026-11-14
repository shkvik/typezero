#pragma once

#include "compiler/semantic/code/variables/scope_reports/scope_reports.h"
#include "compiler/semantic/code/variables/value_declarators/value_declarators.h"
#include "compiler/semantic/code/variables/variable_declarations/variable_declarations.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"

namespace tz {

class CodeVariablesRegistry {
public:
  explicit CodeVariablesRegistry(SemanticState &owner);

  void closeScope();
  void reportUntyped();
  void reportUncheckedResults();
  void afterInit(const AfterInit &task);
  void walkVarDecl(const WalkVarDecl &task);
  void nextDeclarator(const NextDeclarator &task);
  void afterEarlyTimer(const AfterEarlyTimer &task);
  void destructureValue(const DestructureValue &task);
  void afterDeclaredType(const AfterDeclaredType &task);
  void finishConstFunction(const FinishConstFunction &task);
  void rememberValueDeclarator(const RememberValueDeclarator &task);
  void afterConstFunctionSymbol(const AfterConstFunctionSymbol &task);
  void afterDestructuredIterator(const AfterDestructuredIterator &task);

private:
  ScopeReports scopeReports;
  ValueDeclarators valueDeclarators;
  VariableDeclarations variableDeclarations;
};

}
