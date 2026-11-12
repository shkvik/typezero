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
  bool overridesTransitively(const FunctionDecl *functionDecl, const FunctionDecl *base) const;
  FunctionDecl *soleImplementation(const ClassDecl *classDecl, FunctionDecl *variant) const;
  const Type *inheritedReturnType(FuncSymbol *function);
  void noteInheritedReturn(const FunctionDecl *functionDecl);
  static const Type *commonClass(const Type *a, const Type *b);

private:
  using ResultMethodNotField = Result<const FunctionDecl *, ClassLinksNotAFieldError>;
  using ResultOverrideTarget = Result<const FunctionDecl *, ClassLinksOverrideTargetError>;
  using ResultBaseIsNotArrow = Result<const FunctionDecl *, ClassLinksArrowOverrideError>;
  using ResultBaseNotPrivate = Result<const FunctionDecl *, ClassLinksBaseVisibleError>;
  using ResultAccessNotLower = Result<const FunctionDecl *, ClassLinksAccessKeptError>;
  using ResultBodyIsRetained = Result<const FunctionDecl *, ClassLinksBodyKeptError>;
  using ResultSameTypeParams = Result<const FunctionDecl *, ClassLinksSameTypeArgsError>;

  void linkOverride(ClassDecl *classDecl, FunctionDecl *functionDecl);
  FunctionDecl *sameParamsBase(llvm::ArrayRef<FunctionDecl *> inherited,
                               const FunctionDecl *functionDecl) const;
  void walkNewMethod(const ClassDecl *classDecl, FunctionDecl *functionDecl,
                     llvm::ArrayRef<FunctionDecl *> inherited);
  bool overrideFits(const ClassDecl *classDecl, const FunctionDecl *functionDecl,
                    const FunctionDecl *base);
  void walkOverrideChain(const ClassDecl *classDecl, const FunctionDecl *functionDecl,
                         FunctionDecl *base);
  ResultMethodNotField notAField(const ClassDecl *classDecl,
                                 const FunctionDecl *functionDecl) const;
  ResultOverrideTarget overrideTarget(const ClassDecl *classDecl, const FunctionDecl *functionDecl,
                                      llvm::ArrayRef<FunctionDecl *> inherited) const;
  ResultBaseIsNotArrow arrowOverride(const FunctionDecl *functionDecl,
                                     const FunctionDecl *base) const;
  ResultBaseNotPrivate baseVisible(const FunctionDecl *functionDecl,
                                   const FunctionDecl *base) const;
  ResultAccessNotLower accessKept(const FunctionDecl *functionDecl, const FunctionDecl *base) const;
  ResultBodyIsRetained bodyKept(const ClassDecl *classDecl, const FunctionDecl *functionDecl,
                                const FunctionDecl *base) const;
  ResultSameTypeParams sameTypeArgs(const ClassDecl *classDecl, const FunctionDecl *functionDecl,
                                    const FunctionDecl *base) const;
  bool sameParams(const FuncSymbol *a, const FuncSymbol *b) const;
  bool takesParams(const Type *functionType, const FuncSymbol *function) const;
  const char *accessName(Access access) const;
  bool isArrowMethod(const FunctionDecl *functionDecl) const;
  bool declaresReturn(const FunctionDecl *functionDecl) const;
  FunctionDecl *firstImplementation(const ClassDecl *classDecl,
                                    const FunctionDecl *functionDecl) const;
  void interfacesOf(const ClassDecl *classDecl, std::vector<const Type *> &out) const;

  SemanticState &state;
  ChunkTypes *chunkTypes = nullptr;
  ClassBases *classBases = nullptr;
  ClassDeclarer *classDeclarer = nullptr;
  GenericClassInstantiator *genericClassInstantiator = nullptr;
};

}
