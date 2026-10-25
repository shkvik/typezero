#include "driver/loader.h"

#include "lex/lexer.h"
#include "parse/parser.h"
#include "pkg/project.h"
#include "sema/sema.h"

#include "llvm/ADT/DenseMap.h"
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
    return libraryModule("events.ts", "node:events", loc);
  if (spec == "tz:http")
    return libraryModule("http.ts", "tz:http", loc);
  if (from->builtin && (spec == "tz:web" || spec == "tz:url" || spec == "tz:fetch"))
    return libraryModule(spec.drop_front(3).str() + ".ts", spec, loc);
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
  if (spec.ends_with("/") || last == "." || last == "..") {
    candidates.push_back((spec.rtrim('/') + "/index.ts").str());
  } else if (spec.ends_with(".d.ts") || ext == ".json" || ext == ".tsx" || ext == ".mts" ||
             ext == ".cts" || ext == ".mjs" || ext == ".cjs" || ext == ".jsx") {
    diags_.error(loc, ext == ".json"
                          ? "a .json file is not a module: only .ts files can be imported; embed "
                            "the data with 'const data = embedJson(\"" +
                                spec.str() + "\")'"
                          : std::string("only .ts files can be imported"));
    return nullptr;
  } else if (ext == ".ts") {
    candidates.push_back(spec.str());
  } else if (ext == ".js") {
    // As in TypeScript for node: the .js file that math.ts compiles to.
    candidates.push_back((spec.drop_back(3) + ".ts").str());
  } else {
    candidates.push_back((spec + ".ts").str());
    candidates.push_back((spec + "/index.ts").str());
  }

  return load(path::parent_path(realPaths_[from]), candidates, spec, loc, packageOf_[from]);
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
      candidates = {"mod.ts", "index.ts", "src/index.ts", "src/mod.ts"};
  } else {
    llvm::StringRef file = llvm::StringRef(subpath).drop_front(2);
    if (file.ends_with(".ts")) {
      candidates.push_back(file.str());
    } else if (file.ends_with(".js")) {
      candidates.push_back((file.drop_back(3) + ".ts").str());
    } else {
      candidates.push_back((file + ".ts").str());
      candidates.push_back((file + "/index.ts").str());
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
    std::string name = relativeName(real);
    std::string filePath = name;
    if (owner) {
      // A file of a package is named after the package: "@std/path/join.ts".
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
  for (size_t i = 0; i < candidates.size(); ++i) {
    llvm::StringRef shown(candidates[i]);
    if (shown.starts_with("./"))
      shown = shown.drop_front(2);
    message += (i ? " or " : "") + shown.str();
  }
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
    {"EventTarget", "web.ts", "tz:web"},
    {"Event", "web.ts", "tz:web"},
    {"CustomEvent", "web.ts", "tz:web"},
    {"AbortController", "web.ts", "tz:web"},
    {"AbortSignal", "web.ts", "tz:web"},
    {"DOMException", "web.ts", "tz:web"},
    {"EventListenerObject", "web.ts", "tz:web"},
    {"URL", "url.ts", "tz:url"},
    {"URLSearchParams", "url.ts", "tz:url"},
    {"Headers", "fetch.ts", "tz:fetch"},
    {"Request", "fetch.ts", "tz:fetch"},
    {"Response", "fetch.ts", "tz:fetch"},
    {"ReadableStream", "fetch.ts", "tz:fetch"},
    {"ReadableStreamDefaultReader", "fetch.ts", "tz:fetch"},
    {"ReadableStreamDefaultController", "fetch.ts", "tz:fetch"},
    {"FormData", "fetch.ts", "tz:fetch"},
    {"fetch", "fetch.ts", "tz:fetch"},
    // The types that go with them, global in TypeScript too.
    {"RequestInit", "fetch.ts", "tz:fetch"},
    {"ResponseInit", "fetch.ts", "tz:fetch"},
    {"HeadersInit", "fetch.ts", "tz:fetch"},
    {"BodyInit", "fetch.ts", "tz:fetch"},
    {"RequestRedirect", "fetch.ts", "tz:fetch"},
    {"ReadableStreamReadResult", "fetch.ts", "tz:fetch"},
    {"UnderlyingSource", "fetch.ts", "tz:fetch"},
};

} // namespace

void Loader::importLibraryGlobals(Module *module, llvm::StringRef text) {
  // Where the file first names each, as an identifier of its code (not in
  // a comment or a string, nor a property after `.`): the tokens tell, and
  // the parser reported the syntax.
  llvm::StringMap<const char *> mentions;
  bool muted = diags_.setMuted(true);
  Lexer lexer(text, diags_);
  bool property = false;
  for (Token token = lexer.next(); !token.is(Tok::Eof); token = lexer.next()) {
    if (token.is(Tok::Identifier) && !property &&
        llvm::any_of(kLibraryGlobals,
                     [&](const LibraryGlobal &global) { return global.name == token.text; }))
      mentions.try_emplace(token.text, token.text.data());
    property = token.is(Tok::Dot) || token.is(Tok::QuestionDot);
  }
  diags_.setMuted(muted);
  if (mentions.empty())
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
  for (const LibraryGlobal &global : kLibraryGlobals) {
    const char *at = own.contains(global.name) ? nullptr : mentions.lookup(global.name);
    if (!at)
      continue;
    llvm::SMLoc loc = llvm::SMLoc::getFromPointer(at);
    auto it = llvm::find_if(imports, [&](const ImportDecl &d) { return d.path == global.module; });
    if (it == imports.end()) {
      ImportDecl decl;
      decl.implicit = true;
      decl.path = global.module.str();
      decl.loc = decl.pathLoc = loc;
      decl.from = libraryModule(global.file, global.module, loc);
      if (!decl.from)
        continue;
      imports.push_back(std::move(decl));
      it = imports.end() - 1;
    }
    it->names.push_back({global.name, loc, global.name, loc});
  }
  for (ImportDecl &decl : imports)
    module->file.imports.push_back(std::move(decl));
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
