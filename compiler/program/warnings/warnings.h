#pragma once

#include "compiler/program/diagnostic/diagnostic.h"
#include <vector>

namespace tz {

class Warnings {
public:
  void add(Diagnostic diagnostic);

  std::vector<Diagnostic>::const_iterator begin() const;
  std::vector<Diagnostic>::const_iterator end() const;

private:
  std::vector<Diagnostic> list;
};

}
