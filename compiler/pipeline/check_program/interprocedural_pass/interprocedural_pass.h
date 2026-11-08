#pragma once

#include "compiler/program/errors/errors.h"

namespace tz {

class SemanticExternalFacade;
struct Program;

class InterproceduralPass {
public:
  InterproceduralPass(SemanticExternalFacade &owner, Program &checked);

  void checkLoadOrder() const;
  Errors checkValueFlows() const;

private:
  SemanticExternalFacade &engine;
  Program &program;
};

}
