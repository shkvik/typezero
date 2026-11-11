#include "compiler/program/library/os/os_function_table/os_function_table.h"

namespace tz {

namespace {

const OsFunctionInfo kRows[] = {
    {
        .name = "homedir",
        .value = OsFunction::HOMEDIR,
    },
    {
        .name = "tmpdir",
        .value = OsFunction::TMPDIR,
    },
    {
        .name = "hostname",
        .value = OsFunction::HOSTNAME,
    },
    {
        .name = "arch",
        .value = OsFunction::ARCH,
    },
    {
        .name = "type",
        .value = OsFunction::TYPE,
    },
    {
        .name = "release",
        .value = OsFunction::RELEASE,
    },
    {
        .name = "version",
        .value = OsFunction::VERSION,
    },
    {
        .name = "machine",
        .value = OsFunction::MACHINE,
    },
    {
        .name = "totalmem",
        .value = OsFunction::TOTALMEM,
    },
    {
        .name = "freemem",
        .value = OsFunction::FREEMEM,
    },
    {
        .name = "uptime",
        .value = OsFunction::UPTIME,
    },
    {
        .name = "platform",
        .value = OsFunction::PLATFORM,
    },
    {
        .name = "endianness",
        .value = OsFunction::ENDIANNESS,
    },
    {
        .name = "availableParallelism",
        .value = OsFunction::AVAILABLE_PARALLELISM,
    },
    {
        .name = "cpus",
        .value = OsFunction::CPUS,
    },
    {
        .name = "loadavg",
        .value = OsFunction::LOADAVG,
    },
    {
        .name = "userInfo",
        .value = OsFunction::USER_INFO,
    },
};

}

std::optional<OsFunction> OsFunctionTable::find(llvm::StringRef name) {
  for (const OsFunctionInfo &info : kRows) {
    if (info.name == name) {
      return info.value;
    }
  }

  return std::nullopt;
}

}
