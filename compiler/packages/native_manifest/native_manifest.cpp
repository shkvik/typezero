#include "compiler/packages/native_manifest/native_manifest.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Path.h"
#include <functional>

namespace tz::pkg {

NativeManifest::NativeManifest(llvm::StringRef dir, llvm::StringRef path)
    : packageDir(dir.str()),
      manifestPath(path.str()) {}

NativeManifest::ResultEntriesRead NativeManifest::readEntries(const JsonValue &value) const {
  if (!value.isArray()) {
    auto message = ErrorTextNative::notList();

    return err(NativeManifestError{
        .code = NativeManifestErrorCode::NOT_A_LIST,
        .message = message,
        .offset = value.begin,
    });
  }
  auto entries = std::vector<NativeEntry>();
  for (const JsonValue &item : value.items()) {
    auto entry = this->readEntry(item);
    if (entry.isErr()) {
      return err(entry.error());
    }
    entries.push_back(entry.value());
  }

  return ok(entries);
}

NativeManifest::ResultEntryRead NativeManifest::readEntry(const JsonValue &value) const {
  if (!value.isObject()) {
    auto message = ErrorTextNative::notObject();

    return err(NativeManifestError{
        .code = NativeManifestErrorCode::NOT_AN_OBJECT,
        .message = message,
        .offset = value.begin,
    });
  }
  auto entry = NativeEntry{
      .dir = this->packageDir,
      .manifestPath = this->manifestPath,
      .offset = value.begin,
  };
  for (size_t index = 0; index < value.keys().size(); ++index) {
    auto field = this->readField(entry, value.keys()[index], value.keyOffsets()[index],
                                 value.items()[index]);
    if (field.isErr()) {
      return err(field.error());
    }
  }
  auto required = this->checkRequired(entry, value);
  if (required.isErr()) {
    return err(required.error());
  }
  auto kind = this->checkKind(entry, value);
  if (kind.isErr()) {
    return err(kind.error());
  }
  auto kindFields = this->checkKindFields(entry, value);
  if (kindFields.isErr()) {
    return err(kindFields.error());
  }

  return ok(entry);
}

NativeManifest::ResultFieldRead NativeManifest::readField(NativeEntry &entry, llvm::StringRef key,
                                                          size_t keyAt,
                                                          const JsonValue &value) const {
  if (key == "name" || key == "license" || key == "licenseFile") {
    auto text = this->readText(key, value);
    if (text.isErr()) {
      return err(text.error());
    }
    std::string &field = key == "name"      ? entry.name
                         : key == "license" ? entry.license
                                            : entry.licenseFile;
    field = key == "licenseFile" ? this->resolvePath(text.value()) : text.value();
    return ok();
  }
  if (key == "sources") {
    return this->readFiles(entry.sources, key, value);
  }
  if (key == "libraries") {
    return this->readFiles(entry.libraries, key, value);
  }
  if (key == "includes") {
    return this->readFiles(entry.includes, key, value);
  }
  if (key == "dlls") {
    return this->readFiles(entry.dlls, key, value);
  }
  if (key == "defines") {
    return this->readDefines(entry.defines, key, value);
  }
  if (key == "systemLibraries") {
    auto texts = this->readTexts(key, value);
    if (texts.isErr()) {
      return err(texts.error());
    }
    entry.systemLibraries = texts.value();
    return ok();
  }
  if (key == "os") {
    return this->readSystems(entry, value);
  }
  if (key == "cpu") {
    return this->readProcessors(entry, value);
  }
  if (key == "debug") {
    return this->readDebug(entry, value);
  }
  auto message = ErrorTextNative::unknown(key);

  return err(NativeManifestError{
      .code = NativeManifestErrorCode::UNKNOWN_FIELD,
      .message = message,
      .offset = keyAt,
  });
}

NativeManifest::ResultFieldRead NativeManifest::readFiles(std::vector<std::string> &files,
                                                          llvm::StringRef key,
                                                          const JsonValue &value) const {
  auto texts = this->readTexts(key, value);
  if (texts.isErr()) {
    return err(texts.error());
  }
  if (texts.value().empty()) {
    auto message = ErrorTextNative::empty(key);

    return err(NativeManifestError{
        .code = NativeManifestErrorCode::EMPTY_LIST,
        .message = message,
        .offset = value.begin,
    });
  }
  for (llvm::StringRef file : texts.value()) {
    bool isExcluded = file.consume_front("!");
    auto resolved = this->resolvePath(file);
    files.push_back(isExcluded ? "!" + resolved : resolved);
  }

  return ok();
}

NativeManifest::ResultFieldRead NativeManifest::readDefines(std::vector<std::string> &defines,
                                                            llvm::StringRef key,
                                                            const JsonValue &value) const {
  auto texts = this->readTexts(key, value);
  if (texts.isErr()) {
    return err(texts.error());
  }
  for (size_t index = 0; index < texts.value().size(); ++index) {
    auto define = this->checkDefine(texts.value()[index], value.items()[index].begin);
    if (define.isErr()) {
      return err(define.error());
    }
  }
  defines = texts.value();

  return ok();
}

NativeManifest::ResultFieldRead NativeManifest::readSystems(NativeEntry &entry,
                                                            const JsonValue &value) const {
  auto texts = this->readTexts("os", value);
  if (texts.isErr()) {
    return err(texts.error());
  }
  for (size_t index = 0; index < texts.value().size(); ++index) {
    llvm::StringRef system = texts.value()[index];
    if (system != "win32" && system != "linux") {
      auto message = ErrorTextNative::system(system);

      return err(NativeManifestError{
          .code = NativeManifestErrorCode::UNKNOWN_SYSTEM,
          .message = message,
          .offset = value.items()[index].begin,
      });
    }
  }
  entry.os = texts.value();

  return ok();
}

NativeManifest::ResultFieldRead NativeManifest::readProcessors(NativeEntry &entry,
                                                               const JsonValue &value) const {
  auto texts = this->readTexts("cpu", value);
  if (texts.isErr()) {
    return err(texts.error());
  }
  for (size_t index = 0; index < texts.value().size(); ++index) {
    llvm::StringRef processor = texts.value()[index];
    if (processor != "x64" && processor != "arm64") {
      auto message = ErrorTextNative::processor(processor);

      return err(NativeManifestError{
          .code = NativeManifestErrorCode::UNKNOWN_PROCESSOR,
          .message = message,
          .offset = value.items()[index].begin,
      });
    }
  }
  entry.cpu = texts.value();

  return ok();
}

NativeManifest::ResultFieldRead NativeManifest::readDebug(NativeEntry &entry,
                                                          const JsonValue &value) const {
  if (!value.isObject()) {
    auto message = ErrorTextNative::debug();

    return err(NativeManifestError{
        .code = NativeManifestErrorCode::DEBUG_NOT_AN_OBJECT,
        .message = message,
        .offset = value.begin,
    });
  }
  for (size_t index = 0; index < value.keys().size(); ++index) {
    llvm::StringRef key = value.keys()[index];
    if (key != "defines") {
      auto message = ErrorTextNative::debugKey(key);

      return err(NativeManifestError{
          .code = NativeManifestErrorCode::UNKNOWN_DEBUG_FIELD,
          .message = message,
          .offset = value.keyOffsets()[index],
      });
    }
  }
  const JsonValue *defines = value.get("defines");
  if (defines == nullptr) {
    return ok();
  }

  return this->readDefines(entry.debugDefines, "defines", *defines);
}

NativeManifest::ResultValueRead NativeManifest::readText(llvm::StringRef key,
                                                         const JsonValue &value) const {
  if (!value.isString()) {
    auto message = ErrorTextNative::notString(key);

    return err(NativeManifestError{
        .code = NativeManifestErrorCode::NOT_A_STRING,
        .message = message,
        .offset = value.begin,
    });
  }

  return ok(value.string());
}

NativeManifest::ResultTextsRead NativeManifest::readTexts(llvm::StringRef key,
                                                          const JsonValue &value) const {
  auto isString = std::bind_front(&NativeManifest::isStringItem, this);
  bool isStringList = value.isArray() && llvm::all_of(value.items(), isString);
  if (!isStringList) {
    auto message = ErrorTextNative::notStrings(key);

    return err(NativeManifestError{
        .code = NativeManifestErrorCode::NOT_A_STRING_LIST,
        .message = message,
        .offset = value.begin,
    });
  }
  auto texts = std::vector<std::string>();
  for (const JsonValue &item : value.items()) {
    texts.push_back(item.string());
  }

  return ok(texts);
}

NativeManifest::ResultFieldRead NativeManifest::checkRequired(const NativeEntry &entry,
                                                              const JsonValue &value) const {
  llvm::StringRef missing = entry.name.empty()          ? "name"
                            : entry.license.empty()     ? "license"
                            : entry.licenseFile.empty() ? "licenseFile"
                                                        : "";
  if (!missing.empty()) {
    auto message = ErrorTextNative::missing(missing);

    return err(NativeManifestError{
        .code = NativeManifestErrorCode::MISSING_FIELD,
        .message = message,
        .offset = value.begin,
    });
  }

  return ok();
}

NativeManifest::ResultFieldRead NativeManifest::checkKind(const NativeEntry &entry,
                                                          const JsonValue &value) const {
  bool hasSources = !entry.sources.empty();
  bool hasLibraries = !entry.libraries.empty();
  if (hasSources && hasLibraries) {
    auto message = ErrorTextNative::bothKinds();

    return err(NativeManifestError{
        .code = NativeManifestErrorCode::BOTH_KINDS,
        .message = message,
        .offset = value.begin,
    });
  }
  if (!hasSources && !hasLibraries) {
    auto message = ErrorTextNative::noKind();

    return err(NativeManifestError{
        .code = NativeManifestErrorCode::NO_KIND,
        .message = message,
        .offset = value.begin,
    });
  }

  return ok();
}

NativeManifest::ResultFieldRead NativeManifest::checkKindFields(const NativeEntry &entry,
                                                                const JsonValue &value) const {
  bool hasSources = !entry.sources.empty();
  for (size_t index = 0; index < value.keys().size(); ++index) {
    llvm::StringRef key = value.keys()[index];
    if (hasSources && this->isLibraryField(key)) {
      auto message = ErrorTextNative::libOnly(key);

      return err(NativeManifestError{
          .code = NativeManifestErrorCode::LIBRARIES_ONLY,
          .message = message,
          .offset = value.keyOffsets()[index],
      });
    }
    if (!hasSources && this->isSourceField(key)) {
      auto message = ErrorTextNative::sourceOnly(key);

      return err(NativeManifestError{
          .code = NativeManifestErrorCode::SOURCES_ONLY,
          .message = message,
          .offset = value.keyOffsets()[index],
      });
    }
  }

  return ok();
}

NativeManifest::ResultFieldRead NativeManifest::checkDefine(llvm::StringRef define,
                                                            size_t offset) const {
  if (define.starts_with("-D")) {
    auto message = ErrorTextNative::flag(define);

    return err(NativeManifestError{
        .code = NativeManifestErrorCode::DEFINE_WITH_FLAG,
        .message = message,
        .offset = offset,
    });
  }
  llvm::StringRef name = define.split('=').first;
  auto isNameCharacter = std::bind_front(&NativeManifest::isNameCharacter, this);
  bool isIdentifier =
      !name.empty() && !llvm::isDigit(name.front()) && llvm::all_of(name, isNameCharacter);
  if (!isIdentifier) {
    auto message = ErrorTextNative::notDefine(define);

    return err(NativeManifestError{
        .code = NativeManifestErrorCode::NOT_A_DEFINE,
        .message = message,
        .offset = offset,
    });
  }

  return ok();
}

std::string NativeManifest::resolvePath(llvm::StringRef file) const {
  bool isAbsolute = llvm::sys::path::is_absolute(file);
  llvm::SmallString<256> full(isAbsolute ? llvm::StringRef() : llvm::StringRef(this->packageDir));
  llvm::sys::path::append(full, file);
  llvm::sys::path::remove_dots(full, true);

  return std::string(full);
}

bool NativeManifest::isSourceField(llvm::StringRef key) const {
  return key == "defines" || key == "includes" || key == "debug";
}

bool NativeManifest::isLibraryField(llvm::StringRef key) const {
  return key == "dlls";
}

bool NativeManifest::isStringItem(const JsonValue &item) const {
  return item.isString();
}

bool NativeManifest::isNameCharacter(char character) const {
  return llvm::isAlnum(character) || character == '_';
}

}
