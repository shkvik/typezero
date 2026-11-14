#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/generics_v2/classes/generics_classes_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <optional>

namespace tz {

class GenericNews {
public:
  explicit GenericNews(SemanticState &owner);

  void afterEmitterNew(const AfterEmitterNew &task);
  void checkGenericNew(const CheckGenericNew &task);
  void afterExplicitInstance(const AfterExplicitInstance &task);
  void afterGenericNewBinding(const AfterGenericNewBinding &task);
  void afterGenericNewInstance(const AfterGenericNewInstance &task);
  void nextExplicitNewArgument(const NextExplicitNewArgument &task);
  void afterExplicitNewArgument(const AfterExplicitNewArgument &task);

private:
  using ResultClassConcrete = Result<const ClassDecl *, GenericsClassesRegistryConcreteError>;
  using ResultCtorWasPicked = Result<FunctionDecl *, GenericsClassesRegistryConstructorFitError>;
  using ResultClassDeclared = Result<const ClassDecl *, GenericsClassesRegistryDeclaredFirstError>;

  void walkArgsLoosely(NewExpr *expr);
  void startGenericNew(const CheckGenericNew &task);
  void afterLibraryTries(const CheckGenericNew &task);


  bool hasInitializer(const ClassDecl *instance) const;
  bool isDerivedStream(const ClassDecl *classDecl) const;
  bool isTakingCount(const FuncSymbol *function, size_t count) const;


  ClassDecl *instanceOf(const Type *type) const;
  ResultClassConcrete concrete(const ClassDecl *generic) const;
  ResultClassDeclared declaredFirst(const ClassDecl *generic) const;
  FuncSymbol *constructorFunction(ClassDecl *generic, FunctionDecl *constructor);
  ResultCtorWasPicked constructorFit(const NewExpr *expr, const ClassDecl *generic,
                                     llvm::ArrayRef<FunctionDecl *> constructors) const;
  std::optional<FunctionDecl *> pickedConstructor(const NewExpr *expr, const ClassDecl *generic,
                                                  llvm::ArrayRef<FunctionDecl *> constructors);

  SemanticState &state;
};

}
