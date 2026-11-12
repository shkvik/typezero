#include "compiler/program/library/child_process/child_stdio_table/child_stdio_table.h"

namespace tz {

namespace {

const ChildStdioInfo kStdio[] = {
    {.name = "pipe", .stdio = ChildStdio::PIPE},
    {.name = "overlapped", .stdio = ChildStdio::PIPE},
    {.name = "inherit", .stdio = ChildStdio::INHERIT},
    {.name = "ignore", .stdio = ChildStdio::IGNORED},
    {.name = "ipc", .stdio = ChildStdio::IPC},
};

}

std::optional<ChildStdio> ChildStdioTable::find(llvm::StringRef name) {
  for (const ChildStdioInfo &info : kStdio) {
    if (info.name == name) {
      return info.stdio;
    }
  }

  return std::nullopt;
}

}
