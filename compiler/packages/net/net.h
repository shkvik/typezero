#pragma once

#include "llvm/ADT/StringRef.h"

#include <string>

namespace tz::pkg {

enum class FetchResult { Ok, NotFound, Failed };

// Reads `url` into `body`: https:// and http:// through the system's HTTP
// client (WinHTTP on Windows, curl elsewhere), file:// from the disk, which
// is how a mirror of a registry on the machine is used. NotFound is HTTP 404
// or a missing file; Failed sets `error`.
FetchResult fetchUrl(const std::string &url, std::string &body, std::string &error);

// The path a file:// URL names ("file:///C:/x" is "C:/x"), or "" for another
// URL.
std::string fileUrlPath(llvm::StringRef url);

} // namespace tz::pkg
