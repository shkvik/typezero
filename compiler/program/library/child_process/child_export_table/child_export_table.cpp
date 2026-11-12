#include "compiler/program/library/child_process/child_export_table/child_export_table.h"

namespace tz {

namespace {

const ChildExportInfo kExports[] = {
    {.name = "spawnSync", .kind = ChildExport::SYNC_FUNCTION},
    {.name = "execFileSync", .kind = ChildExport::SYNC_FUNCTION},
    {.name = "execSync", .kind = ChildExport::SYNC_FUNCTION},
    {.name = "spawn", .kind = ChildExport::ASYNC_FUNCTION},
    {.name = "exec", .kind = ChildExport::ASYNC_FUNCTION},
    {.name = "execFile", .kind = ChildExport::ASYNC_FUNCTION},
    {.name = "fork", .kind = ChildExport::FORK},
    {.name = "ChildProcess", .kind = ChildExport::NOT_YET},
    {.name = "_forkChild", .kind = ChildExport::NOT_YET},
};

}

std::optional<ChildExport> ChildExportTable::find(llvm::StringRef name) {
  for (const ChildExportInfo &info : kExports) {
    if (info.name == name) {
      return info.kind;
    }
  }

  return std::nullopt;
}

}
