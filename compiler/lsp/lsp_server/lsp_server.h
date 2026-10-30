#pragma once

#include "compiler/knowledge/knowledge/knowledge.h"
#include "compiler/lsp/check_process/check_process.h"
#include "compiler/lsp/lsp_channel/lsp_channel.h"
#include "compiler/lsp/text_document/text_document.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/JSON.h"
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>

namespace tz {

class LspServer {
public:
  LspServer(LspChannel &channel, std::string self);
  ~LspServer();

  int run();

private:
  using Clock = std::chrono::steady_clock;

  struct Entry {
    std::string key;
    std::string path;
    bool dirty = false;
    Clock::time_point due;
    std::set<std::string> files;
    std::map<std::string, llvm::json::Array> published;
    Knowledge knowledge;
    int level = 1;
    std::map<std::string, std::set<std::string>> quick;
    std::map<std::string, llvm::json::Array> later;
  };

  bool handle(const llvm::json::Object &message);
  bool answer(llvm::StringRef method, const llvm::json::Value &id, const llvm::json::Object &args);
  void reply(const llvm::json::Value &id, llvm::json::Value result);
  void fail(const llvm::json::Value &id, int code, const llvm::Twine &message);
  void notify(llvm::StringRef method, llvm::json::Value params);
  void log(const llvm::Twine &message);

  void opened(const llvm::json::Object &params);
  void changed(const llvm::json::Object &params);
  void closed(const llvm::json::Object &params);
  void watched(const llvm::json::Object &params);

  std::string entryOf(const TextDocument &document) const;
  void assign(TextDocument &document, std::chrono::milliseconds delay);
  void touch(const std::string &file, std::chrono::milliseconds delay);
  void schedule(Entry &entry, std::chrono::milliseconds delay);
  void forget(const std::string &key);
  bool used(const std::string &key) const;
  std::string uriFor(llvm::StringRef path) const;

  void work();
  void start(Entry &entry);
  void finish(int code);
  void publish(Entry &entry, std::map<std::string, llvm::json::Array> next);
  llvm::json::Value diagnostic(const llvm::json::Object &found) const;

  LspChannel &channel_;
  std::string self_;
  std::string request_;
  std::string response_;
  std::string log_;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::map<std::string, std::unique_ptr<TextDocument>> documents_;
  std::map<std::string, Entry> entries_;
  CheckProcess process_;
  std::string running_;
  int runningLevel_ = 1;
  bool refreshTokens_ = false;
  int nextRequest_ = 1;
  bool checking_ = false;
  bool cancel_ = false;
  bool stopping_ = false;
  bool shutdown_ = false;
  Clock::time_point started_;
};

}
