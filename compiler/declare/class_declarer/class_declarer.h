#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class ClassDeclarer {
public:
  explicit ClassDeclarer(Checker &checker);

  void declareClass(ClassDecl *cls);
  void resolveClass(ClassDecl *cls);
  void completeClass(ClassDecl *cls);
  void declareMethods(ClassDecl *cls);
  void declareTyped(FunctionDecl *fn, const Type *declared);
  void checkVariants(ClassDecl *cls);
  void checkVariantPair(FunctionDecl *a, FunctionDecl *b);
  static ClassDecl *classOf(const Type *type);
  static std::vector<FunctionDecl *> methodsNamed(const ClassDecl *cls, llvm::StringRef name,
                                                  bool isStatic);

private:
  Expr *defaultValue(const Type *type, const TypeRef *ref, llvm::SMLoc loc,
                     llvm::SmallPtrSetImpl<const ClassDecl *> &visiting);
  Expr *literalValue(const Type *literal, llvm::SMLoc loc);
  bool startsEmpty(const ClassDecl *cls, llvm::SmallPtrSetImpl<const ClassDecl *> &visiting);

  Checker &checker_;
  llvm::SmallPtrSet<const ClassDecl *, 2> completing_;
};

}
