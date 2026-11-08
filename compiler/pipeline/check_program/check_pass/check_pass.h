#pragma once

#include "compiler/semantic/semantic_external.h"

namespace tz {

class CheckPass {
public:
  using ResultChecked = SemanticExternalFacade::ResultBodiesChecked;

  explicit CheckPass(SemanticExternalFacade &owner);

  ResultChecked run() const;

private:
  SemanticExternalFacade &engine;
};

}
