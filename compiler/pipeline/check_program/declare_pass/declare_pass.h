#pragma once

#include "compiler/program/errors/errors.h"

namespace tz {

class SemanticExternalFacade;
struct Program;

class DeclarePass {
public:
  DeclarePass(SemanticExternalFacade &owner, Program &checked);

  Errors run() const;

private:
  SemanticExternalFacade &engine;
  Program &program;
};

}
