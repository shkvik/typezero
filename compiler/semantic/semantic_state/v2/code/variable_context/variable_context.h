#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/types/types.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include <vector>

namespace tz {

struct Untyped {
  VarSymbol *var;
  bool initUndefined;
  std::vector<const Type *> assigned;
};

class VariableContext {
public:
  void enterModuleLoad();
  void leaveModuleLoad();
  void forgetResultVars();
  void leaveVariableInit();
  void addResultVar(VarSymbol *variable);
  void enterVariableInit(llvm::StringRef name);
  void forgetUntyped(const VarSymbol *variable);
  void addLiftedFunction(FunctionDecl *functionDecl);
  void addUntyped(VarSymbol *variable, bool initUndefined);
  void addUntypedAssignment(const VarSymbol *variable, const Type *type);


  bool hasUntyped() const;
  bool isInitializing(llvm::StringRef name) const;


  std::vector<Untyped> getUntyped() const;
  std::vector<VarSymbol *> getResultVars() const;
  std::vector<FunctionDecl *> getLiftedFunctions() const;

private:
  std::vector<Untyped> untyped;
  std::vector<VarSymbol *> resultVars;
  std::vector<FunctionDecl *> liftedFunctions;
  llvm::SmallVector<llvm::StringRef, 2> initializing;
  std::vector<llvm::SmallVector<llvm::StringRef, 2>> initializingBeforeLoads;
};

}
