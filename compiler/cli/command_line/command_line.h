#pragma once

namespace tz {

// The `tz` command line tool: picks the command by the first word and runs it.
class CommandLine {
public:
  int run(int argc, char **argv) const;
};

} // namespace tz
