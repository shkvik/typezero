#include "compiler/pipeline/check_program/check_state/expression_marks/expression_marks.h"

namespace tz {

void ExpressionMarks::rememberConcatChain(BinaryExpr *chain) {
  this->concatChain = chain;
}

void ExpressionMarks::forgetConcatChain() {
  this->concatChain = nullptr;
}

BinaryExpr *ExpressionMarks::takeConcatChain() {
  BinaryExpr *chain = this->concatChain;
  this->concatChain = nullptr;

  return chain;
}

void ExpressionMarks::rememberMixedConcat(const BinaryExpr *mixed) {
  this->mixedConcat = mixed;
}

const BinaryExpr *ExpressionMarks::takeMixedConcat() {
  const BinaryExpr *mixed = this->mixedConcat;
  this->mixedConcat = nullptr;

  return mixed;
}

const BinaryExpr *ExpressionMarks::getMixedConcat() const {
  return this->mixedConcat;
}

void ExpressionMarks::enterVariableInit(llvm::StringRef name) {
  this->initializing.push_back(name);
}

void ExpressionMarks::leaveVariableInit() {
  this->initializing.pop_back();
}

bool ExpressionMarks::isInitializing(llvm::StringRef name) const {
  return llvm::is_contained(this->initializing, name);
}

void ExpressionMarks::enterModuleLoad() {
  ModuleLoadMarks saved;
  saved.initializing = std::move(this->initializing);
  saved.concatChain = this->concatChain;
  saved.mixedConcat = this->mixedConcat;
  this->moduleLoads.push_back(std::move(saved));
  this->initializing.clear();
  this->concatChain = nullptr;
  this->mixedConcat = nullptr;
}

void ExpressionMarks::leaveModuleLoad() {
  ModuleLoadMarks saved = std::move(this->moduleLoads.back());
  this->moduleLoads.pop_back();
  this->concatChain = saved.concatChain;
  this->mixedConcat = saved.mixedConcat;
  this->initializing = std::move(saved.initializing);
}

void ExpressionMarks::markPrechecked(const Expr *expr) {
  this->prechecked.insert(expr);
}

bool ExpressionMarks::isPrechecked(const Expr *expr) const {
  return this->prechecked.contains(expr);
}

void ExpressionMarks::forgetPrechecked(const Expr *expr) {
  this->prechecked.erase(expr);
}

void ExpressionMarks::markPairLiteral(const Expr *literal) {
  this->pairLiterals.insert(literal);
}

bool ExpressionMarks::isPairLiteral(const Expr *literal) const {
  return this->pairLiterals.contains(literal);
}

void ExpressionMarks::forgetPairLiteral(const Expr *literal) {
  this->pairLiterals.erase(literal);
}

void ExpressionMarks::markSeveralVariants(const CallExpr *call) {
  this->severalVariants.insert(call);
}

bool ExpressionMarks::hasSeveralVariants(const CallExpr *call) const {
  return this->severalVariants.contains(call);
}

void ExpressionMarks::rememberArgumentList(const VarSymbol *var,
                                           std::vector<llvm::StringRef> parts) {
  this->argumentLists[var] = std::move(parts);
}

std::optional<std::vector<llvm::StringRef>>
ExpressionMarks::findArgumentList(const VarSymbol *var) const {
  auto found = this->argumentLists.find(var);
  if (found == this->argumentLists.end()) {
    return std::nullopt;
  }

  return found->second;
}

void ExpressionMarks::rememberIterateSymbol(FuncSymbol *fn) {
  this->iterateSymbol = fn;
}

FuncSymbol *ExpressionMarks::getIterateSymbol() const {
  return this->iterateSymbol;
}

void ExpressionMarks::addUntyped(VarSymbol *var, bool initUndefined) {
  this->untyped.push_back({var, initUndefined, {}});
}

bool ExpressionMarks::hasUntyped() const {
  return !this->untyped.empty();
}

void ExpressionMarks::addUntypedAssignment(const VarSymbol *var, const Type *type) {
  for (Untyped &entry : this->untyped) {
    if (entry.var == var) {
      entry.assigned.push_back(type);
    }
  }
}

std::vector<Untyped> ExpressionMarks::getUntyped() const {
  return this->untyped;
}

void ExpressionMarks::forgetUntyped(const VarSymbol *var) {
  llvm::erase_if(this->untyped, [&](const Untyped &entry) { return entry.var == var; });
}

void ExpressionMarks::addResultVar(VarSymbol *var) {
  this->resultVars.push_back(var);
}

std::vector<VarSymbol *> ExpressionMarks::getResultVars() const {
  return this->resultVars;
}

void ExpressionMarks::forgetResultVars() {
  this->resultVars.clear();
}

}
