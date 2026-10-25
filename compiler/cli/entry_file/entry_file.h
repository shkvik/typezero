#pragma once

namespace tz {

class Args;

// Without a file, `tz build` and `tz run` build the "main" of the project around the current
// directory.
class EntryFile {
public:
  // Returns -1 when the input is known, else the code tz should exit with.
  int find(Args &args) const;
};

} // namespace tz
