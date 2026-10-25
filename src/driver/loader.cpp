#include "driver/loader.h"

#include "lex/lexer.h"
#include "parse/parser.h"
#include "pkg/project.h"
#include "sema/sema.h"

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

// Why an import cannot name `file` (relative to `dir`), which `spec` names as
// written, or "" when its extension is not one of TypeScript or JavaScript:
// the files of tz end in .tz (docs/modules.md).
std::string foreignFile(llvm::StringRef dir, llvm::StringRef file, llvm::StringRef spec) {
  static const llvm::StringLiteral kForeign[] = {".ts",  ".js",  ".tsx", ".jsx",
                                                 ".mts", ".cts", ".mjs", ".cjs"};
  llvm::StringRef ext = path::extension(file, path::Style::posix);
  if (!llvm::is_contained(kForeign, ext))
    return "";
  if (file.ends_with(".d.ts"))
    return "only .tz files can be imported; tz needs no declarations: it reads the code itself";
  std::string stem = spec.drop_back(ext.size()).str();
  std::string message = "the files of tz end in .tz: import '" + stem + "' or '" + stem + ".tz'";
  llvm::SmallString<256> full(dir);
  path::append(full, file);
  if (fs::is_regular_file(full))
    message += ", and rename " + path::filename(file, path::Style::posix).str() + " to " +
               path::stem(file, path::Style::posix).str() + ".tz";
  return message;
}

class Loader {
public:
  Loader(llvm::SourceMgr &sourceMgr, AstContext &ctx, Diagnostics &diags,
         const pkg::Graph *packages)
      : sourceMgr_(sourceMgr), ctx_(ctx), diags_(diags), packages_(packages) {}

  bool run(llvm::StringRef entryPath, SourceFile &program);

private:
  enum class State { New, Loading, Loaded };

  // Reads and parses a file. Null if it cannot be read.
  Module *parse(const std::string &filePath, const std::string &realPath, std::string name,
                std::string &error);
  // Loads the files `module` imports, then puts it in the order.
  void visit(Module *module);
  // The file an import names, read if it is new; null after reporting an
  // error.
  Module *resolve(Module *from, llvm::StringRef spec, llvm::SMLoc loc);
  // The file of a package an import names by the package's name.
  Module *resolvePackage(Module *from, llvm::StringRef spec, llvm::SMLoc loc);
  // The first of `candidates` (relative to `baseDir`) that exists, read if it
  // is new, as a file of package `package`; null after reporting that none
  // does.
  Module *load(llvm::StringRef baseDir, const std::vector<std::string> &candidates,
               llvm::StringRef spec, llvm::SMLoc loc, int package);
  // `realPath` relative to the entry's directory, with '/'.
  std::string relativeName(llvm::StringRef realPath) const;
  // `/// <reference path="...">` before the code: TypeScript's way to join
  // files without modules.
  void rejectReferences(llvm::StringRef text);
  // A built-in module of tz written in tz (Module::builtin): `file` of the
  // directory lib next to the compiler, known to the program as `name`;
  // read once. Null after reporting an error at `loc`.
  Module *libraryModule(llvm::StringRef file, llvm::StringRef name, llvm::SMLoc loc);
  // The globals of the web platform that tz has written in tz (see
  // kLibraryGlobals): EventTarget, AbortController and the others of
  // tz:web, URL of tz:url, fetch, Response and the others of tz:fetch. A file
  // that names one gets it as if it imported it, unless it declares or
  // imports that name itself.
  void importLibraryGlobals(Module *module, llvm::StringRef text);
  // The streams of node:fs and of process, written in tz (src/lib/fs_stream.tz):
  // the names createReadStream, createWriteStream, ReadStream and WriteStream
  // that a file imports from node:fs come from there, and a file that uses
  // process.stdin, process.stdout or process.stderr as a stream loads it.
  void importFsStreams(Module *module, llvm::StringRef text);

  llvm::SourceMgr &sourceMgr_;
  AstContext &ctx_;
  Diagnostics &diags_;
  // The project's packages (packages[0] is the project), or null without a
  // tz.json.
  const pkg::Graph *packages_;
  llvm::DenseMap<const Module *, int> packageOf_; // -1 without a tz.json
  std::string entryDir_;     // as given
  std::string entryRealDir_; // canonical
  llvm::StringMap<Module *> byRealPath_;
  llvm::StringMap<Module *> libraries_; // by name, null for one that cannot be read
  llvm::DenseMap<const Module *, std::string> realPaths_;
  llvm::DenseMap<const Module *, State> states_;
  std::vector<Module *> loading_;
  std::vector<Module *> order_;
};

bool Loader::run(llvm::StringRef entryPath, SourceFile &program) {
  if (path::extension(entryPath) != ".tz") {
    llvm::errs() << "tz: '" << entryPath << "' is not a file of tz: they end in .tz";
    if (path::extension(entryPath) == ".ts")
      llvm::errs() << "; rename it to " << path::stem(entryPath) << ".tz";
    llvm::errs() << "\n";
    return false;
  }
  llvm::SmallString<256> real;
  if (std::error_code ec = fs::real_path(entryPath, real)) {
    llvm::errs() << "tz: cannot read '" << entryPath << "': " << ec.message() << "\n";
    return false;
  }
  entryDir_ = path::parent_path(entryPath).str();
  entryRealDir_ = path::parent_path(real).str();
  std::string error;
  Module *entry = parse(entryPath.str(), std::string(real), path::filename(real).str(), error);
  if (!entry) {
    llvm::errs() << "tz: cannot read '" << entryPath << "': " << error << "\n";
    return false;
  }
  entry->isEntry = true;
  packageOf_[entry] = packages_ ? 0 : -1;
  visit(entry);

  for (Module *module : order_) {
    program.modules.push_back(module);
    SourceFile &file = module->file;
    program.items.insert(program.items.end(), file.items.begin(), file.items.end());
    for (const auto &name : file.fieldNames)
      program.fieldNames.insert(name.getKey());
    // A name declared in several files gets all their strings: a field write
    // through such a key may assign any of them.
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
  importLibraryGlobals(module, text);
  importFsStreams(module, text);
  byRealPath_[realPath] = module;
  realPaths_[module] = realPath;
  states_[module] = State::New;
  return module;
}

void Loader::visit(Module *module) {
  states_[module] = State::Loading;
  loading_.push_back(module);
  module->loadPath = loading_;

  // The files it names, in the order they appear in it.
  struct Request {
    const char *at;
    llvm::StringRef path;
    llvm::SMLoc loc;
    Module **from;
  };
  std::vector<Request> requests;
  for (ImportDecl &decl : module->file.imports)
    requests.push_back({decl.pathLoc.getPointer(), decl.path, decl.pathLoc, &decl.from});
  for (ExportDecl &decl : module->file.exports)
    if (!decl.path.empty())
      requests.push_back({decl.pathLoc.getPointer(), decl.path, decl.pathLoc, &decl.from});
  std::stable_sort(requests.begin(), requests.end(),
                   [](const Request &a, const Request &b) { return a.at < b.at; });

  for (const Request &request : requests) {
    // The globals written in tz come resolved (see importLibraryGlobals).
    Module *target = *request.from ? *request.from : resolve(module, request.path, request.loc);
    *request.from = target;
    // A file that is loading already is part of a cycle: it is not waited for
    // (docs/modules.md, section 4).
    if (target && states_[target] == State::New)
      visit(target);
  }

  loading_.pop_back();
  states_[module] = State::Loaded;
  module->order = static_cast<unsigned>(order_.size());
  order_.push_back(module);
}

Module *Loader::resolve(Module *from, llvm::StringRef spec, llvm::SMLoc loc) {
  // Built into tz (docs/io.md) or into the language (docs/errors.md): Sema
  // binds the names they give.
  if (spec == "neverthrow")
    return nullptr;
  // Built into tz and written in tz (src/lib, docs/events.md, docs/net.md);
  // the modules of the globals only for the code there.
  if (spec == "node:events" || spec == "events")
    return libraryModule("events.tz", "node:events", loc);
  if (spec == "tz:http")
    return libraryModule("http.tz", "tz:http", loc);
  if (from->builtin && (spec == "tz:web" || spec == "tz:url" || spec == "tz:fetch"))
    return libraryModule(spec.drop_front(3).str() + ".tz", spec, loc);
  // node:stream and the modules on it (docs/streams.md).
  if (spec == "node:stream" || spec == "stream")
    return libraryModule("stream.tz", "node:stream", loc);
  if (spec == "node:stream/promises" || spec == "stream/promises")
    return libraryModule("stream_promises.tz", "node:stream/promises", loc);
  if (spec == "node:stream/consumers" || spec == "stream/consumers")
    return libraryModule("stream_consumers.tz", "node:stream/consumers", loc);
  if (spec == "node:string_decoder" || spec == "string_decoder")
    return libraryModule("string_decoder.tz", "node:string_decoder", loc);
  if (isBuiltinModule(spec)) {
    if (std::string problem = builtinModuleProblem(spec); !problem.empty())
      diags_.error(loc, problem);
    return nullptr;
  }
  if (spec.contains('\\')) {
    diags_.error(loc, "write '/' in the path of an import, not '\\', as on every system");
    return nullptr;
  }
  bool relative = spec == "." || spec == ".." || spec.starts_with("./") || spec.starts_with("../");
  if (!relative) {
    if (spec.starts_with("jsr:") || spec.starts_with("npm:"))
      return resolvePackage(from, spec, loc);
    if (spec.empty() || path::is_absolute(spec) || spec.starts_with("/") || spec.contains(':'))
      diags_.error(loc, "an import names a file by its path relative to the importing file, e.g. "
                        "'./math' or '../lib/math'");
    else
      return resolvePackage(from, spec, loc);
    return nullptr;
  }

  // The files the path may name, in the order they are looked for.
  std::vector<std::string> candidates;
  llvm::StringRef last = path::filename(spec, path::Style::posix);
  llvm::StringRef ext = path::extension(spec, path::Style::posix);
  std::string baseDir = path::parent_path(realPaths_[from]).str();
  if (spec.ends_with("/") || last == "." || last == "..") {
    candidates.push_back((spec.rtrim('/') + "/index.tz").str());
  } else if (ext == ".tz") {
    candidates.push_back(spec.str());
  } else if (ext == ".json") {
    diags_.error(loc, "a .json file is not a module: only .tz files can be imported; embed the "
                      "data with 'const data = embedJson(\"" +
                          spec.str() + "\")'");
    return nullptr;
  } else if (std::string problem = foreignFile(baseDir, spec, spec); !problem.empty()) {
    diags_.error(loc, problem);
    return nullptr;
  } else {
    candidates.push_back((spec + ".tz").str());
    candidates.push_back((spec + "/index.tz").str());
  }

  return load(baseDir, candidates, spec, loc, packageOf_[from]);
}

Module *Loader::resolvePackage(Module *from, llvm::StringRef spec, llvm::SMLoc loc) {
  if (spec.starts_with("npm:")) {
    diags_.error(loc, "npm packages are not supported: npm has compiled JavaScript, not "
                      "TypeScript sources (docs/packages.md); look for the package on JSR or take "
                      "its sources from git");
    return nullptr;
  }
  if (!packages_) {
    diags_.error(loc, "'" + spec +
                          "' is the name of a package, but there is no tz.json to list packages "
                          "in; make one with 'tz init' and add the package with 'tz add " +
                          spec + "' (a file of the program is imported by its relative path, e.g. "
                          "'./" +
                          spec + "')");
    return nullptr;
  }
  int fromPackage = packageOf_[from];
  const pkg::Package &home = packages_->packages[fromPackage];
  std::string name, subpath;
  const unsigned *target = nullptr;
  if (spec.starts_with("jsr:")) {
    // "jsr:@std/path@^1/join", as JSR writes the imports of the packages it
    // publishes; tz.lock has made sure that the version fits.
    std::string source, inside;
    if (!pkg::splitJsrImport(spec, name, source, inside)) {
      diags_.error(loc, "'" + spec + "' does not name a JSR package: 'jsr:@scope/name@^1'");
      return nullptr;
    }
    subpath = "." + inside;
    if (!home.jsr) {
      diags_.error(loc, "import a package by its name in tz.json: add it with 'tz add jsr:" + name +
                            "' and write 'from \"" + name +
                            (subpath == "." ? std::string() : subpath.substr(1)) + "\"'");
      return nullptr;
    }
    auto it = home.jsrDeps.find(name);
    if (it != home.jsrDeps.end())
      target = &it->second;
  } else {
    if (!pkg::splitPackageSpecifier(spec, name, subpath)) {
      diags_.error(loc, "'" + spec + "' does not name a package: a scoped name is '@scope/name'");
      return nullptr;
    }
    auto it = home.aliases.find(name);
    if (it != home.aliases.end())
      target = &it->second;
  }
  if (!target) {
    if (fromPackage == 0)
      diags_.error(loc, "'" + name + "' is not a dependency in " + home.manifestPath +
                            "; add it with 'tz add " + name + "'");
    else if (home.jsr)
      diags_.error(loc, "'" + name + "' is not a dependency of " + home.name +
                            ": a JSR package imports another one as 'jsr:@scope/name@^1'");
    else
      diags_.error(loc, "'" + name + "' is not a dependency of package " + home.name +
                            (home.manifestPath.empty() ? std::string(", which has no tz.json")
                                                       : " (" + home.manifestPath + ")"));
    return nullptr;
  }
  const pkg::Package &package = packages_->packages[*target];
  int index = static_cast<int>(*target);

  // What the package exports under that path, or, if it does not say, the
  // file of the path.
  std::vector<std::string> candidates;
  if (!package.exports.empty()) {
    std::string listed;
    for (const auto &[key, file] : package.exports) {
      if (key == subpath)
        candidates.push_back(file);
      listed += (listed.empty() ? "'" : ", '") + name +
                (key == "." ? std::string() : key.substr(1)) + "'";
    }
    if (candidates.empty()) {
      diags_.error(loc, "'" + spec + "' names nothing " + name + " exports; it exports " + listed);
      return nullptr;
    }
  } else if (subpath == ".") {
    if (!package.main.empty())
      candidates.push_back(package.main);
    else
      candidates = {"mod.tz", "index.tz", "src/index.tz", "src/mod.tz"};
  } else {
    llvm::StringRef file = llvm::StringRef(subpath).drop_front(2);
    if (file.ends_with(".tz")) {
      candidates.push_back(file.str());
    } else if (std::string problem = foreignFile(package.dir, file, spec); !problem.empty()) {
      diags_.error(loc, problem);
      return nullptr;
    } else {
      candidates.push_back((file + ".tz").str());
      candidates.push_back((file + "/index.tz").str());
    }
  }
  return load(package.dir, candidates, spec, loc, index);
}

Module *Loader::load(llvm::StringRef baseDir, const std::vector<std::string> &candidates,
                     llvm::StringRef spec, llvm::SMLoc loc, int package) {
  const pkg::Package *owner = packages_ && package > 0 ? &packages_->packages[package] : nullptr;
  bool byName = !spec.starts_with(".");
  for (const std::string &candidate : candidates) {
    llvm::SmallString<256> full(baseDir);
    path::append(full, candidate);
    path::remove_dots(full, /*remove_dot_dot=*/true);
    if (!fs::is_regular_file(full))
      continue;
    llvm::SmallString<256> real;
    if (fs::real_path(full, real))
      real = full;
    // On a system that ignores the case of names the file is found anyway;
    // elsewhere it would not be.
    if (real.str() != full.str() && real.str().equals_insensitive(full)) {
      diags_.error(loc, "the file is named '" + relativeName(real) + "', but the import says '" +
                            spec +
                            "'; write the name with the same capital letters, or the program will "
                            "not build where names are case-sensitive");
      return nullptr;
    }
    if (Module *known = byRealPath_.lookup(real))
      return known;
    if (path::extension(real) != ".tz") {
      diags_.error(loc, "'" + spec + "' names " + path::filename(real) +
                            ", and the files of tz end in .tz: rename it to " +
                            path::stem(real) + ".tz");
      return nullptr;
    }
    std::string name = relativeName(real);
    std::string filePath = name;
    if (owner) {
      // A file of a package is named after the package: "@std/path/join.tz".
      llvm::StringRef inside = real.str();
      bool within = inside.consume_front(owner->dir) &&
                    (inside.starts_with("/") || inside.starts_with("\\"));
      if (!within && owner->external) {
        diags_.error(loc, "'" + spec + "' is outside package " + owner->name +
                              ": a package imports only its own files and its dependencies");
        return nullptr;
      }
      if (within)
        name = owner->name + "/" + path::convert_to_slash(inside.drop_front());
      filePath = std::string(real);
    } else if (!path::is_absolute(name)) {
      llvm::SmallString<256> joined(entryDir_);
      path::append(joined, name);
      path::native(joined);
      filePath = std::string(joined);
    }
    std::string error;
    Module *module = parse(filePath, std::string(real), name, error);
    if (!module)
      diags_.error(loc, "cannot read '" + name + "': " + error);
    else
      packageOf_[module] = package;
    return module;
  }

  if (owner && byName) {
    // What the package names (main, exports), a path inside it, or the files
    // a package without a tz.json may start with.
    if (candidates.size() == 1)
      diags_.error(loc, "cannot find '" + candidates[0] + "' in package " + owner->name + " (" +
                            owner->dir + "), which '" + spec + "' names");
    else if (candidates.size() == 2)
      diags_.error(loc, "cannot find '" + spec + "': package " + owner->name + " has no " +
                            candidates[0] + " or " + candidates[1] + " (in " + owner->dir + ")");
    else
      diags_.error(loc, "package " + owner->name +
                            " names no file to import: it has no tz.json with \"main\" or "
                            "\"exports\", and none of " +
                            llvm::join(candidates, ", ") + " (in " + owner->dir + ")");
    return nullptr;
  }

  std::string message = "cannot find '" + spec.str() + "': there is no ";
  std::string typescript;
  for (size_t i = 0; i < candidates.size(); ++i) {
    llvm::StringRef shown(candidates[i]);
    if (shown.starts_with("./"))
      shown = shown.drop_front(2);
    message += (i ? " or " : "") + shown.str();
    // The same file of TypeScript, which only needs another name.
    llvm::SmallString<256> ts(baseDir);
    path::append(ts, shown.drop_back(3) + ".ts");
    if (typescript.empty() && fs::is_regular_file(ts))
      typescript = path::convert_to_slash(shown.drop_back(3)) + ".ts";
  }
  if (!typescript.empty())
    message += "; there is " + typescript + ", and the files of tz end in .tz: rename it to " +
               typescript.substr(0, typescript.size() - 3) + ".tz";
  diags_.error(loc, message);
  return nullptr;
}

Module *Loader::libraryModule(llvm::StringRef file, llvm::StringRef name, llvm::SMLoc loc) {
  auto [it, inserted] = libraries_.try_emplace(name, nullptr);
  if (!inserted)
    return it->second;
  std::string self = llvm::sys::fs::getMainExecutable(nullptr, (void *)(intptr_t)&loadProgram);
  llvm::SmallString<256> path(path::parent_path(self));
  path::append(path, "lib", file);
  auto buffer = llvm::MemoryBuffer::getFile(path);
  if (!buffer) {
    diags_.error(loc, "cannot read '" + std::string(path) + "', the code of '" + name +
                          "' that tz has built in: " + buffer.getError().message() +
                          "; reinstall tz");
    return nullptr;
  }
  // Errors and the places of stack traces name the module as node names
  // its own, `node:events:12:5`, `tz:http:12:5`.
  unsigned id = sourceMgr_.AddNewSourceBuffer(
      llvm::MemoryBuffer::getMemBufferCopy((*buffer)->getBuffer(), name), llvm::SMLoc());
  llvm::StringRef text = sourceMgr_.getMemoryBuffer(id)->getBuffer();
  Module *module = ctx_.newModule();
  module->path = std::string(path);
  module->name = name.str();
  module->builtin = true;
  Lexer lexer(text, diags_);
  module->file = Parser(lexer, ctx_, diags_, /*library=*/true).parseSourceFile();
  realPaths_[module] = std::string(path);
  states_[module] = State::New;
  it->second = module;
  return module;
}

namespace {

// The globals of the web platform that tz has written in tz, and the files
// of src/lib that declare them.
struct LibraryGlobal {
  llvm::StringLiteral name, file, module;
};
constexpr LibraryGlobal kLibraryGlobals[] = {
    {"EventTarget", "web.tz", "tz:web"},
    {"Event", "web.tz", "tz:web"},
    {"CustomEvent", "web.tz", "tz:web"},
    {"AbortController", "web.tz", "tz:web"},
    {"AbortSignal", "web.tz", "tz:web"},
    {"DOMException", "web.tz", "tz:web"},
    {"EventListenerObject", "web.tz", "tz:web"},
    {"URL", "url.tz", "tz:url"},
    {"URLSearchParams", "url.tz", "tz:url"},
    {"Headers", "fetch.tz", "tz:fetch"},
    {"Request", "fetch.tz", "tz:fetch"},
    {"Response", "fetch.tz", "tz:fetch"},
    {"ReadableStream", "fetch.tz", "tz:fetch"},
    {"ReadableStreamDefaultReader", "fetch.tz", "tz:fetch"},
    {"ReadableStreamDefaultController", "fetch.tz", "tz:fetch"},
    {"FormData", "fetch.tz", "tz:fetch"},
    {"fetch", "fetch.tz", "tz:fetch"},
    // The types that go with them, global in TypeScript too.
    {"RequestInit", "fetch.tz", "tz:fetch"},
    {"ResponseInit", "fetch.tz", "tz:fetch"},
    {"HeadersInit", "fetch.tz", "tz:fetch"},
    {"BodyInit", "fetch.tz", "tz:fetch"},
    {"RequestRedirect", "fetch.tz", "tz:fetch"},
    {"ReadableStreamReadResult", "fetch.tz", "tz:fetch"},
    {"UnderlyingSource", "fetch.tz", "tz:fetch"},
    // Those of Web Crypto: the types of TypeScript's lib.dom that its
    // functions take and give.
    {"CryptoKey", "webcrypto.tz", "tz:webcrypto"},
    {"CryptoKeyPair", "webcrypto.tz", "tz:webcrypto"},
    {"SubtleCrypto", "webcrypto.tz", "tz:webcrypto"},
    {"JsonWebKey", "webcrypto.tz", "tz:webcrypto"},
    {"RsaOtherPrimesInfo", "webcrypto.tz", "tz:webcrypto"},
    {"BufferSource", "webcrypto.tz", "tz:webcrypto"},
    {"KeyUsage", "webcrypto.tz", "tz:webcrypto"},
    {"KeyFormat", "webcrypto.tz", "tz:webcrypto"},
    {"KeyType", "webcrypto.tz", "tz:webcrypto"},
    {"NamedCurve", "webcrypto.tz", "tz:webcrypto"},
    {"BigInteger", "webcrypto.tz", "tz:webcrypto"},
    {"Algorithm", "webcrypto.tz", "tz:webcrypto"},
    {"AlgorithmIdentifier", "webcrypto.tz", "tz:webcrypto"},
    {"HashAlgorithmIdentifier", "webcrypto.tz", "tz:webcrypto"},
    {"KeyAlgorithm", "webcrypto.tz", "tz:webcrypto"},
    {"AesKeyAlgorithm", "webcrypto.tz", "tz:webcrypto"},
    {"HmacKeyAlgorithm", "webcrypto.tz", "tz:webcrypto"},
    {"EcKeyAlgorithm", "webcrypto.tz", "tz:webcrypto"},
    {"RsaKeyAlgorithm", "webcrypto.tz", "tz:webcrypto"},
    {"RsaHashedKeyAlgorithm", "webcrypto.tz", "tz:webcrypto"},
    {"AesCbcParams", "webcrypto.tz", "tz:webcrypto"},
    {"AesCtrParams", "webcrypto.tz", "tz:webcrypto"},
    {"AesGcmParams", "webcrypto.tz", "tz:webcrypto"},
    {"AesKeyGenParams", "webcrypto.tz", "tz:webcrypto"},
    {"AesDerivedKeyParams", "webcrypto.tz", "tz:webcrypto"},
    {"HmacImportParams", "webcrypto.tz", "tz:webcrypto"},
    {"HmacKeyGenParams", "webcrypto.tz", "tz:webcrypto"},
    {"EcKeyGenParams", "webcrypto.tz", "tz:webcrypto"},
    {"EcKeyImportParams", "webcrypto.tz", "tz:webcrypto"},
    {"EcdsaParams", "webcrypto.tz", "tz:webcrypto"},
    {"EcdhKeyDeriveParams", "webcrypto.tz", "tz:webcrypto"},
    {"RsaKeyGenParams", "webcrypto.tz", "tz:webcrypto"},
    {"RsaHashedKeyGenParams", "webcrypto.tz", "tz:webcrypto"},
    {"RsaHashedImportParams", "webcrypto.tz", "tz:webcrypto"},
    {"RsaPssParams", "webcrypto.tz", "tz:webcrypto"},
    {"RsaOaepParams", "webcrypto.tz", "tz:webcrypto"},
    {"Pbkdf2Params", "webcrypto.tz", "tz:webcrypto"},
    {"HkdfParams", "webcrypto.tz", "tz:webcrypto"},
};

} // namespace

void Loader::importLibraryGlobals(Module *module, llvm::StringRef text) {
  // Where the file first names each, as an identifier of its code (not in
  // a comment or a string, nor a property after `.`): the tokens tell, and
  // the parser reported the syntax. `crypto.subtle` names the object of
  // tz:webcrypto too, which Sema reads under a name of its own.
  llvm::StringMap<const char *> mentions;
  const char *subtle = nullptr;
  bool muted = diags_.setMuted(true);
  Lexer lexer(text, diags_);
  bool property = false;
  Token before[2];
  for (Token token = lexer.next(); !token.is(Tok::Eof); token = lexer.next()) {
    if (token.is(Tok::Identifier) && !property &&
        llvm::any_of(kLibraryGlobals,
                     [&](const LibraryGlobal &global) { return global.name == token.text; }))
      mentions.try_emplace(token.text, token.text.data());
    if (!subtle && token.is(Tok::Identifier) && token.text == "subtle" &&
        (before[1].is(Tok::Dot) || before[1].is(Tok::QuestionDot)) &&
        before[0].is(Tok::Identifier) && before[0].text == "crypto")
      subtle = token.text.data();
    property = token.is(Tok::Dot) || token.is(Tok::QuestionDot);
    before[0] = before[1];
    before[1] = token;
  }
  diags_.setMuted(muted);
  if (mentions.empty() && !subtle)
    return;
  llvm::StringSet<> own;
  for (const Stmt *item : module->file.items) {
    if (auto *cls = llvm::dyn_cast<ClassDecl>(item))
      own.insert(cls->name);
    else if (auto *fn = llvm::dyn_cast<FunctionDecl>(item))
      own.insert(fn->name);
    else if (auto *alias = llvm::dyn_cast<TypeAliasDecl>(item))
      own.insert(alias->name);
    else if (auto *e = llvm::dyn_cast<EnumDecl>(item))
      own.insert(e->name);
    else if (auto *decl = llvm::dyn_cast<VarDeclStmt>(item))
      for (const VarDeclarator &d : decl->decls)
        own.insert(d.name);
  }
  for (const ImportDecl &decl : module->file.imports) {
    for (const ModuleName &name : decl.names)
      own.insert(name.as);
    if (!decl.whole.empty())
      own.insert(decl.whole);
  }
  // One import for each module of the globals the file names.
  std::vector<ImportDecl> imports;
  auto importOf = [&](llvm::StringRef file, llvm::StringRef path,
                      llvm::SMLoc loc) -> ImportDecl * {
    auto it = llvm::find_if(imports, [&](const ImportDecl &d) { return d.path == path; });
    if (it != imports.end())
      return &*it;
    ImportDecl decl;
    decl.implicit = true;
    decl.path = path.str();
    decl.loc = decl.pathLoc = loc;
    decl.from = libraryModule(file, path, loc);
    if (!decl.from)
      return nullptr;
    imports.push_back(std::move(decl));
    return &imports.back();
  };
  for (const LibraryGlobal &global : kLibraryGlobals) {
    const char *at = own.contains(global.name) ? nullptr : mentions.lookup(global.name);
    if (!at)
      continue;
    llvm::SMLoc loc = llvm::SMLoc::getFromPointer(at);
    if (ImportDecl *decl = importOf(global.file, global.module, loc))
      decl->names.push_back({global.name, loc, global.name, loc});
  }
  // `crypto.subtle`: the object of tz:webcrypto, which Sema reads under a
  // name of its own.
  if (subtle) {
    llvm::SMLoc loc = llvm::SMLoc::getFromPointer(subtle);
    if (ImportDecl *decl = importOf("webcrypto.tz", "tz:webcrypto", loc))
      decl->names.push_back({"subtle", loc, kSubtleName, loc});
  }
  for (ImportDecl &decl : imports)
    module->file.imports.push_back(std::move(decl));
}

void Loader::importFsStreams(Module *module, llvm::StringRef text) {
  static constexpr llvm::StringLiteral kNames[] = {"createReadStream", "createWriteStream",
                                                    "ReadStream", "WriteStream"};
  ImportDecl streams;
  streams.implicit = true;
  for (ImportDecl &decl : module->file.imports) {
    if (decl.path != "node:fs" && decl.path != "fs")
      continue;
    for (size_t i = 0; i < decl.names.size();) {
      if (!llvm::is_contained(kNames, decl.names[i].name)) {
        ++i;
        continue;
      }
      if (!streams.loc.isValid())
        streams.loc = streams.pathLoc = decl.pathLoc;
      streams.names.push_back(decl.names[i]);
      decl.names.erase(decl.names.begin() + long(i));
    }
  }
  // `process.stdin`, and `process.stdout` or `process.stderr` but in
  // `.write(chunk)` and `.isTTY`, and but the input and the output of
  // readline: the tokens tell, as for the web globals. A write with an
  // encoding or a callback needs the stream. The same of the names that
  // node:process gives them (`import { stdin } from "node:process"`).
  llvm::StringMap<llvm::StringRef> imported; // the name in the file: stdin, stdout or stderr
  llvm::SmallPtrSet<const char *, 4> importedAt;
  for (const ImportDecl &decl : module->file.imports) {
    if (decl.path != "node:process" && decl.path != "process")
      continue;
    for (const ModuleName &n : decl.names)
      if (n.name == "stdin" || n.name == "stdout" || n.name == "stderr") {
        imported[n.as] = n.name;
        importedAt.insert(n.loc.getPointer());
        importedAt.insert(n.asLoc.getPointer());
      }
  }
  if (streams.names.empty()) {
    bool muted = diags_.setMuted(true);
    Lexer lexer(text, diags_);
    std::vector<Token> tokens;
    for (Token token = lexer.next(); !token.is(Tok::Eof); token = lexer.next())
      tokens.push_back(token);
    diags_.setMuted(muted);
    auto is = [&](size_t i, Tok kind, llvm::StringRef text) {
      return i < tokens.size() && tokens[i].is(kind) && (text.empty() || tokens[i].text == text);
    };
    // Whether the call whose `(` is at `i` has more than one argument.
    auto severalArgs = [&](size_t i) {
      if (!is(i, Tok::LParen, ""))
        return false;
      int depth = 0;
      for (size_t j = i; j < tokens.size(); ++j) {
        Tok kind = tokens[j].kind;
        if (kind == Tok::LParen || kind == Tok::LBracket || kind == Tok::LBrace ||
            kind == Tok::TemplateHead)
          ++depth;
        else if (kind == Tok::RParen || kind == Tok::RBracket || kind == Tok::RBrace ||
                 kind == Tok::TemplateTail)
          --depth;
        if (depth == 0)
          return false;
        if (depth == 1 && kind == Tok::Comma)
          return true;
      }
      return false;
    };
    for (size_t at = 0; at < tokens.size() && !streams.loc.isValid(); ++at) {
      if (!is(at, Tok::Identifier, "") || (at > 0 && is(at - 1, Tok::Dot, "")))
        continue;
      // The token of the stream: `process.stdout`, or `stdout` imported.
      size_t last = at;
      llvm::StringRef stream;
      if (tokens[at].text == "process" && is(at + 1, Tok::Dot, "") &&
          is(at + 2, Tok::Identifier, "")) {
        last = at + 2;
        stream = tokens[last].text;
      } else if (auto found = imported.find(tokens[at].text);
                 found != imported.end() && !importedAt.contains(tokens[at].text.data())) {
        stream = found->second;
      }
      if (stream != "stdin" && stream != "stdout" && stream != "stderr")
        continue;
      bool member = is(last + 1, Tok::Dot, "") && is(last + 2, Tok::Identifier, "");
      llvm::StringRef name = member ? tokens[last + 2].text : llvm::StringRef();
      if (stream != "stdin" && (name == "isTTY" || (name == "write" && !severalArgs(last + 3))))
        continue;
      // `{ input: process.stdin, output: process.stdout }` of readline.
      if (at >= 2 && is(at - 1, Tok::Colon, "") &&
          (is(at - 2, Tok::Identifier, "input") || is(at - 2, Tok::Identifier, "output")))
        continue;
      streams.loc = streams.pathLoc = llvm::SMLoc::getFromPointer(tokens[at].text.data());
    }
  }
  if (!streams.loc.isValid())
    return;
  streams.path = "node:internal/fs/streams";
  streams.from = libraryModule("fs_stream.tz", "node:internal/fs/streams", streams.loc);
  if (streams.from)
    module->file.imports.push_back(std::move(streams));
}

std::string Loader::relativeName(llvm::StringRef realPath) const {
  std::vector<llvm::StringRef> base(path::begin(entryRealDir_), path::end(entryRealDir_));
  std::vector<llvm::StringRef> target(path::begin(realPath), path::end(realPath));
  size_t common = 0;
  while (common < base.size() && common < target.size() && base[common] == target[common])
    ++common;
  if (common == 0) // another drive
    return path::convert_to_slash(realPath);
  std::string name;
  for (size_t i = common; i < base.size(); ++i)
    name += "../";
  for (size_t i = common; i < target.size(); ++i)
    name += target[i].str() + (i + 1 < target.size() ? "/" : "");
  return name;
}

void Loader::rejectReferences(llvm::StringRef text) {
  for (size_t at = 0; at < text.size();) {
    size_t end = text.find('\n', at);
    llvm::StringRef line = text.slice(at, end).ltrim();
    if (line.consume_front("///") && line.ltrim().starts_with("<reference"))
      diags_.error(llvm::SMLoc::getFromPointer(line.data() - 3),
                   "'/// <reference>' is not supported: files are joined by imports; import what "
                   "the file needs by name, e.g. 'import { a } from \"./x\"'");
    if (end == llvm::StringRef::npos)
      break;
    at = end + 1;
  }
}

} // namespace

bool loadProgram(llvm::StringRef entryPath, llvm::SourceMgr &sourceMgr, AstContext &ctx,
                 Diagnostics &diags, SourceFile &program, const pkg::Graph *packages) {
  return Loader(sourceMgr, ctx, diags, packages).run(entryPath, program);
}

} // namespace tz
