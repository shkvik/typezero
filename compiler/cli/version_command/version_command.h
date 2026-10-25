#pragma once

namespace tz {

// `tz version`: the version of tz and of the LLVM it is built on.
class VersionCommand {
public:
  int run() const;
};

} // namespace tz
