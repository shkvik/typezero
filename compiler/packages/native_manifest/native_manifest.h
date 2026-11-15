#pragma once

#include "compiler/packages/json_value/json_value.h"
#include "compiler/packages/native_entry/native_entry.h"
#include "compiler/packages/native_manifest/native_manifest_errors.h"
#include "compiler/result/result/result.h"
#include "llvm/ADT/StringRef.h"
#include <string>
#include <vector>

namespace tz::pkg {

class NativeManifest {
public:
  using ResultEntriesRead = Result<std::vector<NativeEntry>, NativeManifestError>;

  NativeManifest(llvm::StringRef dir, llvm::StringRef path);

  ResultEntriesRead readEntries(const JsonValue &value) const;

private:
  using ResultEntryRead = Result<NativeEntry, NativeManifestError>;
  using ResultFieldRead = Result<void, NativeManifestError>;
  using ResultValueRead = Result<std::string, NativeManifestError>;
  using ResultTextsRead = Result<std::vector<std::string>, NativeManifestError>;

  ResultEntryRead readEntry(const JsonValue &value) const;
  ResultFieldRead readField(NativeEntry &entry, llvm::StringRef key, size_t keyAt,
                            const JsonValue &value) const;
  ResultFieldRead readFiles(std::vector<std::string> &files, llvm::StringRef key,
                            const JsonValue &value) const;
  ResultFieldRead readDefines(std::vector<std::string> &defines, llvm::StringRef key,
                              const JsonValue &value) const;
  ResultFieldRead readSystems(NativeEntry &entry, const JsonValue &value) const;
  ResultFieldRead readProcessors(NativeEntry &entry, const JsonValue &value) const;
  ResultFieldRead readDebug(NativeEntry &entry, const JsonValue &value) const;
  ResultValueRead readText(llvm::StringRef key, const JsonValue &value) const;
  ResultTextsRead readTexts(llvm::StringRef key, const JsonValue &value) const;
  ResultFieldRead checkRequired(const NativeEntry &entry, const JsonValue &value) const;
  ResultFieldRead checkKind(const NativeEntry &entry, const JsonValue &value) const;
  ResultFieldRead checkKindFields(const NativeEntry &entry, const JsonValue &value) const;
  ResultFieldRead checkDefine(llvm::StringRef define, size_t offset) const;

  std::string resolvePath(llvm::StringRef file) const;
  bool isSourceField(llvm::StringRef key) const;
  bool isLibraryField(llvm::StringRef key) const;
  bool isStringItem(const JsonValue &item) const;
  bool isNameCharacter(char character) const;

  std::string packageDir;
  std::string manifestPath;
};

}
