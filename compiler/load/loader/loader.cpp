#include "compiler/load/loader/loader.h"
#include "compiler/check/checker/checker.h"
#include "compiler/load/lexer/lexer.h"
#include "compiler/load/library_modules/library_modules.h"
#include "compiler/load/module_resolver/module_resolver.h"
#include "compiler/load/parser/parser.h"
#include "compiler/packages/graph/graph.h"
#include "compiler/packages/package/package.h"
#include "compiler/packages/specifier/specifier.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>

namespace tz {

namespace {

namespace fs = llvm::sys::fs;

namespace path = llvm::sys::path;

}

bool Loader::run(llvm::StringRef entryPath, SourceFile &program) {
  if (path::extension(entryPath) != ".tz") {
    std::string message = "'" + entryPath.str() + "' is not a file of tz: they end in .tz";
    if (path::extension(entryPath) == ".ts") {
      message += "; rename it to " + path::stem(entryPath).str() + ".tz";
    }
    diags_.tool(message);
    return false;
  }
  llvm::SmallString<256> real;
  if (std::error_code ec = fs::real_path(entryPath, real)) {
    diags_.tool("cannot read '" + entryPath + "': " + ec.message());
    return false;
  }
  entryDir_ = path::parent_path(entryPath).str();
  entryRealDir_ = path::parent_path(real).str();
  std::string error;
  Module *entry = parse(entryPath.str(), std::string(real), path::filename(real).str(), error);
  if (!entry) {
    diags_.tool("cannot read '" + entryPath + "': " + error);
    return false;
  }
  entry->isEntry = true;
  packageOf_[entry] = packages_ ? 0 : -1;
  visit(entry);

  for (Module *module : order_) {
    program.modules.push_back(module);
    SourceFile &file = module->file;
    program.items.insert(program.items.end(), file.items.begin(), file.items.end());
    for (const auto &name : file.fieldNames) {
      program.fieldNames.insert(name.getKey());
    }
    for (const auto &constant : file.stringConstants) {
      std::vector<std::string> &all = program.stringConstants[constant.getKey()];
      all.insert(all.end(), constant.getValue().begin(), constant.getValue().end());
    }
  }

  return true;
}

Module *Loader::parse(const std::string &filePath, const std::string &realPath, std::string name,
                      std::string &error) {
  auto buffer = llvm::MemoryBuffer::getFile(filePath);
  if (!buffer) {
    error = buffer.getError().message();
    return nullptr;
  }
  unsigned id = sourceMgr_.AddNewSourceBuffer(std::move(*buffer), llvm::SMLoc());
  llvm::StringRef text = sourceMgr_.getMemoryBuffer(id)->getBuffer();
  rejectReferences(text);

  Module *module = ctx_.newModule();
  module->path = filePath;
  module->name = std::move(name);
  Lexer lexer(text, diags_);
  module->file = Parser(lexer, ctx_, diags_).parseSourceFile();
  library().importLibraryGlobals(module, text);
  library().importFsStreams(module, text);
  byRealPath_[realPath] = module;
  realPaths_[module] = realPath;
  states_[module] = State::New;

  return module;
}

void Loader::visit(Module *module) {
  states_[module] = State::Loading;
  loading_.push_back(module);
  module->loadPath = loading_;

  struct Request {
    const char *at;
    llvm::StringRef path;
    llvm::SMLoc loc;
    Module **from;
  };
  std::vector<Request> requests;
  for (ImportDecl &decl : module->file.imports) {
    requests.push_back({decl.pathLoc.getPointer(), decl.path, decl.pathLoc, &decl.from});
  }
  for (ExportDecl &decl : module->file.exports) {
    if (!decl.path.empty()) {
      requests.push_back({decl.pathLoc.getPointer(), decl.path, decl.pathLoc, &decl.from});
    }
  }
  std::stable_sort(requests.begin(), requests.end(),
                   [](const Request &a, const Request &b) { return a.at < b.at; });

  for (const Request &request : requests) {
    Module *target =
        *request.from ? *request.from : resolver().resolve(module, request.path, request.loc);
    *request.from = target;
    if (target && states_[target] == State::New) {
      visit(target);
    }
  }

  loading_.pop_back();
  states_[module] = State::Loaded;
  module->order = static_cast<unsigned>(order_.size());
  order_.push_back(module);
}

std::string Loader::relativeName(llvm::StringRef realPath) const {
  std::vector<llvm::StringRef> base(path::begin(entryRealDir_), path::end(entryRealDir_));
  std::vector<llvm::StringRef> target(path::begin(realPath), path::end(realPath));
  size_t common = 0;
  while (common < base.size() && common < target.size() && base[common] == target[common]) {
    ++common;
  }
  if (common == 0) {
    return path::convert_to_slash(realPath);
  }
  std::string name;
  for (size_t i = common; i < base.size(); ++i) {
    name += "../";
  }
  for (size_t i = common; i < target.size(); ++i) {
    name += target[i].str() + (i + 1 < target.size() ? "/" : "");
  }

  return name;
}

void Loader::rejectReferences(llvm::StringRef text) {
  for (size_t at = 0; at < text.size();) {
    size_t end = text.find('\n', at);
    llvm::StringRef line = text.slice(at, end).ltrim();
    if (line.consume_front("///") && line.ltrim().starts_with("<reference")) {
      diags_.error(llvm::SMLoc::getFromPointer(line.data() - 3),
                   "'/// <reference>' is not supported: files are joined by imports; import what "
                   "the file needs by name, e.g. 'import { a } from \"./x\"'");
    }
    if (end == llvm::StringRef::npos) {
      break;
    }
    at = end + 1;
  }
}

bool loadProgram(llvm::StringRef entryPath, llvm::SourceMgr &sourceMgr, AstContext &ctx,
                 Diagnostics &diags, SourceFile &program, const pkg::Graph *packages) {
  return Loader(sourceMgr, ctx, diags, packages).run(entryPath, program);
}

Loader::Loader(llvm::SourceMgr &sourceMgr, AstContext &ctx, Diagnostics &diags,
               const pkg::Graph *packages)
    : sourceMgr_(sourceMgr), ctx_(ctx), diags_(diags), packages_(packages) {
  resolver_ = std::make_unique<ModuleResolver>(*this);
  library_ = std::make_unique<LibraryModules>(*this);
}

Loader::~Loader() = default;
}
