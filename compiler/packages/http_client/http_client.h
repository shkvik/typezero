#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz::pkg {

enum class FetchResult { Ok, NotFound, Failed };

class HttpClient {
public:
  static std::string fileUrlPath(llvm::StringRef url);
  static FetchResult fetchUrl(const std::string &url, std::string &body, std::string &error);
};

}
