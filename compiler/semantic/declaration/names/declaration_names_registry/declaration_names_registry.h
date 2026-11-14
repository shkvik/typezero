#pragma once

#include "compiler/program/library/builtin_names/global_member_table/global_member_table.h"
#include "compiler/program/library/builtin_names/prelude_type_table/prelude_type_table.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/declaration/names/declaration_names_registry/declaration_names_registry_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include <vector>

namespace tz {

class SemanticParts;
class ClassLinks;
class VarChecker;
class ImportBinder;
class TypeDeclarer;
class ReturnInferrer;
class AliasInstantiator;
class BuiltinModuleTypes;
class BuiltinModuleChecker;

class DeclarationNamesRegistry {
public:
  using ResultEntryDeclared = Result<ScopeEntry, DeclarationNamesRegistryDeclareError>;
  using ResultNameIsVisible = Result<const VarSymbol *, DeclarationNamesRegistryCheckVisibleError>;

  DeclarationNamesRegistry(SemanticState &owner, SemanticParts &all);

  void openScope();
  void closeScope();
  void rememberNamesInFunctions(SourceFile &program);


  bool isUnshadowedGlobal(const NameExpr *name) const;
  bool hasMethodFields(const ClassDecl *classDecl) const;
  bool isGlobalName(const Expr *expr, GlobalName global) const;
  bool isGlobalMember(const MemberExpr *member, GlobalMember which) const;


  const Type *findPreludeType(PreludeType type);
  const Type *resolveVariableType(VarSymbol *variable);
  const Type *instantiatePreludeType(llvm::StringRef name, std::vector<const Type *> args);


  VarSymbol *declareLet(llvm::StringRef name, llvm::SMLoc loc, const Type *type);
  VarSymbol *declareConst(llvm::StringRef name, llvm::SMLoc loc, const Type *type);
  VarSymbol *declareVar(llvm::StringRef name, llvm::SMLoc loc, const Type *type, bool isConst);


  std::vector<FunctionDecl *> collectMemberFunctions(const ClassDecl *classDecl) const;
  std::vector<FunctionDecl *> findMethodsNamed(const ClassDecl *classDecl,
                                               llvm::StringRef name) const;
  std::vector<FunctionDecl *> findStaticMethodsNamed(const ClassDecl *classDecl,
                                                     llvm::StringRef name) const;


  ClassDecl *findClassOf(const Type *type) const;
  ScopeEntry findName(llvm::StringRef name) const;
  Stmt *findTypeDeclaration(llvm::StringRef name) const;
  FuncSymbol *findGenericFunction(const NameExpr *name) const;
  ResultEntryDeclared declare(llvm::StringRef name, ScopeEntry entry);
  ResultNameIsVisible checkVisible(NameExpr *name, const VarSymbol *variable) const;
  const ClassField *findField(const ClassDecl *classDecl, llvm::StringRef name) const;
  const ClassDecl *findFieldOwner(const ClassDecl *classDecl, llvm::StringRef name) const;

private:
  using ResultNameIsAllowed = Result<void, DeclarationNamesRegistryDeclareError>;

  void placeVariable(VarSymbol *variable);
  void notePreviousDeclaration(llvm::StringRef name);
  void addNotHidden(std::vector<FunctionDecl *> &found,
                    const std::vector<FunctionDecl *> &own) const;
  void addNotOverridden(std::vector<FunctionDecl *> &found,
                        const std::vector<FunctionDecl *> &own) const;
  void reportDeclareError(llvm::SMLoc loc, llvm::StringRef name,
                          const DeclarationNamesRegistryDeclareError &error);


  bool hasOwnMethodField(const ClassDecl *classDecl) const;
  bool isNamingGlobal(const Expr *expr, GlobalName global) const;
  bool isGlobalThisMember(const Expr *expr, GlobalName global) const;
  bool hasSameSignature(const FuncSymbol *first, const FuncSymbol *second) const;
  bool isHiddenByAny(const std::vector<FunctionDecl *> &found, const FunctionDecl *method) const;
  bool isOverriddenByAny(const std::vector<FunctionDecl *> &found,
                         const FunctionDecl *method) const;


  ResultNameIsAllowed checkNotDeclared(llvm::StringRef name) const;
  ResultNameIsAllowed checkNameAllowed(llvm::StringRef name) const;
  ResultNameIsAllowed checkFunctionOnce(llvm::StringRef name, const ScopeEntry &entry) const;


  std::vector<FunctionDecl *> ownMethodsNamed(const ClassDecl *classDecl,
                                              llvm::StringRef name) const;
  std::vector<FunctionDecl *> ownStaticMethodsNamed(const ClassDecl *classDecl,
                                                    llvm::StringRef name) const;


  FunctionDecl *functionOf(VarSymbol *variable) const;

  SemanticState &state;
  ClassLinks *classLinks = nullptr;
  VarChecker *varChecker = nullptr;
  ImportBinder *importBinder = nullptr;
  TypeDeclarer *typeDeclarer = nullptr;
  ReturnInferrer *returnInferrer = nullptr;
  AliasInstantiator *aliasInstantiator = nullptr;
  BuiltinModuleTypes *builtinModuleTypes = nullptr;
  BuiltinModuleChecker *builtinModuleChecker = nullptr;
};

}
