#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/stmt/var_checker/var_checker_errors.h"
#include "compiler/semantic/bodies/stmt/var_checker/var_checker_types.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class Scopes;
class CallCycles;
class Destination;
class DictChecker;
class ExprChecker;
class FlowChecker;
class IndexProofs;
class RegexChecker;
class TimerChecker;
class TypeResolver;
class TypeRelations;
class ClosureChecker;
class ConstantFolder;
class GenericChecker;
class FunctionChecker;
class IteratorMethods;
class GeneratorChecker;
class TzModulesChecker;
class KeyedFieldChecker;

class VarChecker {
public:
  VarChecker(SemanticState &owner, SemanticParts &all);

  static std::vector<const Stmt *> followingStatements(llvm::ArrayRef<Stmt *> stmts, size_t i);

  void walkVarDecl(VarDeclStmt *decl);
  void noteUntyped(VarSymbol *var, bool initUndefined);
  void assignedUntyped(const VarSymbol *var, const Type *type);
  void reportUntyped();
  void reportUncheckedResults();

private:
  using ResultInitHasType = Result<const Type *, VarCheckerInitHasTypeError>;
  using ResultConstValued = Result<const VarDeclarator *, VarCheckerConstValueError>;
  using ResultDestructure = Result<const Type *, VarCheckerDestructurableError>;
  using ResultResultCheck = Result<const VarSymbol *, VarCheckerResultCheckedError>;
  using ResultVarHasTypes = Result<const Type *, VarCheckerVarTypedError>;

  static ResultInitHasType initHasType(const VarDeclStmt *decl, const VarDeclarator &d,
                                       const Type *type);
  static ResultConstValued constValue(const VarDeclStmt *decl, const VarDeclarator &d);
  static ResultDestructure destructurable(const VarDeclarator &d, const Type *type);
  static ResultResultCheck resultChecked(const VarSymbol *var);
  static ResultVarHasTypes varTyped(const Untyped &u, const Type *type);
  static bool isTextChoice(const Expr *expr);

  static bool isFreshArray(const Expr *value);
  static void nameFunctionValue(VarDeclarator &d);
  static const Expr *withoutImplicitCasts(const Expr *expr);
  static std::vector<std::u16string> literalKeyTexts(const Expr *key);
  std::vector<llvm::StringRef> reflectedNames(const Expr *key);
  void walkDeclarator(const DeclaratorPlace &place);
  void walkValueDeclarator(const DeclaratorPlace &place);
  DeclaratorForm declaratorForm(const DeclaratorPlace &place);
  void rememberReflection(VarDeclarator &d, const Expr *value);
  void liftGenericFunction(const DeclaratorPlace &place, FunctionDecl *fn);
  void declareGenericAlias(const DeclaratorPlace &place, FuncSymbol *generic);
  void declareConstFunction(const DeclaratorPlace &place, FunctionExpr *function);
  void findDestination(const DeclaratorPlace &place, VariableValue &variable);
  void declareTimerEarly(const DeclaratorPlace &place, VariableValue &variable);
  void walkVariableValue(const DeclaratorPlace &place, VariableValue &variable);
  void walkInit(const DeclaratorPlace &place, VariableValue &variable);
  void typeFromInit(const DeclaratorPlace &place, VariableValue &variable);
  void typeConstWithoutValue(const DeclaratorPlace &place, VariableValue &variable);
  void walkDestructuring(const DeclaratorPlace &place, VariableValue &variable);
  void spreadTextIntoArray(const DeclaratorPlace &place, VariableValue &variable);
  void destructureIterator(const DeclaratorPlace &place, VariableValue &variable);
  void reportPatternShape(const DeclaratorPlace &place, VariableValue &variable);
  void declareVariable(const DeclaratorPlace &place, const VariableValue &variable);
  void markUnsharedArray(const VarDeclarator &d, const VariableValue &variable);
  void rememberConstLiteral(const DeclaratorPlace &place, const VariableValue &variable);
  void rememberResultVariable(const DeclaratorPlace &place, const VariableValue &variable);
  void rememberFixedLength(const DeclaratorPlace &place, const VariableValue &variable);
  void rememberConstNumber(const DeclaratorPlace &place, const VariableValue &variable);
  void rememberConstText(const DeclaratorPlace &place, const VariableValue &variable);

  bool exported(llvm::StringRef name) const;
  SemanticState &state;
  Scopes *scopes = nullptr;
  CallCycles *callCycles = nullptr;
  Destination *destination = nullptr;
  DictChecker *dictChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  FlowChecker *flowChecker = nullptr;
  IndexProofs *indexProofs = nullptr;
  RegexChecker *regexChecker = nullptr;
  TimerChecker *timerChecker = nullptr;
  TypeResolver *typeResolver = nullptr;
  TypeRelations *typeRelations = nullptr;
  ClosureChecker *closureChecker = nullptr;
  ConstantFolder *constantFolder = nullptr;
  GenericChecker *genericChecker = nullptr;
  FunctionChecker *functionChecker = nullptr;
  IteratorMethods *iteratorMethods = nullptr;
  GeneratorChecker *generatorChecker = nullptr;
  TzModulesChecker *tzModulesChecker = nullptr;
  KeyedFieldChecker *keyedFieldChecker = nullptr;
};

}
