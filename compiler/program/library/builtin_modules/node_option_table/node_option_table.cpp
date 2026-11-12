#include "compiler/program/library/builtin_modules/node_option_table/node_option_table.h"
#include "llvm/Support/ErrorHandling.h"

namespace tz {

namespace {

const NodeOptionInfo kRows[] = {
    {
        .name = "encoding",
        .value = NodeOption::ENCODING,
    },
    {
        .name = "flag",
        .value = NodeOption::FLAG,
    },
    {
        .name = "mode",
        .value = NodeOption::MODE,
    },
    {
        .name = "recursive",
        .value = NodeOption::RECURSIVE,
    },
    {
        .name = "force",
        .value = NodeOption::FORCE,
    },
    {
        .name = "withFileTypes",
        .value = NodeOption::WITH_FILE_TYPES,
    },
    {
        .name = "throwIfNoEntry",
        .value = NodeOption::THROW_IF_NO_ENTRY,
    },
    {
        .name = "bigint",
        .value = NodeOption::BIGINT,
    },
    {
        .name = "maxRetries",
        .value = NodeOption::MAX_RETRIES,
    },
    {
        .name = "retryDelay",
        .value = NodeOption::RETRY_DELAY,
    },
    {
        .name = "input",
        .value = NodeOption::INPUT,
    },
    {
        .name = "output",
        .value = NodeOption::OUTPUT,
    },
    {
        .name = "terminal",
        .value = NodeOption::TERMINAL,
    },
    {
        .name = "crlfDelay",
        .value = NodeOption::CRLF_DELAY,
    },
    {
        .name = "historySize",
        .value = NodeOption::HISTORY_SIZE,
    },
    {
        .name = "removeHistoryDuplicates",
        .value = NodeOption::REMOVE_HISTORY_DUPLICATES,
    },
    {
        .name = "prompt",
        .value = NodeOption::PROMPT,
    },
    {
        .name = "tabSize",
        .value = NodeOption::TAB_SIZE,
    },
    {
        .name = "completer",
        .value = NodeOption::COMPLETER,
    },
    {
        .name = "escapeCodeTimeout",
        .value = NodeOption::ESCAPE_CODE_TIMEOUT,
    },
    {
        .name = "history",
        .value = NodeOption::HISTORY,
    },
    {
        .name = "ref",
        .value = NodeOption::REF,
    },
    {
        .name = "cwd",
        .value = NodeOption::WORKING_DIRECTORY,
    },
    {
        .name = "env",
        .value = NodeOption::ENVIRONMENT,
    },
    {
        .name = "stdio",
        .value = NodeOption::STDIO,
    },
    {
        .name = "timeout",
        .value = NodeOption::TIMEOUT,
    },
    {
        .name = "killSignal",
        .value = NodeOption::KILL_SIGNAL,
    },
    {
        .name = "maxBuffer",
        .value = NodeOption::MAX_BUFFER,
    },
    {
        .name = "shell",
        .value = NodeOption::SHELL,
    },
    {
        .name = "windowsHide",
        .value = NodeOption::WINDOWS_HIDE,
    },
    {
        .name = "windowsVerbatimArguments",
        .value = NodeOption::WINDOWS_VERBATIM_ARGUMENTS,
    },
    {
        .name = "detached",
        .value = NodeOption::DETACHED,
    },
    {
        .name = "argv0",
        .value = NodeOption::ARGV0,
    },
    {
        .name = "uid",
        .value = NodeOption::USER_ID,
    },
    {
        .name = "gid",
        .value = NodeOption::GROUP_ID,
    },
    {
        .name = "flags",
        .value = NodeOption::FLAGS,
    },
    {
        .name = "end",
        .value = NodeOption::END,
    },
};

}

std::optional<NodeOption> NodeOptionTable::find(llvm::StringRef name) {
  for (const NodeOptionInfo &info : kRows) {
    if (info.name == name) {
      return info.value;
    }
  }

  return std::nullopt;
}

llvm::StringRef NodeOptionTable::getName(NodeOption value) {
  for (const NodeOptionInfo &info : kRows) {
    if (info.value == value) {
      return info.name;
    }
  }
  llvm_unreachable("node option missing from the table");
}

}
