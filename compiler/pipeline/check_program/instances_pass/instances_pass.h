#pragma once

#include "compiler/program/errors/errors.h"

namespace tz {

class SemanticExternalFacade;

class InstancesPass {
public:
  explicit InstancesPass(SemanticExternalFacade &owner);

  Errors run() const;

private:
  SemanticExternalFacade &engine;
};

}
