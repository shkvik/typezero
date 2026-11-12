#pragma once

#include "compiler/program/ast/ast.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringRef.h"
#include <vector>

namespace tz {

struct InitializerFrame {
  FunctionDecl *fn = nullptr;
  const VarSymbol *self = nullptr;
};

class ClassContext {
public:
  void enterClass(ClassDecl *classDecl);
  void leaveClass();
  ClassDecl *getCurrentClass() const;
  void enterInitializer(FunctionDecl *functionDecl, const VarSymbol *self);
  void leaveInitializer();
  FunctionDecl *getInitializer() const;
  const VarSymbol *getInitializerThis() const;
  void enterInitializerClosure(bool isThisReady);
  void leaveInitializerClosure();
  bool isClosureThisReady() const;
  void enterStaticInit();
  void leaveStaticInit();
  bool isInsideStaticInit() const;
  void forgetSetStatics();
  void markStaticSet(const VarSymbol *variable);
  bool isStaticSet(const VarSymbol *variable) const;
  void rememberThisMember(llvm::StringRef name);
  void forgetThisMember();
  llvm::StringRef getThisMember() const;

private:
  std::vector<ClassDecl *> classes;
  std::vector<InitializerFrame> initializers;
  std::vector<bool> closureThisReady;
  int staticInitDepth = 0;
  llvm::SmallPtrSet<const VarSymbol *, 4> setStatics;
  llvm::StringRef thisMember;
};

}
