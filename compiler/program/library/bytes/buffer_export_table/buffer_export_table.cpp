#include "compiler/program/library/bytes/buffer_export_table/buffer_export_table.h"

namespace tz {

namespace {

const BufferExportInfo kExports[] = {
    {.name = "Buffer", .kind = BufferExport::BUFFER},
    {.name = "Blob", .kind = BufferExport::NOT_YET},
    {.name = "File", .kind = BufferExport::NOT_YET},
    {.name = "SlowBuffer", .kind = BufferExport::NOT_YET},
    {.name = "atob", .kind = BufferExport::NOT_YET},
    {.name = "btoa", .kind = BufferExport::NOT_YET},
    {.name = "constants", .kind = BufferExport::NOT_YET},
    {.name = "isAscii", .kind = BufferExport::NOT_YET},
    {.name = "isUtf8", .kind = BufferExport::NOT_YET},
    {.name = "kMaxLength", .kind = BufferExport::NOT_YET},
    {.name = "kStringMaxLength", .kind = BufferExport::NOT_YET},
    {.name = "resolveObjectURL", .kind = BufferExport::NOT_YET},
    {.name = "transcode", .kind = BufferExport::NOT_YET},
    {.name = "INSPECT_MAX_BYTES", .kind = BufferExport::NOT_YET},
};

}

std::optional<BufferExport> BufferExportTable::find(llvm::StringRef name) {
  for (const BufferExportInfo &info : kExports) {
    if (info.name == name) {
      return info.kind;
    }
  }

  return std::nullopt;
}

}
