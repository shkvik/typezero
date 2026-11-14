#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/declaration/links/declaration_links_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"

namespace tz {

class DeclarationLinksRegistry {
public:
  explicit DeclarationLinksRegistry(SemanticState &owner);

  void numberClasses();
  void startLinkClasses();
  void buildVtable(ClassDecl *classDecl);
  void linkOverride(const LinkOverride &task);
  void finishLinkClasses(const FinishLinkClasses &task);
  void startLinkOverrides(const StartLinkOverrides &task);
  void finishLinkOverrides(const FinishLinkOverrides &task);
  void interfacesOf(const ClassDecl *classDecl, std::vector<const Type *> &out) const;


  bool isTakingParams(const Type *functionType, const FuncSymbol *function) const;
  bool isOverridingTransitively(const FunctionDecl *functionDecl, const FunctionDecl *base) const;


  std::vector<FunctionDecl *> ownMethods(const ClassDecl *classDecl) const;

private:
  using ResultBodyIsRetained = Result<const FunctionDecl *, DeclarationLinksRegistryBodyKeptError>;
  using ResultMethodNotField = Result<const FunctionDecl *, DeclarationLinksRegistryNotAFieldError>;
  using ResultAccessNotLower =
      Result<const FunctionDecl *, DeclarationLinksRegistryAccessKeptError>;
  using ResultBaseNotPrivate =
      Result<const FunctionDecl *, DeclarationLinksRegistryBaseVisibleError>;
  using ResultSameTypeParams =
      Result<const FunctionDecl *, DeclarationLinksRegistrySameTypeArgsError>;
  using ResultBaseIsNotArrow =
      Result<const FunctionDecl *, DeclarationLinksRegistryArrowOverrideError>;
  using ResultOverrideTarget =
      Result<const FunctionDecl *, DeclarationLinksRegistryOverrideTargetError>;

  void linkOverrideReturn(FunctionDecl *functionDecl);
  void placeInVtable(ClassDecl *classDecl, FunctionDecl *functionDecl);
  void linkInterfaceReturn(const ClassDecl *classDecl, FunctionDecl *functionDecl);
  void linkInterfaceOverloads(const Type *interfaceType, FunctionDecl *functionDecl);
  void linkReturnTypes(const ClassDecl *classDecl,
                       llvm::SmallPtrSet<const FunctionDecl *, 4> &sources);
  void walkOverrideChain(const ClassDecl *classDecl, const FunctionDecl *functionDecl,
                         FunctionDecl *base);
  void addInterface(const Type *interfaceType, std::vector<const Type *> &queue,
                    std::vector<const Type *> &out) const;
  void walkNewMethod(const ClassDecl *classDecl, FunctionDecl *functionDecl,
                     const std::vector<FunctionDecl *> &inherited);
  void linkReturnType(const ClassDecl *classDecl, FunctionDecl *functionDecl,
                      llvm::SmallPtrSet<const FunctionDecl *, 4> &sources);
  void linkAbstractReturn(const ClassDecl *classDecl, FunctionDecl *functionDecl,
                          llvm::SmallPtrSet<const FunctionDecl *, 4> &sources);


  bool isArrowMethod(const FunctionDecl *functionDecl) const;
  bool isDeclaringReturn(const FunctionDecl *functionDecl) const;
  bool isSameParams(const FuncSymbol *first, const FuncSymbol *second) const;
  bool linkInterfaceField(const Type *interfaceType, FunctionDecl *functionDecl);
  bool reportBadOverride(const ClassDecl *classDecl, const FunctionDecl *functionDecl,
                         const FunctionDecl *base);


  FunctionDecl *ownImplementation(const ClassDecl *child, const FunctionDecl *functionDecl) const;
  FunctionDecl *firstImplementation(const ClassDecl *classDecl,
                                    const FunctionDecl *functionDecl) const;
  FunctionDecl *sameParamsBase(llvm::ArrayRef<FunctionDecl *> inherited,
                               const FunctionDecl *functionDecl) const;


  unsigned lastDescendantId(const ClassDecl *classDecl) const;
  ResultAccessNotLower checkAccessKept(const FunctionDecl *functionDecl,
                                       const FunctionDecl *base) const;
  ResultBaseNotPrivate checkBaseVisible(const FunctionDecl *functionDecl,
                                        const FunctionDecl *base) const;
  ResultMethodNotField checkNotAField(const ClassDecl *classDecl,
                                      const FunctionDecl *functionDecl) const;
  ResultBaseIsNotArrow checkArrowOverride(const FunctionDecl *functionDecl,
                                          const FunctionDecl *base) const;
  ResultBodyIsRetained checkBodyKept(const ClassDecl *classDecl, const FunctionDecl *functionDecl,
                                     const FunctionDecl *base) const;
  ResultSameTypeParams checkSameTypeArgs(const ClassDecl *classDecl,
                                         const FunctionDecl *functionDecl,
                                         const FunctionDecl *base) const;
  ResultOverrideTarget checkOverrideTarget(const ClassDecl *classDecl,
                                           const FunctionDecl *functionDecl,
                                           llvm::ArrayRef<FunctionDecl *> inherited) const;

  SemanticState &state;
};

}
