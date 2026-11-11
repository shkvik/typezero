#include "compiler/program/library/process/process_member_table/process_member_table.h"

namespace tz {

namespace {

const ProcessMemberInfo kRows[] = {
    {
        .name = "nextTick",
        .value = ProcessMember::NEXT_TICK,
    },
    {
        .name = "exit",
        .value = ProcessMember::EXIT,
    },
    {
        .name = "cwd",
        .value = ProcessMember::CWD,
    },
    {
        .name = "uptime",
        .value = ProcessMember::UPTIME,
    },
    {
        .name = "on",
        .value = ProcessMember::ON,
    },
    {
        .name = "once",
        .value = ProcessMember::ONCE,
    },
    {
        .name = "off",
        .value = ProcessMember::OFF,
    },
    {
        .name = "addListener",
        .value = ProcessMember::ADD_LISTENER,
    },
    {
        .name = "prependListener",
        .value = ProcessMember::PREPEND_LISTENER,
    },
    {
        .name = "argv",
        .value = ProcessMember::ARGV,
    },
    {
        .name = "env",
        .value = ProcessMember::ENV,
    },
    {
        .name = "exitCode",
        .value = ProcessMember::EXIT_CODE,
    },
    {
        .name = "execPath",
        .value = ProcessMember::EXEC_PATH,
    },
    {
        .name = "platform",
        .value = ProcessMember::PLATFORM,
    },
    {
        .name = "pid",
        .value = ProcessMember::PID,
    },
    {
        .name = "stdin",
        .value = ProcessMember::STDIN,
    },
    {
        .name = "stdout",
        .value = ProcessMember::STDOUT,
    },
    {
        .name = "stderr",
        .value = ProcessMember::STDERR,
    },
};

}

std::optional<ProcessMember> ProcessMemberTable::find(llvm::StringRef name) {
  for (const ProcessMemberInfo &info : kRows) {
    if (info.name == name) {
      return info.value;
    }
  }

  return std::nullopt;
}

}
