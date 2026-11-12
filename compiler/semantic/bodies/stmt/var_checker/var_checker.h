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

  std::vector<const Stmt *> followingStatements(llvm::ArrayRef<Stmt *> stmts, size_t index) const;

  void walkVarDecl(VarDeclStmt *varDecl);
  void noteUntyped(VarSymbol *variable, bool initUndefined);
  void assignedUntyped(const VarSymbol *variable, const Type *type);
  void reportUntyped();
  void reportUncheckedResults();

private:
  using ResultInitHasType = Result<const Type *, VarCheckerInitHasTypeError>;
  using ResultConstValued = Result<const VarDeclarator *, VarCheckerConstValueError>;
  using ResultDestructure = Result<const Type *, VarCheckerDestructurableError>;
  using ResultResultCheck = Result<const VarSymbol *, VarCheckerResultCheckedError>;
  using ResultVarHasTypes = Result<const Type *, VarCheckerVarTypedError>;

  ResultInitHasType checkInitHasType(const VarDeclStmt *varDecl, const VarDeclarator &declarator,
                                     const Type *type) const;
  ResultConstValued checkConstValue(const VarDeclStmt *varDecl,
                                    const VarDeclarator &declarator) const;
  ResultDestructure checkDestructurable(const VarDeclarator &declarator, const Type *type) const;
  ResultResultCheck resultChecked(const VarSymbol *variable) const;
  ResultVarHasTypes varTyped(const Untyped &untyped, const Type *type) const;
  std::string typeHint(const Untyped &untyped, const Type *type) const;
  bool isTextChoice(const Expr *expr) const;
  const Type *declaredType(const VarDeclarator &declarator);
  void walkDeclarator(const DeclaratorPlace &place);
  DeclaratorForm declaratorForm(const DeclaratorPlace &place);
  FuncSymbol *aliasedGeneric(Expr *init);
  void declareGenericAlias(const DeclaratorPlace &place, FuncSymbol *generic);
  void liftGenericFunction(const DeclaratorPlace &place, FunctionDecl *functionDecl);
  void declareConstFunction(const DeclaratorPlace &place, FunctionExpr *functionExpr);
  Expr *checkConstFunction(FunctionExpr *functionExpr, const Type *declared);
  void nameFunctionValue(VarDeclarator &declarator) const;
  void walkValueDeclarator(const DeclaratorPlace &place);
  void findDestination(const DeclaratorPlace &place, VariableValue &variable);
  std::vector<const Stmt *> followingInBlock(const DeclaratorPlace &place) const;
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
  void rememberReflection(VarDeclarator &declarator, const Expr *value);
  void rememberReflectedNames(VarDeclarator &declarator, const Expr *key);
  std::vector<llvm::StringRef> reflectedNames(const Expr *key);
  std::vector<std::u16string> literalKeyTexts(const Expr *key) const;
  std::vector<const Type *> keyMembers(const Type *keyType) const;
  void markUnsharedArray(const VarDeclarator &declarator, const VariableValue &variable);
  bool isFreshArray(const Expr *value) const;
  void rememberConstLiteral(const DeclaratorPlace &place, const VariableValue &variable);
  bool isLiteralValue(const Expr *value) const;
  void rememberResultVariable(const DeclaratorPlace &place, const VariableValue &variable);
  void rememberFixedLength(const DeclaratorPlace &place, const VariableValue &variable);
  void rememberConstNumber(const DeclaratorPlace &place, const VariableValue &variable);
  void rememberConstText(const DeclaratorPlace &place, const VariableValue &variable);
  bool isExported(llvm::StringRef name) const;
  bool isExportingName(const ExportDecl &exportDecl, llvm::StringRef name) const;
  void reportUncheckedResult(const VarSymbol *variable);
  bool isUntypedHere(const Untyped &untyped) const;
  void reportUntypedVariable(const Untyped &untyped);
  const Type *assignedType(const Untyped &untyped) const;
  const Type *commonAssigned(const Type *type, const Type *candidate) const;

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
