#pragma once

#include "compiler/program/ast/ast.h"
#include "llvm/ADT/SmallPtrSet.h"
#include <vector>

namespace tz {

class ClassListContext {
public:
  void markClassesLinked();
  void markMethodsDeclared();
  void addClass(ClassDecl *classDecl);
  void rememberClassCount(unsigned count);
  void enterCompletion(const ClassDecl *classDecl);
  void leaveCompletion(const ClassDecl *classDecl);


  bool hasLinkedClasses() const;
  bool hasDeclaredMethods() const;
  bool isCompleting(const ClassDecl *classDecl) const;


  unsigned takeClassId();
  std::vector<ClassDecl *> getClasses() const;

private:
  unsigned classCount = 0;
  bool classesLinked = false;
  bool methodsDeclared = false;
  std::vector<ClassDecl *> classes;
  llvm::SmallPtrSet<const ClassDecl *, 2> completing;
};

}
