#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/declaration/names/declaration_names_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <vector>

namespace tz {

class DeclarationNamesRegistry {
public:
  using ResultNameIsVisible = Result<const VarSymbol *, DeclarationNamesRegistryCheckVisibleError>;

  explicit DeclarationNamesRegistry(SemanticState &owner);

  void openScope();
  void closeScope();
  void declareVariable(const DeclareVariable &task);
  void rememberNamesInFunctions(SourceFile &program);
  void declareVariableSlot(const DeclareVariableSlot &task);


  VarSymbol *declareLet(llvm::StringRef name, llvm::SMLoc loc, const Type *type);
  VarSymbol *declareConst(llvm::StringRef name, llvm::SMLoc loc, const Type *type);
  VarSymbol *declareVar(llvm::StringRef name, llvm::SMLoc loc, const Type *type, bool isConst);


  ResultNameIsVisible checkVisible(NameExpr *name, const VarSymbol *variable) const;

private:
  void placeVariable(VarSymbol *variable);
  void notePreviousDeclaration(llvm::StringRef name);
  void reportDeclareError(llvm::SMLoc loc, llvm::StringRef name,
                          const ScopeStackContextDeclareError &error);


  SemanticState &state;
};

}
