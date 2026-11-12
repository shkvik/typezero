#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/classes/class_links/class_links_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class ClassBases;
class SemanticParts;
class ChunkTypes;
class ClassDeclarer;
class GenericClassInstantiator;

class ClassLinks {
public:
  ClassLinks(SemanticState &owner, SemanticParts &all);

  void numberClasses();
  void buildVtable(ClassDecl *classDecl);
  void linkClasses();
  void linkOverrides(ClassDecl *classDecl);
  std::vector<FunctionDecl *> constructorsOf(const ClassDecl *classDecl) const;
  std::vector<FunctionDecl *> ownMethods(const ClassDecl *classDecl) const;
  bool isOverridingTransitively(const FunctionDecl *functionDecl, const FunctionDecl *base) const;
  FunctionDecl *soleImplementation(const ClassDecl *classDecl, FunctionDecl *variant) const;
  const Type *inheritedReturnType(FuncSymbol *function);
  void noteInheritedReturn(const FunctionDecl *functionDecl);
  static const Type *commonClass(const Type *first, const Type *second);
  bool isTakingParams(const Type *functionType, const FuncSymbol *function) const;
  bool isArrowMethod(const FunctionDecl *functionDecl) const;
  void interfacesOf(const ClassDecl *classDecl, std::vector<const Type *> &out) const;

private:
  using ResultMethodNotField = Result<const FunctionDecl *, ClassLinksNotAFieldError>;
  using ResultOverrideTarget = Result<const FunctionDecl *, ClassLinksOverrideTargetError>;
  using ResultBaseIsNotArrow = Result<const FunctionDecl *, ClassLinksArrowOverrideError>;
  using ResultBaseNotPrivate = Result<const FunctionDecl *, ClassLinksBaseVisibleError>;
  using ResultAccessNotLower = Result<const FunctionDecl *, ClassLinksAccessKeptError>;
  using ResultBodyIsRetained = Result<const FunctionDecl *, ClassLinksBodyKeptError>;
  using ResultSameTypeParams = Result<const FunctionDecl *, ClassLinksSameTypeArgsError>;

  struct OwnedMethod {
    FunctionDecl *method;
    const ClassDecl *owner;
  };

  bool isSameParams(const FuncSymbol *first, const FuncSymbol *second) const;
  bool isDeclaringReturn(const FunctionDecl *functionDecl) const;
  void addInterface(const Type *interfaceType, std::vector<const Type *> &queue,
                    std::vector<const Type *> &out) const;
  std::vector<OwnedMethod> implementationsBelow(const ClassDecl *classDecl,
                                                const FunctionDecl *variant) const;
  void addImplementations(const ClassDecl *child, const FunctionDecl *variant,
                          std::vector<OwnedMethod> &implementations) const;
  FunctionDecl *firstImplementation(const ClassDecl *classDecl,
                                    const FunctionDecl *functionDecl) const;
  FunctionDecl *ownImplementation(const ClassDecl *child, const FunctionDecl *functionDecl) const;
  void linkReturnTypes(const ClassDecl *classDecl,
                       llvm::SmallPtrSet<const FunctionDecl *, 4> &sources);
  void linkReturnType(const ClassDecl *classDecl, FunctionDecl *functionDecl,
                      llvm::SmallPtrSet<const FunctionDecl *, 4> &sources);
  void linkAbstractReturn(const ClassDecl *classDecl, FunctionDecl *functionDecl,
                          llvm::SmallPtrSet<const FunctionDecl *, 4> &sources);
  void linkOverrideReturn(FunctionDecl *functionDecl);
  void linkInterfaceReturn(const ClassDecl *classDecl, FunctionDecl *functionDecl);
  bool linkInterfaceField(const Type *interfaceType, FunctionDecl *functionDecl);
  void linkInterfaceOverloads(const Type *interfaceType, FunctionDecl *functionDecl);
  unsigned lastDescendantId(const ClassDecl *classDecl) const;
  void placeInVtable(ClassDecl *classDecl, FunctionDecl *functionDecl);
  ResultMethodNotField checkNotAField(const ClassDecl *classDecl,
                                      const FunctionDecl *functionDecl) const;
  ResultOverrideTarget checkOverrideTarget(const ClassDecl *classDecl,
                                           const FunctionDecl *functionDecl,
                                           llvm::ArrayRef<FunctionDecl *> inherited) const;
  ResultBaseIsNotArrow checkArrowOverride(const FunctionDecl *functionDecl,
                                          const FunctionDecl *base) const;
  ResultBaseNotPrivate checkBaseVisible(const FunctionDecl *functionDecl,
                                        const FunctionDecl *base) const;
  ResultAccessNotLower checkAccessKept(const FunctionDecl *functionDecl,
                                       const FunctionDecl *base) const;
  ResultBodyIsRetained checkBodyKept(const ClassDecl *classDecl, const FunctionDecl *functionDecl,
                                     const FunctionDecl *base) const;
  ResultSameTypeParams checkSameTypeArgs(const ClassDecl *classDecl,
                                         const FunctionDecl *functionDecl,
                                         const FunctionDecl *base) const;
  void linkOverride(ClassDecl *classDecl, FunctionDecl *functionDecl);
  FunctionDecl *sameParamsBase(llvm::ArrayRef<FunctionDecl *> inherited,
                               const FunctionDecl *functionDecl) const;
  void walkNewMethod(const ClassDecl *classDecl, FunctionDecl *functionDecl,
                     llvm::ArrayRef<FunctionDecl *> inherited);
  void walkHookParams(const FunctionDecl *functionDecl, llvm::ArrayRef<FunctionDecl *> inherited);
  bool reportBadOverride(const ClassDecl *classDecl, const FunctionDecl *functionDecl,
                         const FunctionDecl *base);
  void walkOverrideChain(const ClassDecl *classDecl, const FunctionDecl *functionDecl,
                         FunctionDecl *base);

  SemanticState &state;
  ChunkTypes *chunkTypes = nullptr;
  ClassBases *classBases = nullptr;
  ClassDeclarer *classDeclarer = nullptr;
  GenericClassInstantiator *genericClassInstantiator = nullptr;
};

}
