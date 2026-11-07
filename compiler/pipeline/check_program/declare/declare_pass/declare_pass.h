#pragma once

#include "compiler/program/errors/errors.h"

namespace tz {

class CheckState;
struct SourceFile;

class DeclarePass {
public:
  DeclarePass(CheckState &owner, SourceFile &program);

  Errors run() const;

private:
  CheckState &state;
  SourceFile &file;
};

}
