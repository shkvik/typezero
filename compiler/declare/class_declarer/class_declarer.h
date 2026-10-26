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
  Checker &checker_;
  llvm::SmallPtrSet<const ClassDecl *, 2> completing_;
};

}
