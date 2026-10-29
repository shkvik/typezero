#include "compiler/mcp/mcp_server/mcp_server.h"
#include "compiler/knowledge/knowledge_json/knowledge_json.h"
#include "compiler/knowledge/knowledge_question/knowledge_question.h"
#include "compiler/knowledge/knowledge_target/knowledge_target.h"
#include "compiler/lsp/program_entry/program_entry.h"
#include "compiler/program/source_places/source_places.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"
#include <cstdio>

namespace tz {

namespace {

namespace fs = llvm::sys::fs;

namespace path = llvm::sys::path;

const char kEntry[] = "The program's main .tz file. By default: the \"main\" of the nearest "
                      "tz.json, or the file the target names.";

const char kTarget[] = "What to look at: 'name', 'Class.member', 'file.tz:line' or "
                       "'file.tz:line:column'.";

const char kBudget[] = "Roughly how many tokens the answer may take.";

llvm::json::Object property(llvm::StringRef type, llvm::StringRef description) {
  return llvm::json::Object{{"type", type}, {"description", description}};
}

llvm::json::Object schema(llvm::json::Object properties, std::vector<std::string> required) {
  llvm::json::Array names;
  for (const std::string &name : required) {
    names.push_back(name);
  }

  return llvm::json::Object{
      {"type", "object"}, {"properties", std::move(properties)}, {"required", std::move(names)}};
}

llvm::json::Object tool(llvm::StringRef name, llvm::StringRef description,
                        llvm::json::Object input) {
  return llvm::json::Object{
      {"name", name}, {"description", description}, {"inputSchema", std::move(input)}};
}

std::string readAll(llvm::StringRef file) {
  auto buffer = llvm::MemoryBuffer::getFile(file);

  return buffer ? (*buffer)->getBuffer().str() : std::string();
}

std::string temporary(llvm::StringRef prefix, llvm::StringRef suffix) {
  llvm::SmallString<256> file;
  if (fs::createTemporaryFile(prefix, suffix, file)) {
    return "";
  }

  return std::string(file);
}

}

McpServer::McpServer(std::string self) : self_(std::move(self)) {
  llvm::SmallString<256> here;
  fs::current_path(here);
  here_ = std::string(here);
  llvm::sys::ChangeStdinToBinary();
  llvm::sys::ChangeStdoutToBinary();
}

std::optional<std::string> McpServer::readLine() {
  std::string line;
  for (int c = std::fgetc(stdin); c != EOF; c = std::fgetc(stdin)) {
    if (c == '\n') {
      return line;
    }
    line.push_back(static_cast<char>(c));
  }
  if (line.empty()) {
    return std::nullopt;
  }

  return line;
}

void McpServer::send(const llvm::json::Value &message) {
  std::string text;
  llvm::raw_string_ostream stream(text);
  stream << message;
  text += "\n";
  std::fwrite(text.data(), 1, text.size(), stdout);
  std::fflush(stdout);
}

int McpServer::run() {
  for (;;) {
    std::optional<std::string> line = readLine();
    if (!line) {
      return 0;
    }
    if (llvm::StringRef(*line).trim().empty()) {
      continue;
    }
    llvm::Expected<llvm::json::Value> message = llvm::json::parse(*line);
    if (!message) {
      llvm::consumeError(message.takeError());
      llvm::json::Object error{{"code", -32700}, {"message", "the message is not JSON"}};
      send(llvm::json::Object{{"jsonrpc", "2.0"}, {"id", nullptr}, {"error", std::move(error)}});
      continue;
    }
    if (const llvm::json::Object *object = message->getAsObject()) {
      if (std::optional<llvm::json::Value> answer = handle(*object)) {
        send(*answer);
      }
    }
  }
}

std::optional<llvm::json::Value> McpServer::handle(const llvm::json::Object &message) {
  std::optional<llvm::StringRef> method = message.getString("method");
  const llvm::json::Value *id = message.get("id");
  if (!method || !id) {
    return std::nullopt;
  }
  const llvm::json::Object empty;
  const llvm::json::Object *params = message.getObject("params");
  const llvm::json::Object &args = params ? *params : empty;
  llvm::json::Value result = nullptr;
  if (*method == "initialize") {
    llvm::StringRef version = args.getString("protocolVersion").value_or("2025-06-18");
    if (version != "2024-11-05" && version != "2025-03-26" && version != "2025-06-18") {
      version = "2025-06-18";
    }
    llvm::json::Object capabilities{{"tools", llvm::json::Object{{"listChanged", false}}}};
    llvm::json::Object info{{"name", "tz"}, {"version", TZ_VERSION}};
    result = llvm::json::Object{
        {"protocolVersion", version.str()},
        {"capabilities", std::move(capabilities)},
        {"serverInfo", std::move(info)},
        {"instructions",
         "The tz compiler answers questions about a tz program: start with map, then context "
         "for a symbol; query for definitions, references, callers and types; impact before "
         "a change; check_edit to check an edit without writing it; language_card for how tz "
         "differs from TypeScript."}};
  } else if (*method == "ping") {
    result = llvm::json::Object{};
  } else if (*method == "tools/list") {
    result = llvm::json::Object{{"tools", tools()}};
  } else if (*method == "tools/call") {
    llvm::StringRef name = args.getString("name").value_or("");
    const llvm::json::Object *given = args.getObject("arguments");
    bool failed = false;
    std::string text = call(name, given ? *given : empty, failed);
    llvm::json::Array content;
    content.push_back(llvm::json::Object{{"type", "text"}, {"text", text}});
    result = llvm::json::Object{{"content", std::move(content)}, {"isError", failed}};
  } else {
    llvm::json::Object error{{"code", -32601},
                             {"message", "tz does not answer '" + method->str() + "'"}};
    return llvm::json::Value(
        llvm::json::Object{{"jsonrpc", "2.0"}, {"id", *id}, {"error", std::move(error)}});
  }

  return llvm::json::Value(
      llvm::json::Object{{"jsonrpc", "2.0"}, {"id", *id}, {"result", std::move(result)}});
}

llvm::json::Value McpServer::tools() const {
  llvm::json::Array all;
  all.push_back(tool(
      "map",
      "Overview of a tz program: its modules, what each declares with full types (inferred "
      "ones too), what each imports and how often each symbol is used; within the budget the "
      "exported and most used come first. Start here to learn a codebase.",
      schema(llvm::json::Object{{"entry", property("string", kEntry)},
                                {"budget", property("integer", kBudget)},
                                {"library", property("boolean", "Show the built-in modules too.")}},
             {})));
  all.push_back(tool(
      "context",
      "Everything about one symbol or place: its signature and source, the compiler's errors in "
      "it, what it uses (with signatures), who uses it (with the line of each use), the members "
      "of a class, docs sections and tests that mention it.",
      schema(llvm::json::Object{{"target", property("string", kTarget)},
                                {"entry", property("string", kEntry)},
                                {"budget", property("integer", kBudget)}},
             {"target"})));
  llvm::json::Object kind = property("string", "What to find.");
  kind["enum"] = llvm::json::Array{"definition", "references", "callers", "type"};
  all.push_back(
      tool("query",
           "Exact answers from the compiler: the definition, all references, the callers or the "
           "type of a symbol, or the type of the name at file.tz:line:column (locals too).",
           schema(llvm::json::Object{{"kind", std::move(kind)},
                                     {"target", property("string", kTarget)},
                                     {"entry", property("string", kEntry)}},
                  {"kind", "target"})));
  all.push_back(
      tool("impact",
           "What a change to a symbol can affect: who uses it, who uses those, level by level, the "
           "modules and the exported symbols among them.",
           schema(llvm::json::Object{{"target", property("string", kTarget)},
                                     {"entry", property("string", kEntry)},
                                     {"budget", property("integer", kBudget)}},
                  {"target"})));
  all.push_back(tool(
      "check_edit",
      "Check an edit without writing it: give the file and either its whole new text or an "
      "exact old -> new replacement (old must occur once). The compiler checks the program with "
      "that text in memory and returns its errors as file:line:column: message, or says that "
      "it checks. Nothing is written to the disk.",
      schema(llvm::json::Object{{"file", property("string", "The .tz file the edit changes.")},
                                {"text", property("string", "The whole new text of the file.")},
                                {"old", property("string", "Text of the file to replace.")},
                                {"new", property("string", "What replaces 'old'.")},
                                {"entry", property("string", kEntry)}},
             {"file"})));
  all.push_back(tool("language_card",
                     "How tz differs from TypeScript: what is missing, numbers, comparisons, "
                     "errors as Result values, async, the library's style. Read it before "
                     "writing tz code.",
                     schema(llvm::json::Object{}, {})));

  return all;
}

std::string McpServer::call(llvm::StringRef name, const llvm::json::Object &args, bool &failed) {
  if (name == "language_card") {
    return languageCard(failed);
  }
  if (name == "check_edit") {
    return checkEdit(args, failed);
  }
  if (name != "map" && name != "context" && name != "query" && name != "impact") {
    failed = true;
    return "tz has no tool '" + name.str() + "'";
  }
  KnowledgeRequest request;
  request.command = name.str();
  request.kind = name == "query" ? args.getString("kind").value_or("").str() : "";
  if (name == "query" && request.kind != "definition" && request.kind != "references" &&
      request.kind != "callers" && request.kind != "type") {
    failed = true;
    return "'query' needs a kind: definition, references, callers or type";
  }
  std::optional<KnowledgeTarget> target;
  if (name != "map") {
    std::optional<llvm::StringRef> given = args.getString("target");
    if (!given) {
      failed = true;
      return "'" + name.str() + "' needs a target: 'name', 'Class.member' or 'file.tz:line'";
    }
    request.target = given->str();
    target.emplace(*given);
  }
  request.budget =
      static_cast<size_t>(args.getInteger("budget").value_or(name == "map" ? 2000 : 1500));
  request.library = args.getBoolean("library").value_or(false);

  std::string entry;
  if (std::optional<llvm::StringRef> named = args.getString("entry")) {
    entry = SourcePlaces::absolute(*named);
  } else if (target && target->isPlace()) {
    if (!fs::is_regular_file(target->file())) {
      failed = true;
      return "there is no file '" + target->file() + "'";
    }
    entry = ProgramEntry::forFile(target->file());
  } else {
    entry = ProgramEntry::forDirectory(here_);
  }
  if (entry.empty()) {
    failed = true;
    return "which program? give 'entry', its main .tz file, or work in a directory with a tz.json "
           "whose \"main\" names it";
  }
  if (target && target->isPlace() && target->hasColumn()) {
    std::vector<std::string> words = {name.str()};
    if (name == "query") {
      words.push_back(request.kind);
    }
    words.insert(words.end(),
                 {*request.target, "--entry", entry, "--budget", std::to_string(request.budget)});
    return runTz(words, failed);
  }
  std::string message;
  const Knowledge *known = knowledgeFor(entry, failed, message);
  if (known && target && target->isPlace() && !known->moduleAt(target->file())) {
    std::string alone = SourcePlaces::absolute(target->file());
    known = alone == entry ? nullptr : knowledgeFor(alone, failed, message);
    if (!known && !failed) {
      failed = true;
      message = "'" + target->file() + "' is not a module of the program";
    }
  }
  if (!known) {
    return message;
  }
  std::string out;
  std::string error;
  if (!KnowledgeQuestion::answer(*known, request, out, error)) {
    failed = true;
    return error;
  }

  return out;
}

const Knowledge *McpServer::knowledgeFor(const std::string &entry, bool &failed,
                                         std::string &message) {
  auto stampOf = [](llvm::StringRef file) {
    fs::file_status status;
    if (fs::status(file, status)) {
      return std::make_pair(int64_t(-1), uint64_t(0));
    }
    int64_t time = status.getLastModificationTime().time_since_epoch().count();

    return std::make_pair(time, status.getSize());
  };
  auto found = cache_.find(entry);
  if (found != cache_.end()) {
    bool fresh = llvm::all_of(found->second.stamps, [&](const auto &stamp) {
      return stampOf(stamp.first) == stamp.second;
    });
    if (fresh) {
      return &found->second.knowledge;
    }
    cache_.erase(found);
  }
  std::string text = runTz({"query", "records", "--entry", entry, "--json", "--brief"}, failed);
  if (failed) {
    message = text;
    return nullptr;
  }
  llvm::Expected<llvm::json::Value> parsed = llvm::json::parse(text);
  Cached cached;
  if (!parsed || !KnowledgeJson::read(*parsed, cached.knowledge)) {
    if (!parsed) {
      llvm::consumeError(parsed.takeError());
    }
    failed = true;
    message = "tz did not answer with the records of the program";
    return nullptr;
  }
  for (const KnownModule &module : cached.knowledge.modules) {
    if (!module.library) {
      cached.stamps.push_back({module.file, stampOf(module.file)});
    }
  }
  auto [at, added] = cache_.try_emplace(entry, std::move(cached));

  return &at->second.knowledge;
}

std::string McpServer::runTz(const std::vector<std::string> &args, bool &failed) {
  std::string out = temporary("tz-mcp-out", "txt");
  std::string err = temporary("tz-mcp-err", "txt");
  std::vector<llvm::StringRef> argv = {self_};
  for (const std::string &arg : args) {
    argv.push_back(arg);
  }
  std::optional<llvm::StringRef> redirects[] = {llvm::StringRef(""), llvm::StringRef(out),
                                                llvm::StringRef(err)};
  std::string message;
  int code = llvm::sys::ExecuteAndWait(self_, argv, std::nullopt, redirects, 0, 0, &message);
  std::string output = readAll(out);
  std::string errors = readAll(err);
  fs::remove(out);
  fs::remove(err);
  if (code != 0) {
    failed = true;
    std::string text = errors.empty() ? output : errors;
    return text.empty() ? "tz stopped (exit code " + std::to_string(code) + "): " + message : text;
  }

  return output;
}

std::string McpServer::shown(llvm::StringRef file) const {
  llvm::StringRef rest = file;
  if (rest.size() > here_.size() && rest.starts_with_insensitive(here_) &&
      path::is_separator(rest[here_.size()])) {
    rest = rest.drop_front(here_.size() + 1);
  }

  return path::convert_to_slash(rest);
}

std::string McpServer::checkEdit(const llvm::json::Object &args, bool &failed) {
  std::optional<llvm::StringRef> given = args.getString("file");
  if (!given) {
    failed = true;
    return "check_edit needs 'file'";
  }
  std::string file = SourcePlaces::absolute(*given);
  std::string text;
  if (std::optional<llvm::StringRef> whole = args.getString("text")) {
    text = whole->str();
  } else {
    auto buffer = llvm::MemoryBuffer::getFile(file);
    if (!buffer) {
      failed = true;
      return "cannot read '" + given->str() + "': " + buffer.getError().message();
    }
    text = (*buffer)->getBuffer().str();
    std::optional<llvm::StringRef> old = args.getString("old");
    std::optional<llvm::StringRef> replacement = args.getString("new");
    if (old && replacement) {
      size_t count = llvm::StringRef(text).count(*old);
      if (count != 1) {
        failed = true;
        return "'old' occurs " + std::to_string(count) + " times in " + given->str() +
               "; it must occur once";
      }
      size_t at = text.find(old->str());
      text.replace(at, old->size(), replacement->str());
    }
  }
  std::optional<llvm::StringRef> named = args.getString("entry");
  std::string entry = named ? SourcePlaces::absolute(*named) : ProgramEntry::forFile(file);

  std::string request = temporary("tz-mcp-request", "json");
  std::string response = temporary("tz-mcp-response", "json");
  llvm::json::Object report;
  for (int attempt = 0; attempt < 2; ++attempt) {
    {
      llvm::json::Array files;
      files.push_back(llvm::json::Object{{"path", file}, {"text", text}});
      std::error_code ec;
      llvm::raw_fd_ostream stream(request, ec);
      stream << llvm::json::Value(
          llvm::json::Object{{"entry", entry}, {"files", std::move(files)}});
    }
    std::string output = runTz({"lsp", "--check", request, response}, failed);
    if (failed) {
      fs::remove(request);
      fs::remove(response);
      return output;
    }
    llvm::Expected<llvm::json::Value> parsed = llvm::json::parse(readAll(response));
    if (!parsed || !parsed->getAsObject()) {
      if (!parsed) {
        llvm::consumeError(parsed.takeError());
      }
      failed = true;
      return "tz did not answer the check";
    }
    report = std::move(*parsed->getAsObject());
    bool inside = false;
    if (const llvm::json::Array *files = report.getArray("files")) {
      for (const llvm::json::Value &item : *files) {
        inside = inside || item.getAsString().value_or("").equals_insensitive(file);
      }
    }
    if (inside || entry == file) {
      break;
    }
    entry = file;
  }
  fs::remove(request);
  fs::remove(response);

  std::string out;
  size_t count = 0;
  auto at = [&](const llvm::json::Object &item) {
    const llvm::json::Object *range = item.getObject("range");
    const llvm::json::Object *start = range ? range->getObject("start") : nullptr;
    int64_t line = start ? start->getInteger("line").value_or(0) : 0;
    int64_t character = start ? start->getInteger("character").value_or(0) : 0;

    return shown(item.getString("path").value_or("")) + ":" + std::to_string(line + 1) + ":" +
           std::to_string(character + 1);
  };
  if (const llvm::json::Array *diagnostics = report.getArray("diagnostics")) {
    for (const llvm::json::Value &value : *diagnostics) {
      const llvm::json::Object *item = value.getAsObject();
      if (!item) {
        continue;
      }
      ++count;
      bool error = item->getInteger("severity").value_or(1) == 1;
      out += at(*item) + (error ? ": error: " : ": warning: ") +
             item->getString("message").value_or("").str() + "\n";
      if (const llvm::json::Array *related = item->getArray("related")) {
        for (const llvm::json::Value &note : *related) {
          if (const llvm::json::Object *fields = note.getAsObject()) {
            out += "  note: " + at(*fields) + ": " +
                   fields->getString("message").value_or("").str() + "\n";
          }
        }
      }
    }
  }
  if (count == 0) {
    return "no problems: the program " + shown(entry) +
           " checks with this edit (nothing was written)\n";
  }

  return std::to_string(count) + (count == 1 ? " problem" : " problems") + " with this edit " +
         "(nothing was written):\n" + out;
}

std::string McpServer::languageCard(bool &failed) const {
  llvm::SmallString<256> dir(path::parent_path(self_));
  llvm::SmallString<256> card(dir);
  path::append(card, "ai.md");
  if (!fs::is_regular_file(card)) {
    card = dir;
    path::append(card, "..", "..", "docs");
    path::append(card, "ai.md");
  }
  std::string text = readAll(card);
  if (text.empty()) {
    failed = true;
    return "the language card (ai.md) is not next to tz";
  }

  return text;
}

}
