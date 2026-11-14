#include "compiler/semantic/code/variables/code_variables_registry.h"

namespace tz {

CodeVariablesRegistry::CodeVariablesRegistry(SemanticState &owner)
    : scopeReports(owner),
      valueDeclarators(owner),
      variableDeclarations(owner) {}

void CodeVariablesRegistry::walkVarDecl(const WalkVarDecl &task) {
  return this->variableDeclarations.walkVarDecl(task);
}

void CodeVariablesRegistry::nextDeclarator(const NextDeclarator &task) {
  return this->variableDeclarations.nextDeclarator(task);
}

void CodeVariablesRegistry::afterDeclaredType(const AfterDeclaredType &task) {
  return this->variableDeclarations.afterDeclaredType(task);
}

void CodeVariablesRegistry::afterConstFunctionSymbol(const AfterConstFunctionSymbol &task) {
  return this->variableDeclarations.afterConstFunctionSymbol(task);
}

void CodeVariablesRegistry::finishConstFunction(const FinishConstFunction &task) {
  return this->variableDeclarations.finishConstFunction(task);
}

void CodeVariablesRegistry::afterEarlyTimer(const AfterEarlyTimer &task) {
  return this->variableDeclarations.afterEarlyTimer(task);
}

void CodeVariablesRegistry::afterInit(const AfterInit &task) {
  return this->variableDeclarations.afterInit(task);
}

void CodeVariablesRegistry::destructureValue(const DestructureValue &task) {
  return this->variableDeclarations.destructureValue(task);
}

void CodeVariablesRegistry::afterDestructuredIterator(const AfterDestructuredIterator &task) {
  return this->variableDeclarations.afterDestructuredIterator(task);
}

void CodeVariablesRegistry::rememberValueDeclarator(const RememberValueDeclarator &task) {
  return this->valueDeclarators.rememberValueDeclarator(task);
}

void CodeVariablesRegistry::closeScope() {
  return this->scopeReports.closeScope();
}

void CodeVariablesRegistry::reportUncheckedResults() {
  return this->scopeReports.reportUncheckedResults();
}

void CodeVariablesRegistry::reportUntyped() {
  return this->scopeReports.reportUntyped();
}

}
