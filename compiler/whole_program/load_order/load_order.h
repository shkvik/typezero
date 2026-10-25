#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// Reads of a file's globals before the file is loaded.
class LoadOrder {
public:
  explicit LoadOrder(Checker &checker);

  // Reads of variables before the code that sets them runs: top-level code
  // calls a function that uses a global declared later, in its file or in a
  // file that loads later.
  void checkLoadOrder(SourceFile &program);

private:
  Checker &checker_;
};

} // namespace tz
