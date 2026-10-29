#pragma once

#include "compiler/knowledge/knowledge/knowledge.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/JSON.h"
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace tz {

class McpServer {
public:
  explicit McpServer(std::string self);

  int run();

private:
  std::optional<std::string> readLine();
  void send(const llvm::json::Value &message);
  std::optional<llvm::json::Value> handle(const llvm::json::Object &message);
  llvm::json::Value tools() const;
  std::string call(llvm::StringRef name, const llvm::json::Object &args, bool &failed);
  std::string runTz(const std::vector<std::string> &args, bool &failed);
  std::string checkEdit(const llvm::json::Object &args, bool &failed);
  std::string languageCard(bool &failed) const;
  std::string shown(llvm::StringRef path) const;
  const Knowledge *knowledgeFor(const std::string &entry, bool &failed, std::string &message);

  struct Cached {
    Knowledge knowledge;
    std::vector<std::pair<std::string, std::pair<int64_t, uint64_t>>> stamps;
  };

  std::string self_;
  std::string here_;
  std::map<std::string, Cached> cache_;
};

}
