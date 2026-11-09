#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/classes/class_links/class_links_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class ChunkTypes;
class ClassDeclarer;
class GenericClassInstantiator;

class ClassLinks {
public:
  ClassLinks(SemanticState &owner, SemanticParts &all);

  void numberClasses();
  void buildVtable(ClassDecl *cls);
  void linkClasses();
  void linkOverrides(ClassDecl *cls);
  static std::vector<FunctionDecl *> constructorsOf(const ClassDecl *cls);
  static std::vector<FunctionDecl *> ownMethods(const ClassDecl *cls);
  static bool overridesTransitively(const FunctionDecl *fn, const FunctionDecl *base);
  static FunctionDecl *soleImplementation(const ClassDecl *cls, FunctionDecl *variant);
  const Type *inheritedReturnType(FuncSymbol *fn);
  void noteInheritedReturn(const FunctionDecl *fn);
  static const Type *commonClass(const Type *a, const Type *b);

private:
  using ResultMethodNotField = Result<const FunctionDecl *, ClassLinksNotAFieldError>;
  using ResultOverrideTarget = Result<const FunctionDecl *, ClassLinksOverrideTargetError>;
  using ResultBaseIsNotArrow = Result<const FunctionDecl *, ClassLinksArrowOverrideError>;
  using ResultBaseNotPrivate = Result<const FunctionDecl *, ClassLinksBaseVisibleError>;
  using ResultAccessNotLower = Result<const FunctionDecl *, ClassLinksAccessKeptError>;
  using ResultBodyIsRetained = Result<const FunctionDecl *, ClassLinksBodyKeptError>;
  using ResultSameTypeParams = Result<const FunctionDecl *, ClassLinksSameTypeArgsError>;

  static ResultMethodNotField notAField(const ClassDecl *cls, const FunctionDecl *fn);
  static ResultOverrideTarget overrideTarget(const ClassDecl *cls, const FunctionDecl *fn,
                                             llvm::ArrayRef<FunctionDecl *> inherited);
  static ResultBaseIsNotArrow arrowOverride(const FunctionDecl *fn, const FunctionDecl *base);
  static ResultBaseNotPrivate baseVisible(const FunctionDecl *fn, const FunctionDecl *base);
  static ResultAccessNotLower accessKept(const FunctionDecl *fn, const FunctionDecl *base);
  static ResultBodyIsRetained bodyKept(const ClassDecl *cls, const FunctionDecl *fn,
                                       const FunctionDecl *base);
  ResultSameTypeParams sameTypeArgs(const ClassDecl *cls, const FunctionDecl *fn,
                                    const FunctionDecl *base) const;
  static bool sameParams(const FuncSymbol *a, const FuncSymbol *b);
  static bool takesParams(const Type *fnType, const FuncSymbol *sym);
  static const char *accessName(Access access);
  static bool isArrowMethod(const FunctionDecl *fn);
  static bool declaresReturn(const FunctionDecl *fn);
  static FunctionDecl *firstImplementation(const ClassDecl *cls, const FunctionDecl *fn);
  static void interfacesOf(const ClassDecl *cls, std::vector<const Type *> &out);

  SemanticState &state;
  ChunkTypes *chunkTypes = nullptr;
  ClassDeclarer *classDeclarer = nullptr;
  GenericClassInstantiator *genericClassInstantiator = nullptr;
};

}
