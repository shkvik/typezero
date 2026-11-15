#pragma once

#include "compiler/program/ast/ast.h"

namespace tz {

class FileSystemContext {
public:
  void enterPromiseCall();
  void leavePromiseCall();
  void rememberStatsClass(ClassDecl *classDecl);
  void rememberDirentClass(ClassDecl *classDecl);


  bool isInsidePromiseCall() const;
  bool isFileClass(const ClassDecl *classDecl) const;
  bool isStatsClass(const ClassDecl *classDecl) const;


  ClassDecl *getStatsClass() const;
  ClassDecl *getDirentClass() const;

private:
  int promiseCallDepth = 0;
  ClassDecl *statsClass = nullptr;
  ClassDecl *direntClass = nullptr;
};

}
