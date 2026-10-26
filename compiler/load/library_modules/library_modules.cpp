#include "compiler/load/library_modules/library_modules.h"
#include "compiler/check/checker/checker.h"
#include "compiler/load/lexer/lexer.h"
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

namespace fs = llvm::sys::fs;
namespace path = llvm::sys::path;

namespace tz {

namespace {

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
    {"RequestInit", "fetch.tz", "tz:fetch"},
    {"ResponseInit", "fetch.tz", "tz:fetch"},
    {"HeadersInit", "fetch.tz", "tz:fetch"},
    {"BodyInit", "fetch.tz", "tz:fetch"},
    {"RequestRedirect", "fetch.tz", "tz:fetch"},
    {"ReadableStreamReadResult", "fetch.tz", "tz:fetch"},
    {"UnderlyingSource", "fetch.tz", "tz:fetch"},
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

struct ModuleParts {
  llvm::StringLiteral file;
  std::vector<llvm::StringLiteral> parts;
};
const ModuleParts kModuleParts[] = {
    {"stream.tz",
     {"stream_readable.tz", "stream_writable.tz", "stream_duplex.tz", "stream_pipeline.tz"}},
};

void appendPart(SourceFile &file, SourceFile &part) {
  llvm::append_range(file.items, part.items);
  llvm::append_range(file.imports, part.imports);
  llvm::append_range(file.exports, part.exports);
  for (const auto &name : part.fieldNames) {
    file.fieldNames.insert(name.getKey());
  }
  for (auto &entry : part.stringConstants) {
    llvm::append_range(file.stringConstants[entry.getKey()], entry.getValue());
  }
}

}

LibraryModules::LibraryModules(Loader &loader) : loader_(loader) {}

std::optional<SourceFile> LibraryModules::parseLibraryFile(llvm::StringRef file,
                                                           llvm::StringRef name, llvm::SMLoc loc,
                                                           std::string &fullPath,
                                                           std::vector<unsigned> &buffers) {
  std::string self = llvm::sys::fs::getMainExecutable(nullptr, (void *)(intptr_t)&loadProgram);
  llvm::SmallString<256> path(path::parent_path(self));
  path::append(path, "lib", file);
  fullPath = std::string(path);
  auto buffer = llvm::MemoryBuffer::getFile(path);
  if (!buffer) {
    loader_.diags_.error(loc, "cannot read '" + std::string(path) + "', the code of '" + name +
                                  "' that tz has built in: " + buffer.getError().message() +
                                  "; reinstall tz");
    return std::nullopt;
  }
  unsigned id = loader_.sourceMgr_.AddNewSourceBuffer(
      llvm::MemoryBuffer::getMemBufferCopy((*buffer)->getBuffer(), name), llvm::SMLoc());
  buffers.push_back(id);
  llvm::StringRef text = loader_.sourceMgr_.getMemoryBuffer(id)->getBuffer();
  Lexer lexer(text, loader_.diags_);

  return Parser(lexer, loader_.ctx_, loader_.diags_, true).parseSourceFile();
}

Module *LibraryModules::libraryModule(llvm::StringRef file, llvm::StringRef name, llvm::SMLoc loc) {
  auto [it, inserted] = loader_.libraries_.try_emplace(name, nullptr);
  if (!inserted) {
    return it->second;
  }
  std::string path;
  std::vector<unsigned> buffers;
  std::optional<SourceFile> parsed = parseLibraryFile(file, name, loc, path, buffers);
  if (!parsed) {
    return nullptr;
  }
  for (const ModuleParts &m : kModuleParts) {
    if (m.file != file) {
      continue;
    }
    for (llvm::StringRef part : m.parts) {
      std::string partPath;
      std::optional<SourceFile> rest = parseLibraryFile(part, name, loc, partPath, buffers);
      if (!rest) {
        return nullptr;
      }
      appendPart(*parsed, *rest);
    }
  }
  Module *module = loader_.ctx_.newModule();
  module->path = path;
  module->name = name.str();
  module->builtin = true;
  module->buffers = std::move(buffers);
  module->file = std::move(*parsed);
  loader_.realPaths_[module] = path;
  loader_.states_[module] = Loader::State::New;
  it->second = module;

  return module;
}

void LibraryModules::importLibraryGlobals(Module *module, llvm::StringRef text) {
  llvm::StringMap<const char *> mentions;
  const char *subtle = nullptr;
  bool muted = loader_.diags_.setMuted(true);
  Lexer lexer(text, loader_.diags_);
  bool property = false;
  Token before[2];
  for (Token token = lexer.next(); !token.is(Tok::Eof); token = lexer.next()) {
    if (token.is(Tok::Identifier) && !property &&
        llvm::any_of(kLibraryGlobals,
                     [&](const LibraryGlobal &global) { return global.name == token.text; })) {
      mentions.try_emplace(token.text, token.text.data());
    }
    if (!subtle && token.is(Tok::Identifier) && token.text == "subtle" &&
        (before[1].is(Tok::Dot) || before[1].is(Tok::QuestionDot)) &&
        before[0].is(Tok::Identifier) && before[0].text == "crypto") {
      subtle = token.text.data();
    }
    property = token.is(Tok::Dot) || token.is(Tok::QuestionDot);
    before[0] = before[1];
    before[1] = token;
  }
  loader_.diags_.setMuted(muted);
  if (mentions.empty() && !subtle) {
    return;
  }
  llvm::StringSet<> own;
  for (const Stmt *item : module->file.items) {
    if (auto *cls = llvm::dyn_cast<ClassDecl>(item)) {
      own.insert(cls->name);
    } else if (auto *fn = llvm::dyn_cast<FunctionDecl>(item)) {
      own.insert(fn->name);
    } else if (auto *alias = llvm::dyn_cast<TypeAliasDecl>(item)) {
      own.insert(alias->name);
    } else if (auto *e = llvm::dyn_cast<EnumDecl>(item)) {
      own.insert(e->name);
    } else if (auto *decl = llvm::dyn_cast<VarDeclStmt>(item)) {
      for (const VarDeclarator &d : decl->decls) {
        own.insert(d.name);
      }
    }
  }
  for (const ImportDecl &decl : module->file.imports) {
    for (const ModuleName &name : decl.names) {
      own.insert(name.as);
    }
    if (!decl.whole.empty()) {
      own.insert(decl.whole);
    }
  }
  std::vector<ImportDecl> imports;
  auto importOf = [&](llvm::StringRef file, llvm::StringRef path, llvm::SMLoc loc) -> ImportDecl * {
    auto it = llvm::find_if(imports, [&](const ImportDecl &d) { return d.path == path; });
    if (it != imports.end()) {
      return &*it;
    }
    ImportDecl decl;
    decl.implicit = true;
    decl.path = path.str();
    decl.loc = decl.pathLoc = loc;
    decl.from = libraryModule(file, path, loc);
    if (!decl.from) {
      return nullptr;
    }
    imports.push_back(std::move(decl));

    return &imports.back();
  };
  for (const LibraryGlobal &global : kLibraryGlobals) {
    const char *at = own.contains(global.name) ? nullptr : mentions.lookup(global.name);
    if (!at) {
      continue;
    }
    llvm::SMLoc loc = llvm::SMLoc::getFromPointer(at);
    if (ImportDecl *decl = importOf(global.file, global.module, loc)) {
      decl->names.push_back({global.name, loc, global.name, loc});
    }
  }
  if (subtle) {
    llvm::SMLoc loc = llvm::SMLoc::getFromPointer(subtle);
    if (ImportDecl *decl = importOf("webcrypto.tz", "tz:webcrypto", loc)) {
      decl->names.push_back({"subtle", loc, kSubtleName, loc});
    }
  }
  for (ImportDecl &decl : imports) {
    module->file.imports.push_back(std::move(decl));
  }
}

void LibraryModules::importFsStreams(Module *module, llvm::StringRef text) {
  static constexpr llvm::StringLiteral kNames[] = {"createReadStream", "createWriteStream",
                                                   "ReadStream", "WriteStream"};
  ImportDecl streams;
  streams.implicit = true;
  for (ImportDecl &decl : module->file.imports) {
    if (decl.path != "node:fs" && decl.path != "fs") {
      continue;
    }
    for (size_t i = 0; i < decl.names.size();) {
      if (!llvm::is_contained(kNames, decl.names[i].name)) {
        ++i;
        continue;
      }
      if (!streams.loc.isValid()) {
        streams.loc = streams.pathLoc = decl.pathLoc;
      }
      streams.names.push_back(decl.names[i]);
      decl.names.erase(decl.names.begin() + long(i));
    }
  }
  llvm::StringMap<llvm::StringRef> imported;
  llvm::SmallPtrSet<const char *, 4> importedAt;
  for (const ImportDecl &decl : module->file.imports) {
    if (decl.path != "node:process" && decl.path != "process") {
      continue;
    }
    for (const ModuleName &n : decl.names) {
      if (n.name == "stdin" || n.name == "stdout" || n.name == "stderr") {
        imported[n.as] = n.name;
        importedAt.insert(n.loc.getPointer());
        importedAt.insert(n.asLoc.getPointer());
      }
    }
  }
  if (streams.names.empty()) {
    bool muted = loader_.diags_.setMuted(true);
    Lexer lexer(text, loader_.diags_);
    std::vector<Token> tokens;
    for (Token token = lexer.next(); !token.is(Tok::Eof); token = lexer.next()) {
      tokens.push_back(token);
    }
    loader_.diags_.setMuted(muted);
    auto is = [&](size_t i, Tok kind, llvm::StringRef text) {
      return i < tokens.size() && tokens[i].is(kind) && (text.empty() || tokens[i].text == text);
    };
    auto severalArgs = [&](size_t i) {
      if (!is(i, Tok::LParen, "")) {
        return false;
      }
      int depth = 0;
      for (size_t j = i; j < tokens.size(); ++j) {
        Tok kind = tokens[j].kind;
        if (kind == Tok::LParen || kind == Tok::LBracket || kind == Tok::LBrace ||
            kind == Tok::TemplateHead) {
          ++depth;
        } else if (kind == Tok::RParen || kind == Tok::RBracket || kind == Tok::RBrace ||
                   kind == Tok::TemplateTail) {
          --depth;
        }
        if (depth == 0) {
          return false;
        }
        if (depth == 1 && kind == Tok::Comma) {
          return true;
        }
      }

      return false;
    };
    for (size_t at = 0; at < tokens.size() && !streams.loc.isValid(); ++at) {
      if (!is(at, Tok::Identifier, "") || (at > 0 && is(at - 1, Tok::Dot, ""))) {
        continue;
      }
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
      if (stream != "stdin" && stream != "stdout" && stream != "stderr") {
        continue;
      }
      bool member = is(last + 1, Tok::Dot, "") && is(last + 2, Tok::Identifier, "");
      llvm::StringRef name = member ? tokens[last + 2].text : llvm::StringRef();
      if (stream != "stdin" && (name == "isTTY" || (name == "write" && !severalArgs(last + 3)))) {
        continue;
      }
      if (at >= 2 && is(at - 1, Tok::Colon, "") &&
          (is(at - 2, Tok::Identifier, "input") || is(at - 2, Tok::Identifier, "output"))) {
        continue;
      }
      streams.loc = streams.pathLoc = llvm::SMLoc::getFromPointer(tokens[at].text.data());
    }
  }
  if (!streams.loc.isValid()) {
    return;
  }
  streams.path = "node:internal/fs/streams";
  streams.from = libraryModule("fs_stream.tz", "node:internal/fs/streams", streams.loc);
  if (streams.from) {
    module->file.imports.push_back(std::move(streams));
  }
}

}
