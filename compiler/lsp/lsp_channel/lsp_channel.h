#pragma once

#include "llvm/Support/JSON.h"
#include <mutex>
#include <optional>
#include <string>

namespace tz {

class LspChannel {
public:
  LspChannel();

  std::optional<std::string> read();
  void send(const llvm::json::Value &message);

private:
  std::mutex mutex_;
};

}
