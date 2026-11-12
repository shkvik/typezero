#include "compiler/program/library/builtin_modules/pending_export_table/pending_export_table.h"

namespace tz {

namespace {

const PendingExportInfo kExports[] = {
    {.module = NodeModule::Os, .name = "EOL"},
    {.module = NodeModule::Os, .name = "devNull"},
    {.module = NodeModule::Os, .name = "getPriority"},
    {.module = NodeModule::Os, .name = "setPriority"},
    {.module = NodeModule::Os, .name = "networkInterfaces"},
    {.module = NodeModule::Os, .name = "constants"},
    {.module = NodeModule::Path, .name = "sep"},
    {.module = NodeModule::Path, .name = "delimiter"},
    {.module = NodeModule::Path, .name = "posix"},
    {.module = NodeModule::Path, .name = "win32"},
    {.module = NodeModule::Path, .name = "matchesGlob"},
    {.module = NodeModule::Path, .name = "_makeLong"},
};

}

bool PendingExportTable::isListed(NodeModule module, llvm::StringRef name) {
  for (const PendingExportInfo &info : kExports) {
    if (info.module == module && info.name == name) {
      return true;
    }
  }

  return false;
}

}
