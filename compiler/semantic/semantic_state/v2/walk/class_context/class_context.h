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
  void leaveClass();
  void enterStaticInit();
  void leaveStaticInit();
  void forgetSetStatics();
  void forgetThisMember();
  void leaveInitializer();
  void leaveInitializerClosure();
  void enterClass(ClassDecl *classDecl);
  void markStaticSet(const VarSymbol *variable);
  void rememberThisMember(llvm::StringRef name);
  void enterInitializerClosure(bool isThisReady);
  void enterInitializer(FunctionDecl *functionDecl, const VarSymbol *self);


  bool isClosureThisReady() const;
  bool isInsideStaticInit() const;
  bool isStaticSet(const VarSymbol *variable) const;


  ClassDecl *getCurrentClass() const;
  FunctionDecl *getInitializer() const;
  llvm::StringRef getThisMember() const;
  const VarSymbol *getInitializerThis() const;

private:
  int staticInitDepth = 0;
  llvm::StringRef thisMember;
  std::vector<ClassDecl *> classes;
  std::vector<bool> closureThisReady;
  std::vector<InitializerFrame> initializers;
  llvm::SmallPtrSet<const VarSymbol *, 4> setStatics;
};

}
