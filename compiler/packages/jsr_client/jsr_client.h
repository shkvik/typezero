#pragma once

#include "compiler/packages/jsr_version/jsr_version.h"
#include "compiler/packages/jsr_versions/jsr_versions.h"
#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz::pkg {

struct JsonValue;

class JsrClient {
public:
  static std::string jsrBaseUrl();
  static bool jsrVersion(llvm::StringRef scope, llvm::StringRef name, llvm::StringRef version,
                         JsrVersion &out, std::string &error);
  static bool jsrDownload(llvm::StringRef scope, llvm::StringRef name, llvm::StringRef version,
                          const JsrVersion &meta, std::string &dir, std::string &error);
  static bool jsrVersions(llvm::StringRef scope, llvm::StringRef name, JsrVersions &out,
                          std::string &error);

private:
  static bool writeFile(llvm::StringRef file, llvm::StringRef data, std::string &error);
  static bool safeRelativePath(llvm::StringRef file);
  static bool fetchJson(const std::string &url, std::string &text, JsonValue &out,
                        std::string &error);
  static std::string jsrPackageUrl(llvm::StringRef scope, llvm::StringRef name);
  static bool readJsrVersion(llvm::StringRef text, const std::string &where, JsrVersion &out,
                             std::string &error);
};

}
