#pragma once

#include "compiler/program/errors/errors.h"
#include "compiler/program/pass_done/pass_done.h"
#include "compiler/result/result/result.h"

namespace tz {

class SemanticExternalFacade;

class CheckPass {
public:
  using ResultChecked = Result<PassDone, Errors>;

  explicit CheckPass(SemanticExternalFacade &owner);

  ResultChecked run() const;

private:
  SemanticExternalFacade &engine;
};

}
