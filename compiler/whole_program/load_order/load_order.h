#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// Reads of a file's globals before the file is loaded.
class LoadOrder {
public:
  explicit LoadOrder(Sema &sema);

  // Reads of variables before the code that sets them runs: top-level code
  // calls a function that uses a global declared later, in its file or in a
  // file that loads later.
  void checkLoadOrder(SourceFile &program);

private:
  Sema &sema_;
};

} // namespace tz
