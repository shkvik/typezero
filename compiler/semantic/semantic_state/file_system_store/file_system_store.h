#pragma once

#include "compiler/program/ast/ast.h"

namespace tz {

class FileSystemStore {
public:
  void rememberStatsClass(ClassDecl *classDecl);
  void rememberDirentClass(ClassDecl *classDecl);
  ClassDecl *getStatsClass() const;
  ClassDecl *getDirentClass() const;
  ClassDecl *findClassNamed(llvm::StringRef name) const;
  bool isStatsClass(const ClassDecl *classDecl) const;
  bool isFileClass(const ClassDecl *classDecl) const;
  void enterPromiseCall();
  void leavePromiseCall();
  bool isInsidePromiseCall() const;

private:
  ClassDecl *statsClass = nullptr;
  ClassDecl *direntClass = nullptr;
  int promiseCallDepth = 0;
};

}
