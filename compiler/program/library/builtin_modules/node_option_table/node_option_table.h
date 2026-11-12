#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class NodeOption {
  ENCODING,
  FLAG,
  MODE,
  RECURSIVE,
  FORCE,
  WITH_FILE_TYPES,
  THROW_IF_NO_ENTRY,
  BIGINT,
  MAX_RETRIES,
  RETRY_DELAY,
  INPUT,
  OUTPUT,
  TERMINAL,
  CRLF_DELAY,
  HISTORY_SIZE,
  REMOVE_HISTORY_DUPLICATES,
  PROMPT,
  TAB_SIZE,
  COMPLETER,
  ESCAPE_CODE_TIMEOUT,
  HISTORY,
  REF,
  WORKING_DIRECTORY,
  ENVIRONMENT,
  STDIO,
  TIMEOUT,
  KILL_SIGNAL,
  MAX_BUFFER,
  SHELL,
  WINDOWS_HIDE,
  WINDOWS_VERBATIM_ARGUMENTS,
  DETACHED,
  ARGV0,
  USER_ID,
  GROUP_ID,
  FLAGS,
  END,
};

struct NodeOptionInfo {
  llvm::StringLiteral name;
  NodeOption value;
};

class NodeOptionTable {
public:
  static std::optional<NodeOption> find(llvm::StringRef name);
  static llvm::StringRef getName(NodeOption value);
};

}
