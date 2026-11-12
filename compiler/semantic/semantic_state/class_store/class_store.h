#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/types/types.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringRef.h"
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace tz {

class ClassStore {
public:
  unsigned takeClassId();
  void rememberClassCount(unsigned count);
  void addClass(ClassDecl *classDecl);
  std::vector<ClassDecl *> getClasses() const;
  void markClassesLinked();
  bool hasLinkedClasses() const;
  void markMethodsDeclared();
  bool hasDeclaredMethods() const;
  void rememberReturnSource(const FunctionDecl *functionDecl, FunctionDecl *source);
  bool hasReturnSource(const FunctionDecl *functionDecl) const;
  FunctionDecl *findReturnSource(const FunctionDecl *functionDecl) const;
  void rememberInheritedReturn(const FunctionDecl *functionDecl, FunctionDecl *base);
  FunctionDecl *findInheritedReturn(const FunctionDecl *functionDecl) const;
  void markRootSuper(const CallExpr *call);
  bool isRootSuper(const CallExpr *call) const;
  void markBrokenClass(const ClassDecl *classDecl);
  bool isBrokenClass(const ClassDecl *classDecl) const;
  void enterCompletion(const ClassDecl *classDecl);
  void leaveCompletion(const ClassDecl *classDecl);
  bool isCompleting(const ClassDecl *classDecl) const;
  void rememberSignatureVariants(const Type *interfaceType, llvm::StringRef name,
                                 std::vector<FunctionDecl *> variants);
  std::vector<FunctionDecl *> getSignatureVariants(const Type *interfaceType,
                                                   llvm::StringRef name) const;
  void markCallReported(const CallExpr *call);
  bool isCallReported(const CallExpr *call) const;

private:
  unsigned classCount = 0;
  std::vector<ClassDecl *> classes;
  bool classesLinked = false;
  bool methodsDeclared = false;
  llvm::DenseMap<const FunctionDecl *, FunctionDecl *> returnSources;
  llvm::DenseMap<const FunctionDecl *, FunctionDecl *> inheritedReturns;
  llvm::SmallPtrSet<const CallExpr *, 4> rootSupers;
  llvm::SmallPtrSet<const ClassDecl *, 2> brokenClasses;
  llvm::SmallPtrSet<const ClassDecl *, 2> completing;
  std::map<std::pair<const Type *, std::string>, std::vector<FunctionDecl *>> signatureVariants;
  llvm::SmallPtrSet<const CallExpr *, 4> reportedCalls;
};

}
