#pragma once

#include "compiler/program/errors/errors.h"

namespace tz {

class SemanticExternalFacade;

class InferPass {
public:
  explicit InferPass(SemanticExternalFacade &owner);

  Errors run() const;

private:
  SemanticExternalFacade &engine;
};

}
