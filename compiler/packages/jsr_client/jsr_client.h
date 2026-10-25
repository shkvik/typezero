#pragma once

#include "compiler/packages/jsr_version/jsr_version.h"
#include "compiler/packages/jsr_versions/jsr_versions.h"
#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz::pkg {

// JSR (https://jsr.io/docs/api).

// The registry: TZ_JSR_URL, or https://jsr.io. A file:// address is a
// mirror on the disk with the same layout as the registry's API.
std::string jsrBaseUrl();

// meta.json of @scope/name. False with an empty `error` if the registry has
// no such package.
bool jsrVersions(llvm::StringRef scope, llvm::StringRef name, JsrVersions &out, std::string &error);
// <version>_meta.json, from the cache or the registry (and then kept).
bool jsrVersion(llvm::StringRef scope, llvm::StringRef name, llvm::StringRef version,
                JsrVersion &out, std::string &error);
// The files of the version in the cache, downloaded and checked against their
// hashes if they are not there yet; `dir` is where they are.
bool jsrDownload(llvm::StringRef scope, llvm::StringRef name, llvm::StringRef version,
                 const JsrVersion &meta, std::string &dir, std::string &error);

} // namespace tz::pkg
