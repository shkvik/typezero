#include "compiler/cli/port_command/port_command.h"
#include "compiler/cli/exit_codes/exit_codes.h"
#include "compiler/packages/git_client/git_client.h"
#include "compiler/packages/json_parser/json_parser.h"
#include "compiler/packages/json_text/json_text.h"
#include "compiler/packages/json_value/json_value.h"
#include "compiler/packages/jsr_client/jsr_client.h"
#include "compiler/packages/jsr_version/jsr_version.h"
#include "compiler/packages/jsr_versions/jsr_versions.h"
#include "compiler/packages/manifest/manifest.h"
#include "compiler/packages/port/file_port/file_port.h"
#include "compiler/packages/port/note/note.h"
#include "compiler/packages/port/port_context/port_context.h"
#include "compiler/packages/port/port_names/port_names.h"
#include "compiler/packages/range/range.h"
#include "compiler/packages/report/report.h"
#include "compiler/packages/scanner/scanner.h"
#include "compiler/packages/source/source.h"
#include "compiler/packages/specifier/specifier.h"
#include "compiler/packages/version/version.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

namespace tz::pkg {

namespace fs = llvm::sys::fs;
namespace path = llvm::sys::path;

namespace {

const char kSynopsis[] =
    "tz port <package> [-o <directory>] [--in-place] [--entry <file>]...\n\n"
    "  <package> is a directory, jsr:@scope/name[@range], github:user/repo[@tag] or\n"
    "  git:<address>[@tag]. The adapted copy goes to <directory> (default: the\n"
    "  package's name); --in-place changes a directory itself. --entry names the\n"
    "  files the package exports when it does not say.";

bool readFile(llvm::StringRef file, std::string &text) {
  auto buffer = llvm::MemoryBuffer::getFile(file, /*IsText=*/false,
                                            /*RequiresNullTerminator=*/false);
  if (!buffer) {
    report("cannot read '" + file + "': " + buffer.getError().message());
    return false;
  }
  text = (*buffer)->getBuffer().str();

  return true;
}

bool writeFile(llvm::StringRef file, llvm::StringRef text) {
  if (std::error_code ec = fs::create_directories(path::parent_path(file))) {
    report("cannot create '" + path::parent_path(file) + "': " + ec.message());
    return false;
  }
  std::error_code ec;
  llvm::raw_fd_ostream out(file, ec, fs::OF_None);
  if (!ec) {
    out << text;
    out.close();
    if (out.has_error()) {
      ec = out.error();
      out.clear_error();
    }
  }
  if (ec) {
    report("cannot write '" + file + "': " + ec.message());
    return false;
  }

  return true;
}

// The name of the default export of each file, before anything changes.
void findDefaultNames(const std::vector<std::string> &files, llvm::StringRef packageDir,
                      PortContext &ctx) {
  // `export { default } from "./x"` takes x's name: files are read until no
  // name changes.
  for (int round = 0; round < 4; ++round) {
    for (const std::string &file : files) {
      llvm::SmallString<256> real;
      if (fs::real_path(file, real) || ctx.defaultNames.count(std::string(real))) {
        continue;
      }
      std::string text;
      auto buffer = llvm::MemoryBuffer::getFile(file);
      if (!buffer) {
        continue;
      }
      text = (*buffer)->getBuffer().str();
      std::vector<Token> toks = tokenize(text);
      std::string name;
      for (size_t i = 0; i + 1 < toks.size() && name.empty(); ++i) {
        if (toks[i].kind != Tok::Identifier || toks[i].text != "export") {
          continue;
        }
        if (toks[i + 1].is(Tok::KwDefault)) {
          size_t k = i + 2;
          if (k < toks.size() && toks[k].text == "async") {
            ++k;
          }
          if (k < toks.size() && toks[k].text == "abstract") {
            ++k;
          }
          if (k < toks.size() && (toks[k].is(Tok::KwFunction) || toks[k].text == "class" ||
                                  toks[k].text == "interface")) {
            bool isClass = toks[k].text == "class";
            if (k + 1 < toks.size() && toks[k + 1].is(Tok::Star)) {
              ++k;
            }
            if (k + 1 < toks.size() && toks[k + 1].is(Tok::Identifier) &&
                toks[k + 1].text != "extends" && toks[k + 1].text != "implements") {
              name = toks[k + 1].text.str();
            } else {
              name = isClass ? capitalize(derivedName(file, packageDir, ctx.packageName))
                             : derivedName(file, packageDir, ctx.packageName);
            }
          } else if (k < toks.size() && toks[k].is(Tok::Identifier) &&
                     (k + 1 == toks.size() || toks[k + 1].is(Tok::Semi) ||
                      toks[k + 1].newlineBefore)) {
            name = toks[k].text.str();
          } else {
            name = derivedName(file, packageDir, ctx.packageName);
          }
        } else if (toks[i + 1].is(Tok::LBrace)) {
          size_t j = i + 2;
          for (; j < toks.size() && !toks[j].is(Tok::RBrace); ++j) {
            if (toks[j].is(Tok::KwAs) && j + 1 < toks.size() && toks[j + 1].is(Tok::KwDefault)) {
              name = toks[j - 1].is(Tok::KwDefault) ? "" : toks[j - 1].text.str();
            }
          }
          bool reexportsDefault = false;
          for (size_t k = i + 2; k < j; ++k) {
            if (toks[k].is(Tok::KwDefault) && !(k > 0 && toks[k - 1].is(Tok::KwAs)) &&
                (k + 1 == j || toks[k + 1].is(Tok::Comma) ||
                 (toks[k + 1].is(Tok::KwAs) && k + 2 < j && toks[k + 2].is(Tok::KwDefault)))) {
              reexportsDefault = true;
            }
          }
          if (reexportsDefault && j + 2 < toks.size() && toks[j + 1].text == "from") {
            std::string target =
                resolveRelative(file, stringValue(toks[j + 2]), Sources::TypeScript);
            llvm::SmallString<256> targetReal;
            if (!target.empty() && !fs::real_path(target, targetReal)) {
              if (auto it = ctx.defaultNames.find(std::string(targetReal));
                  it != ctx.defaultNames.end()) {
                name = it->second;
              }
            }
          }
        }
      }
      if (!name.empty()) {
        ctx.defaultNames[std::string(real)] = name;
      }
    }
  }
}

// What a package says it exports, as files relative to its directory.
std::vector<std::pair<std::string, std::string>> packageEntries(const std::string &dir,
                                                                const Manifest &manifest,
                                                                std::string &name,
                                                                std::string &version) {
  std::vector<std::pair<std::string, std::string>> entries = manifest.exports;
  if (entries.empty() && !manifest.main.empty()) {
    entries.push_back({".", manifest.main});
  }
  if (name.empty()) {
    name = manifest.name;
  }
  if (version.empty()) {
    version = manifest.version;
  }
  // A package for npm: its name and version, and the .ts files its
  // package.json names.
  llvm::SmallString<256> packageJson(dir);
  path::append(packageJson, "package.json");
  if (auto buffer = llvm::MemoryBuffer::getFile(packageJson)) {
    JsonValue root;
    size_t at;
    std::string error;
    if (parseJson((*buffer)->getBuffer(), root, at, error)) {
      if (name.empty()) {
        name = root.getString("name");
      }
      if (version.empty()) {
        version = root.getString("version");
      }
      for (const char *field : {"source", "main", "module", "types"}) {
        llvm::StringRef file = root.getString(field);
        if (entries.empty() && file.ends_with(".ts") && !file.ends_with(".d.ts")) {
          entries.push_back({".", file.ltrim("./").str()});
        }
      }
    }
  }
  for (const char *candidate :
       {"mod.ts", "index.ts", "src/index.ts", "src/mod.ts", "lib/index.ts"}) {
    if (!entries.empty()) {
      break;
    }
    llvm::SmallString<256> file(dir);
    path::append(file, candidate);
    if (fs::is_regular_file(file)) {
      entries.push_back({".", candidate});
    }
  }

  return entries;
}

// The sources of what `tz port` is given, in the cache or on the disk.
bool fetchSource(llvm::StringRef arg, std::string &dir, std::string &name, std::string &version,
                 std::vector<std::pair<std::string, std::string>> &exports, bool &local) {
  std::string error;
  local = false;
  if (fs::is_directory(arg)) {
    llvm::SmallString<256> real;
    if (fs::real_path(arg, real)) {
      report("cannot read '" + arg + "'");
      return false;
    }
    dir = std::string(real);
    local = true;
    return true;
  }
  std::string spec = arg.starts_with("@") ? "jsr:" + arg.str() : arg.str();
  Source source;
  if (!parseSource(spec, "", source, error)) {
    report(error);
    return false;
  }
  if (source.kind == Source::Kind::Jsr) {
    JsrVersions versions;
    if (!jsrVersions(source.scope, source.name, versions, error)) {
      report(error.empty()
                 ? "there is no package " + source.id() + " in the registry (" + jsrBaseUrl() + ")"
                 : error);
      return false;
    }
    Range range;
    Range::parse(source.range, range, error);
    const Version *best = nullptr;
    for (const JsrVersions::Entry &entry : versions.versions) {
      if (!entry.yanked &&
          (source.range.empty() ? !entry.version.isPrerelease() : range.satisfies(entry.version)) &&
          (!best || *best < entry.version)) {
        best = &entry.version;
      }
    }
    if (!best) {
      report("the registry has no version of " + source.id() + " that fits '" + source.range + "'");
      return false;
    }
    JsrVersion meta;
    version = best->str();
    if (!jsrVersion(source.scope, source.name, version, meta, error) ||
        !jsrDownload(source.scope, source.name, version, meta, dir, error)) {
      report(error);
      return false;
    }
    name = source.shortName();
    exports = meta.exports;
    return true;
  }
  if (source.kind == Source::Kind::Git) {
    std::string commit;
    if (!gitResolve(source.url, source.ref, /*refresh=*/true, commit, error) ||
        !gitCheckout(source.url, commit, dir, error)) {
      report(error);
      return false;
    }
    version = llvm::StringRef(source.ref).ltrim("v").str();
    return true;
  }
  dir = source.dir;
  local = true;

  return true;
}

// The name of the copy of a file: a.tz for a.ts.
std::string copyName(llvm::StringRef file) {
  return file.ends_with(".ts") ? (file.drop_back(3) + ".tz").str() : file.str();
}

std::string displayPath(llvm::StringRef base, llvm::StringRef relative) {
  llvm::SmallString<256> shown(base);
  path::append(shown, relative);

  return path::convert_to_slash(shown);
}

} // namespace

int PortCommand::run(llvm::ArrayRef<const char *> args) const {
  std::string sourceArg, outArg;
  std::vector<std::string> entryArgs;
  bool inPlace = false;
  for (size_t i = 0; i < args.size(); ++i) {
    llvm::StringRef arg = args[i];
    if (arg == "-o" || arg == "--entry") {
      if (i + 1 == args.size()) {
        llvm::errs() << "tz: '" << arg << "' needs a path\n\nusage: " << kSynopsis << "\n";
        return kExitUsage;
      }
      (arg == "-o" ? outArg : entryArgs.emplace_back()) = args[++i];
    } else if (arg == "--in-place") {
      inPlace = true;
    } else if (arg.starts_with("-") || !sourceArg.empty()) {
      llvm::errs() << "tz: unexpected argument '" << arg << "'\n\nusage: " << kSynopsis << "\n";
      return kExitUsage;
    } else {
      sourceArg = arg.str();
    }
  }
  if (sourceArg.empty()) {
    llvm::errs() << "tz: 'tz port' needs a package\n\nusage: " << kSynopsis << "\n";
    return kExitUsage;
  }

  std::string dir, name, version;
  std::vector<std::pair<std::string, std::string>> exports;
  bool local;
  if (!fetchSource(sourceArg, dir, name, version, exports, local)) {
    return kExitError;
  }
  // Paths are compared with the files' real paths.
  llvm::SmallString<256> realDir;
  if (!fs::real_path(dir, realDir)) {
    dir = std::string(realDir);
  }
  if (inPlace && !local) {
    report("--in-place changes a directory of this machine; '" + sourceArg + "' is not one");
    return kExitError;
  }
  Manifest manifest;
  if (!readPackageDir(dir, manifest)) {
    return kExitError;
  }
  if (exports.empty()) {
    exports = packageEntries(dir, manifest, name, version);
  } else {
    packageEntries(dir, manifest, name, version);
  }
  if (!entryArgs.empty()) {
    exports.clear();
    for (size_t i = 0; i < entryArgs.size(); ++i) {
      exports.push_back(
          {i == 0 ? std::string(".") : "./" + path::stem(entryArgs[i], path::Style::posix).str(),
           llvm::StringRef(entryArgs[i]).ltrim("./").str()});
    }
  }
  if (name.empty()) {
    name = path::filename(dir).str();
  }
  if (exports.empty()) {
    report("cannot tell which files " + name +
           " exports: it has no tz.json, jsr.json or package.json that names .ts files, and no "
           "mod.ts, index.ts or src/index.ts; name them with --entry <file>");
    return kExitError;
  }

  // Where the copy goes.
  std::string shortName = llvm::StringRef(name).substr(llvm::StringRef(name).rfind('/') + 1).str();
  // `out` as the report shows it.
  std::string out = inPlace ? sourceArg : outArg.empty() ? shortName : outArg;
  llvm::SmallString<256> outDir(inPlace ? dir : out);
  fs::make_absolute(outDir);
  if (!inPlace) {
    std::error_code ec;
    fs::directory_iterator it(outDir, ec);
    if (!ec && it != fs::directory_iterator()) {
      report("'" + out + "' is not empty; choose another directory with -o");
      return kExitError;
    }
  }

  // The files the exports reach, and the names of their default exports.
  std::vector<std::string> entries, files, external;
  for (const auto &[key, file] : exports) {
    llvm::SmallString<256> full(dir);
    path::append(full, file);
    if (!fs::is_regular_file(full)) {
      report("cannot find '" + file + "', which " + name + " exports as '" + key + "'");
      return kExitError;
    }
    entries.push_back(std::string(full));
  }
  reachableFiles(entries, files, external, Sources::TypeScript);
  PortContext ctx;
  ctx.packageName = derivedName(shortName + ".ts", "", "");
  findDefaultNames(files, dir, ctx);
  for (llvm::StringRef spec : external) {
    std::string package, source, subpath;
    if (splitJsrImport(spec, package, source, subpath)) {
      ctx.jsrDependencies[package] = source;
    }
  }

  // Port each file.
  Counts counts;
  struct Ported {
    std::string relative; // of the copy
    std::string text;
    std::vector<Note> notes;
    std::string source; // relative, of the file ported
  };
  std::vector<Ported> ported;
  for (const std::string &file : files) {
    llvm::SmallString<256> real;
    if (fs::real_path(file, real)) {
      real = file;
    }
    llvm::StringRef relative = real;
    if (!relative.consume_front(dir) ||
        !(relative.starts_with("/") || relative.starts_with("\\"))) {
      report("'" + file + "' is outside " + dir + ", and is not copied");
      continue;
    }
    std::string text;
    if (!readFile(file, text)) {
      return kExitError;
    }
    FilePort port(file, std::move(text), ctx, counts);
    port.run();
    ported.push_back({copyName(path::convert_to_slash(relative.drop_front())), port.text(),
                      std::move(port.notes()), path::convert_to_slash(relative.drop_front())});
  }
  std::sort(ported.begin(), ported.end(),
            [](const Ported &a, const Ported &b) { return a.relative < b.relative; });
  for (const Ported &p : ported) {
    llvm::SmallString<256> target(outDir);
    path::append(target, p.relative);
    if (!writeFile(target, p.text)) {
      return kExitError;
    }
    // In place, a.tz replaces a.ts.
    if (inPlace && p.source != p.relative) {
      llvm::SmallString<256> old(outDir);
      path::append(old, p.source);
      fs::remove(old);
    }
  }
  // The license and the readme go with the code.
  std::error_code ec;
  for (fs::directory_iterator it(dir, ec), end; it != end && !ec; it.increment(ec)) {
    std::string file = path::filename(it->path()).lower();
    if (!inPlace && fs::is_regular_file(it->path()) &&
        (llvm::StringRef(file).starts_with("license") ||
         llvm::StringRef(file).starts_with("licence") ||
         llvm::StringRef(file).starts_with("copying") ||
         llvm::StringRef(file).starts_with("notice") ||
         llvm::StringRef(file).starts_with("readme"))) {
      std::string text;
      llvm::SmallString<256> target(outDir);
      path::append(target, path::filename(it->path()));
      if (!readFile(it->path(), text) || !writeFile(target, text)) {
        return kExitError;
      }
    }
  }
  // A tz.json that says what the package is.
  llvm::SmallString<256> tzJson(outDir);
  path::append(tzJson, "tz.json");
  if (!inPlace || !fs::exists(tzJson)) {
    std::string json = "{\n  \"name\": " + jsonQuote(name);
    if (!version.empty()) {
      json += ",\n  \"version\": " + jsonQuote(version);
    }
    // The exports are files of the copy.
    if (exports.size() == 1 && exports[0].first == ".") {
      json += ",\n  \"main\": " + jsonQuote(copyName(exports[0].second));
    } else {
      json += ",\n  \"exports\": {";
      for (size_t i = 0; i < exports.size(); ++i) {
        json += (i ? ",\n    " : "\n    ") + jsonQuote(exports[i].first) + ": " +
                jsonQuote("./" + copyName(exports[i].second));
      }
      json += "\n  }";
    }
    json += ",\n  \"dependencies\": {";
    size_t i = 0;
    for (const auto &[alias, spec] : ctx.jsrDependencies) {
      json += (i++ ? ",\n    " : "\n    ") + jsonQuote(alias) + ": " + jsonQuote(spec);
    }
    json += i ? "\n  }\n}\n" : "}\n}\n";
    if (!writeFile(tzJson, json)) {
      return kExitError;
    }
  }

  // The report.
  llvm::outs() << "ported " << name << (version.empty() ? "" : " " + version) << " into "
               << path::convert_to_slash(out) << " (" << ported.size()
               << (ported.size() == 1 ? " file" : " files") << ")\n";
  if (!counts.empty()) {
    llvm::outs() << "changed:\n";
    for (const auto &[kind, n] : counts) {
      llvm::outs() << llvm::format("%5u  ", n) << kind << "\n";
    }
  }
  size_t manualCount = 0;
  for (const Ported &p : ported) {
    manualCount += p.notes.size();
  }
  if (manualCount) {
    llvm::outs() << "to change by hand (" << manualCount << "):\n";
  }
  for (const Ported &p : ported) {
    for (const Note &n : p.notes) {
      auto [line, column] = lineAndColumn(p.text, n.offset);
      llvm::outs() << "  " << displayPath(out, p.relative) << ":" << line << ":" << column << ": "
                   << n.message << "\n";
    }
  }
  llvm::outs() << "then 'tz build " << displayPath(out, copyName(exports[0].second))
               << "' reports what else tz does not accept\n";

  return 0;
}

} // namespace tz::pkg
