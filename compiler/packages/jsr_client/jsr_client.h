#pragma once

#include "compiler/packages/jsr_version/jsr_version.h"
#include "compiler/packages/jsr_versions/jsr_versions.h"
#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz::pkg {

std::string jsrBaseUrl();

bool jsrVersions(llvm::StringRef scope, llvm::StringRef name, JsrVersions &out, std::string &error);
bool jsrVersion(llvm::StringRef scope, llvm::StringRef name, llvm::StringRef version,
                JsrVersion &out, std::string &error);
bool jsrDownload(llvm::StringRef scope, llvm::StringRef name, llvm::StringRef version,
                 const JsrVersion &meta, std::string &dir, std::string &error);

}
