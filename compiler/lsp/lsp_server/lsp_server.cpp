#include "compiler/lsp/lsp_server/lsp_server.h"
#include "compiler/knowledge/knowledge_json/knowledge_json.h"
#include "compiler/load/source_overlay/source_overlay.h"
#include "compiler/lsp/file_uri/file_uri.h"
#include "compiler/lsp/lsp_features/lsp_features.h"
#include "compiler/lsp/program_entry/program_entry.h"
#include "compiler/program/source_places/source_places.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <thread>
#include <utility>

namespace tz {

namespace {

namespace fs = llvm::sys::fs;

namespace path = llvm::sys::path;

using std::chrono::milliseconds;

constexpr milliseconds kOpenDelay(50);
constexpr milliseconds kTypingDelay(300);
constexpr milliseconds kPollDelay(10);

}

LspServer::LspServer(LspChannel &channel, std::string self)
    : channel(channel),
      self(std::move(self)) {
  this->request = LspServer::temporaryFile("tz-lsp-request", "json");
  this->response = LspServer::temporaryFile("tz-lsp-response", "json");
  log_ = LspServer::temporaryFile("tz-lsp-check", "log");
}

LspServer::~LspServer() {
  fs::remove(this->request);
  fs::remove(this->response);
  fs::remove(log_);
}

std::string LspServer::temporaryFile(llvm::StringRef prefix, llvm::StringRef suffix) {
  llvm::SmallString<256> file;
  if (fs::createTemporaryFile(prefix, suffix, file)) {
    return "";
  }

  return std::string(file);
}

std::string LspServer::textOf(const llvm::json::Value &value) {
  std::string text;
  llvm::raw_string_ostream stream(text);
  stream << value;

  return text;
}

llvm::json::Object LspServer::startRange() {
  llvm::json::Object start{{"line", 0}, {"character", 0}};
  llvm::json::Object end{{"line", 0}, {"character", 0}};

  return llvm::json::Object{{"start", std::move(start)}, {"end", std::move(end)}};
}

int LspServer::run() {
  std::thread worker([this] { work(); });
  for (;;) {
    std::optional<std::string> body = this->channel.read();
    if (!body) {
      break;
    }
    llvm::Expected<llvm::json::Value> message = llvm::json::parse(*body);
    if (!message) {
      llvm::consumeError(message.takeError());
      fail(nullptr, -32700, "the message is not JSON");
      continue;
    }
    const llvm::json::Object *object = message->getAsObject();
    if (object && !handle(*object)) {
      break;
    }
  }
  {
    std::lock_guard<std::mutex> lock(this->mutex);
    this->stopping = true;
  }
  this->wake.notify_all();
  worker.join();

  return this->shutdown ? 0 : 1;
}

bool LspServer::handle(const llvm::json::Object &message) {
  std::optional<llvm::StringRef> method = message.getString("method");
  const llvm::json::Value *id = message.get("id");
  if (!method) {
    return true;
  }
  const llvm::json::Object empty;
  const llvm::json::Object *params = message.getObject("params");
  const llvm::json::Object &args = params ? *params : empty;
  if (*method == "initialize") {
    llvm::json::Object sync{{"openClose", true}, {"change", 2}};
    llvm::json::Object completion{{"triggerCharacters", llvm::json::Array{"."}}};
    llvm::json::Object renames{{"prepareProvider", true}};
    llvm::json::Object tokens{{"legend", LspFeatures::legend()}, {"full", true}};
    const llvm::json::Object *client = args.getObject("capabilities");
    const llvm::json::Object *workspace = client ? client->getObject("workspace") : nullptr;
    const llvm::json::Object *semantic =
        workspace ? workspace->getObject("semanticTokens") : nullptr;
    this->refreshTokens = semantic && semantic->getBoolean("refreshSupport").value_or(false);
    llvm::json::Object capabilities{{"textDocumentSync", std::move(sync)},
                                    {"hoverProvider", true},
                                    {"definitionProvider", true},
                                    {"referencesProvider", true},
                                    {"completionProvider", std::move(completion)},
                                    {"renameProvider", std::move(renames)},
                                    {"semanticTokensProvider", std::move(tokens)}};
    llvm::json::Object info{{"name", "tz"}, {"version", TZ_VERSION}};
    reply(id ? *id : nullptr, llvm::json::Object{{"capabilities", std::move(capabilities)},
                                                 {"serverInfo", std::move(info)}});
    return true;
  }
  if (*method == "shutdown") {
    this->shutdown = true;
    reply(id ? *id : nullptr, nullptr);
    return true;
  }
  if (*method == "exit") {
    return false;
  }
  std::lock_guard<std::mutex> lock(this->mutex);
  if (*method == "textDocument/didOpen") {
    opened(args);
  } else if (*method == "textDocument/didChange") {
    changed(args);
  } else if (*method == "textDocument/didClose") {
    closed(args);
  } else if (*method == "workspace/didChangeWatchedFiles") {
    watched(args);
  } else if (id && answer(*method, *id, args)) {
    return true;
  } else if (id) {
    fail(*id, -32601, "tz does not answer '" + *method + "'");
  }
  this->wake.notify_all();

  return true;
}

bool LspServer::answer(llvm::StringRef method, const llvm::json::Value &id,
                       const llvm::json::Object &args) {
  const llvm::StringRef methods[] = {"textDocument/hover",
                                     "textDocument/definition",
                                     "textDocument/references",
                                     "textDocument/completion",
                                     "textDocument/prepareRename",
                                     "textDocument/rename",
                                     "textDocument/semanticTokens/full"};
  if (!llvm::is_contained(methods, method)) {
    return false;
  }
  const llvm::json::Object *item = args.getObject("textDocument");
  std::optional<llvm::StringRef> uri = item ? item->getString("uri") : std::nullopt;
  const llvm::json::Object *position = args.getObject("position");
  auto found = uri ? this->documents.find(uri->str()) : this->documents.end();
  const Knowledge *knowledge = nullptr;
  size_t module = 0;
  if (found != this->documents.end()) {
    const TextDocument &document = *found->second;
    auto own = this->entries.find(document.entry);
    if (own != this->entries.end()) {
      if (std::optional<size_t> at = own->second.knowledge.moduleAt(document.path())) {
        knowledge = &own->second.knowledge;
        module = *at;
      }
    }
    for (auto &[key, entry] : this->entries) {
      if (knowledge) {
        break;
      }
      if (std::optional<size_t> at = entry.knowledge.moduleAt(document.path())) {
        knowledge = &entry.knowledge;
        module = *at;
      }
    }
  }
  if (!knowledge && method == "textDocument/semanticTokens/full") {
    reply(id, llvm::json::Object{{"data", llvm::json::Array{}}});
    return true;
  }
  if (!knowledge) {
    if (method == "textDocument/completion") {
      reply(id, llvm::json::Object{{"isIncomplete", true}, {"items", llvm::json::Array{}}});
    } else {
      reply(id, nullptr);
    }
    return true;
  }
  unsigned line = position ? static_cast<unsigned>(position->getInteger("line").value_or(0)) : 0;
  unsigned character =
      position ? static_cast<unsigned>(position->getInteger("character").value_or(0)) : 0;
  auto uriOf = [this](llvm::StringRef file) { return uriFor(file); };
  auto textOf = [this](llvm::StringRef file) {
    std::string key = SourceOverlay::key(file);
    for (const auto &[open, document] : this->documents) {
      if (document->key() == key) {
        return document->text();
      }
    }
    auto buffer = llvm::MemoryBuffer::getFile(file);

    return buffer ? (*buffer)->getBuffer().str() : std::string();
  };
  LspFeatures features(*knowledge, module, found->second->text(), uriOf, textOf);
  if (method == "textDocument/semanticTokens/full") {
    reply(id, features.semanticTokens());
  } else if (method == "textDocument/hover") {
    reply(id, features.hover(line, character));
  } else if (method == "textDocument/definition") {
    reply(id, features.definition(line, character));
  } else if (method == "textDocument/references") {
    const llvm::json::Object *context = args.getObject("context");
    bool declaration = context && context->getBoolean("includeDeclaration").value_or(false);
    reply(id, features.references(line, character, declaration));
  } else if (method == "textDocument/completion") {
    reply(id, features.completion(line, character));
  } else if (method == "textDocument/prepareRename") {
    reply(id, features.prepareRename(line, character));
  } else {
    std::string error;
    llvm::json::Value edit =
        features.rename(line, character, args.getString("newName").value_or(""), error);
    if (error.empty()) {
      reply(id, std::move(edit));
    } else {
      fail(id, -32803, error);
    }
  }

  return true;
}

void LspServer::reply(const llvm::json::Value &id, llvm::json::Value result) {
  this->channel.send(
      llvm::json::Object{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}});
}

void LspServer::fail(const llvm::json::Value &id, int code, const llvm::Twine &message) {
  llvm::json::Object error{{"code", code}, {"message", message.str()}};
  this->channel.send(
      llvm::json::Object{{"jsonrpc", "2.0"}, {"id", id}, {"error", std::move(error)}});
}

void LspServer::notify(llvm::StringRef method, llvm::json::Value params) {
  this->channel.send(
      llvm::json::Object{{"jsonrpc", "2.0"}, {"method", method}, {"params", std::move(params)}});
}

void LspServer::log(const llvm::Twine &message) {
  notify("window/logMessage", llvm::json::Object{{"type", 4}, {"message", message.str()}});
}

void LspServer::opened(const llvm::json::Object &params) {
  const llvm::json::Object *item = params.getObject("textDocument");
  std::optional<llvm::StringRef> uri = item ? item->getString("uri") : std::nullopt;
  std::optional<llvm::StringRef> text = item ? item->getString("text") : std::nullopt;
  if (!uri || !text) {
    return;
  }
  std::string file = FileUri::toPath(*uri);
  if (file.empty()) {
    return;
  }
  auto document = std::make_unique<TextDocument>(uri->str(), file, text->str());
  TextDocument &added = *document;
  this->documents[uri->str()] = std::move(document);
  assign(added, kOpenDelay);
  touch(added.key(), kOpenDelay);
}

void LspServer::changed(const llvm::json::Object &params) {
  const llvm::json::Object *item = params.getObject("textDocument");
  std::optional<llvm::StringRef> uri = item ? item->getString("uri") : std::nullopt;
  const llvm::json::Array *changes = params.getArray("contentChanges");
  if (!uri || !changes) {
    return;
  }
  auto found = this->documents.find(uri->str());
  if (found == this->documents.end()) {
    return;
  }
  TextDocument &document = *found->second;
  for (const llvm::json::Value &change : *changes) {
    if (const llvm::json::Object *fields = change.getAsObject()) {
      document.apply(*fields);
    }
  }
  auto entry = this->entries.find(document.entry);
  if (entry != this->entries.end()) {
    schedule(entry->second, kTypingDelay);
  }
  touch(document.key(), kTypingDelay);
}

void LspServer::closed(const llvm::json::Object &params) {
  const llvm::json::Object *item = params.getObject("textDocument");
  std::optional<llvm::StringRef> uri = item ? item->getString("uri") : std::nullopt;
  if (!uri) {
    return;
  }
  auto found = this->documents.find(uri->str());
  if (found == this->documents.end()) {
    return;
  }
  std::string entry = found->second->entry;
  std::string file = found->second->key();
  this->documents.erase(found);
  if (!used(entry)) {
    forget(entry);
  }
  touch(file, kOpenDelay);
}

void LspServer::watched(const llvm::json::Object &params) {
  const llvm::json::Array *changes = params.getArray("changes");
  if (!changes) {
    return;
  }
  bool manifests = false;
  bool moved = false;
  std::vector<std::string> files;
  for (const llvm::json::Value &change : *changes) {
    const llvm::json::Object *fields = change.getAsObject();
    std::optional<llvm::StringRef> uri = fields ? fields->getString("uri") : std::nullopt;
    if (!uri) {
      continue;
    }
    std::string file = FileUri::toPath(*uri);
    if (path::filename(file) == "package.json") {
      manifests = true;
    }
    if (fields->getInteger("type").value_or(2) != 2) {
      moved = true;
    }
    files.push_back(SourceOverlay::key(file));
  }
  if (manifests) {
    std::vector<std::string> keys;
    for (const auto &[key, entry] : this->entries) {
      keys.push_back(key);
    }
    for (const std::string &key : keys) {
      forget(key);
    }
    for (auto &[uri, document] : this->documents) {
      document->alone = false;
      assign(*document, kOpenDelay);
    }
    return;
  }
  if (moved) {
    for (auto &[key, entry] : this->entries) {
      schedule(entry, kOpenDelay);
    }
    return;
  }
  for (const std::string &file : files) {
    bool open = false;
    for (const auto &[uri, document] : this->documents) {
      open = open || document->key() == file;
    }
    if (!open) {
      touch(file, kOpenDelay);
    }
  }
}

std::string LspServer::entryOf(const TextDocument &document) const {
  if (document.alone) {
    return document.path();
  }

  return ProgramEntry::forFile(document.path());
}

void LspServer::assign(TextDocument &document, milliseconds delay) {
  std::string file = entryOf(document);
  document.entry = SourceOverlay::key(file);
  Entry &entry = this->entries[document.entry];
  if (entry.path.empty()) {
    entry.key = document.entry;
    entry.path = file;
  }
  schedule(entry, delay);
}

void LspServer::touch(const std::string &file, milliseconds delay) {
  for (auto &[key, entry] : this->entries) {
    if (key == file || entry.files.count(file) != 0) {
      schedule(entry, delay);
    }
  }
}

void LspServer::schedule(Entry &entry, milliseconds delay) {
  entry.dirty = true;
  entry.level = 1;
  entry.due = Clock::now() + delay;
  if (this->checking && this->running == entry.key) {
    this->cancel = true;
  }
}

void LspServer::forget(const std::string &key) {
  auto found = this->entries.find(key);
  if (found == this->entries.end()) {
    return;
  }
  publish(found->second, {});
  if (this->checking && this->running == key) {
    this->cancel = true;
  }
  this->entries.erase(found);
}

bool LspServer::used(const std::string &key) const {
  for (const auto &[uri, document] : this->documents) {
    if (document->entry == key) {
      return true;
    }
  }

  return false;
}

std::string LspServer::uriFor(llvm::StringRef file) const {
  std::string key = SourceOverlay::key(file);
  for (const auto &[uri, document] : this->documents) {
    if (document->key() == key) {
      return uri;
    }
  }

  return FileUri::fromPath(file);
}

void LspServer::work() {
  std::unique_lock<std::mutex> lock(this->mutex);
  while (!this->stopping) {
    if (this->checking) {
      if (this->cancel) {
        this->process.kill();
        this->checking = false;
        this->cancel = false;
        continue;
      }
      std::optional<int> code = this->process.poll();
      if (!code) {
        this->wake.wait_for(lock, kPollDelay);
        continue;
      }
      finish(*code);
      continue;
    }
    Clock::time_point now = Clock::now();
    Clock::time_point earliest = Clock::time_point::max();
    Entry *next = nullptr;
    for (auto &[key, entry] : this->entries) {
      if (!entry.dirty) {
        continue;
      }
      if (entry.due <= now) {
        next = &entry;
        break;
      }
      earliest = std::min(earliest, entry.due);
    }
    if (next) {
      start(*next);
    } else if (earliest == Clock::time_point::max()) {
      this->wake.wait(lock);
    } else {
      this->wake.wait_until(lock, earliest);
    }
  }
  if (this->checking) {
    this->process.kill();
    this->checking = false;
  }
}

void LspServer::start(Entry &entry) {
  entry.dirty = false;
  llvm::json::Array files;
  for (const auto &[uri, document] : this->documents) {
    files.push_back(llvm::json::Object{{"path", document->path()}, {"text", document->text()}});
  }
  llvm::json::Value request = llvm::json::Object{{"entry", entry.path},
                                                 {"files", std::move(files)},
                                                 {"knowledge", entry.level == 1},
                                                 {"level", entry.level}};
  std::error_code ec;
  {
    llvm::raw_fd_ostream out(this->request, ec);
    if (!ec) {
      out << request;
    }
  }
  if (ec) {
    log("tz: cannot write '" + this->request + "': " + ec.message());
    return;
  }
  fs::remove(this->response);
  std::string error;
  llvm::StringRef args[] = {this->self, "lsp", "--check", this->request, this->response};
  if (!this->process.start(this->self, args, log_, error)) {
    log("tz: cannot start '" + this->self + "': " + error);
    return;
  }
  this->checking = true;
  this->cancel = false;
  this->running = entry.key;
  this->runningLevel = entry.level;
  this->started = Clock::now();
}

llvm::json::Value LspServer::diagnostic(const llvm::json::Object &found) const {
  llvm::json::Array related;
  if (const llvm::json::Array *notes = found.getArray("related")) {
    for (const llvm::json::Value &note : *notes) {
      const llvm::json::Object *fields = note.getAsObject();
      if (!fields) {
        continue;
      }
      std::string uri = uriFor(fields->getString("path").value_or(""));
      const llvm::json::Value *range = fields->get("range");
      llvm::json::Object location{{"uri", uri},
                                  {"range", range ? *range : LspServer::startRange()}};
      related.push_back(
          llvm::json::Object{{"location", std::move(location)},
                             {"message", fields->getString("message").value_or("").str()}});
    }
  }
  const llvm::json::Value *range = found.get("range");

  return llvm::json::Object{{"range", range ? *range : LspServer::startRange()},
                            {"severity", found.getInteger("severity").value_or(1)},
                            {"source", "tz"},
                            {"message", found.getString("message").value_or("").str()},
                            {"relatedInformation", std::move(related)}};
}

void LspServer::finish(int code) {
  this->checking = false;
  auto found = this->entries.find(this->running);
  if (found == this->entries.end()) {
    return;
  }
  Entry &entry = found->second;
  auto elapsed = std::chrono::duration_cast<milliseconds>(Clock::now() - this->started).count();
  if (auto output = llvm::MemoryBuffer::getFile(log_); output && !(*output)->getBuffer().empty()) {
    log((*output)->getBuffer().rtrim());
  }

  std::optional<llvm::json::Value> parsed;
  if (code == 0) {
    if (auto buffer = llvm::MemoryBuffer::getFile(this->response)) {
      llvm::Expected<llvm::json::Value> value = llvm::json::parse((*buffer)->getBuffer());
      if (value) {
        parsed = std::move(*value);
      } else {
        llvm::consumeError(value.takeError());
      }
    }
  }
  const llvm::json::Object *report = parsed ? parsed->getAsObject() : nullptr;
  std::map<std::string, llvm::json::Array> next;
  if (!report) {
    std::string message = "tz stopped while checking this program (exit code " +
                          std::to_string(code) + "): this is a bug in tz";
    next[uriFor(entry.path)].push_back(llvm::json::Object{{"range", LspServer::startRange()},
                                                          {"severity", 1},
                                                          {"source", "tz"},
                                                          {"message", message}});
    log("tz: " + entry.path + ": " + message);
    publish(entry, std::move(next));
    return;
  }

  if (const llvm::json::Value *known = report->get("knowledge")) {
    KnowledgeJson::read(*known, entry.knowledge);
    if (this->refreshTokens) {
      this->channel.send(llvm::json::Object{{"jsonrpc", "2.0"},
                                            {"id", "tz-" + std::to_string(this->nextRequest++)},
                                            {"method", "workspace/semanticTokens/refresh"}});
    }
  }
  entry.files.clear();
  if (const llvm::json::Array *files = report->getArray("files")) {
    for (const llvm::json::Value &file : *files) {
      if (std::optional<llvm::StringRef> text = file.getAsString()) {
        entry.files.insert(SourceOverlay::key(*text));
      }
    }
  }
  size_t count = 0;
  if (const llvm::json::Array *diagnostics = report->getArray("diagnostics")) {
    for (const llvm::json::Value &item : *diagnostics) {
      if (const llvm::json::Object *fields = item.getAsObject()) {
        next[uriFor(fields->getString("path").value_or(entry.path))].push_back(diagnostic(*fields));
        ++count;
      }
    }
  }
  log("tz: checked " + entry.path + (this->runningLevel == 1 ? " (quick)" : " (whole program)") +
      " in " + std::to_string(elapsed) + " ms: " + std::to_string(count) + " problems");
  for (const auto &[uri, document] : this->documents) {
    if (entry.files.count(document->key()) != 0) {
      next.try_emplace(uri);
    }
  }
  if (this->runningLevel == 1) {
    entry.quick.clear();
    for (const auto &[uri, list] : next) {
      std::set<std::string> &seen = entry.quick[uri];
      for (const llvm::json::Value &item : list) {
        seen.insert(LspServer::textOf(item));
      }
    }
    for (const auto &[uri, list] : entry.later) {
      for (const llvm::json::Value &item : list) {
        next[uri].push_back(item);
      }
    }
    if (!entry.dirty) {
      entry.dirty = true;
      entry.level = 2;
      entry.due = Clock::now();
    }
  } else {
    entry.later.clear();
    for (const auto &[uri, list] : next) {
      const std::set<std::string> &seen = entry.quick[uri];
      for (const llvm::json::Value &item : list) {
        if (seen.count(LspServer::textOf(item)) == 0) {
          entry.later[uri].push_back(item);
        }
      }
    }
  }
  publish(entry, std::move(next));

  if (entry.files.empty()) {
    return;
  }
  std::string key = entry.key;
  for (auto &[uri, document] : this->documents) {
    if (document->entry == key && !document->alone && entry.files.count(document->key()) == 0) {
      document->alone = true;
      assign(*document, milliseconds(0));
    }
  }
  if (!used(key)) {
    forget(key);
  }
}

void LspServer::publish(Entry &entry, std::map<std::string, llvm::json::Array> next) {
  std::set<std::string> uris;
  for (const auto &[uri, list] : entry.published) {
    uris.insert(uri);
  }
  for (const auto &[uri, list] : next) {
    uris.insert(uri);
  }
  entry.published = std::move(next);
  for (const std::string &uri : uris) {
    llvm::json::Array merged;
    std::set<std::string> seen;
    for (const auto &[key, other] : this->entries) {
      auto list = other.published.find(uri);
      if (list == other.published.end()) {
        continue;
      }
      for (const llvm::json::Value &item : list->second) {
        std::string text;
        llvm::raw_string_ostream stream(text);
        stream << item;
        if (seen.insert(text).second) {
          merged.push_back(item);
        }
      }
    }
    notify("textDocument/publishDiagnostics",
           llvm::json::Object{{"uri", uri}, {"diagnostics", std::move(merged)}});
  }
}

}
