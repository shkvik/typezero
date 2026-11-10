#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/diagnostics/diagnostics.h"
#include "compiler/program/source_file/source_file.h"
#include <memory>

namespace tz {

class SemanticParts;
class SemanticState;
struct Program;

class SemanticExternalFacade {
public:
  explicit SemanticExternalFacade(Program &program);
  ~SemanticExternalFacade();

  Diagnostics::ResultTaken collectDeclarations();
  Diagnostics::ResultTaken findRecursion();
  Diagnostics::ResultTaken checkModuleCode();
  Diagnostics::ResultTaken checkGenericCopies();
  Diagnostics::ResultTaken finishProgram();

private:
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
