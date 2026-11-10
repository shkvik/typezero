#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/diagnostics/diagnostics.h"
#include "compiler/program/errors/errors.h"
#include "compiler/program/program_inventory/program_inventory.h"
#include "compiler/program/source_file/source_file.h"
#include "compiler/semantic/semantic_state/widening_store/widening_store.h"
#include "llvm/Support/SMLoc.h"
#include <memory>

namespace tz {

class SemanticParts;
class SemanticState;

class SemanticExternalFacade {
public:
  SemanticExternalFacade(AstContext &ast, Diagnostics &diagnostics, SourceFile &loaded);
  ~SemanticExternalFacade();

  Errors walkBodies();
  void finishProgram();
  Errors walkInstances();
  Errors findCallCycles();
  Errors declareProgram();
  void collectInventory();
  ProgramInventory takeInventory();
  bool isInsideLibrary(llvm::SMLoc loc) const;
  const WideningStore &getWideningStore() const;

private:
  Errors takeErrors();
  void walkModuleItems();
  void walkLiftedFunctions();
  void walkDeclarationFiles();
  void walkClass(ClassDecl *cls);
  void walkFunction(FunctionDecl *fn);
  void walkLiftedFunction(FunctionDecl *fn);

  SourceFile &file;
  std::unique_ptr<SemanticState> state;
  std::unique_ptr<SemanticParts> parts;
};

}
