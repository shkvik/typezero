#pragma once

namespace tz {

// The `tz` command line tool: `tz build`, `tz run`, `tz version`, and the
// commands for projects and packages (pkg/commands.h).
int runDriver(int argc, char **argv);

} // namespace tz
