#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class HttpMethod {
  GET,
  HEAD,
  CONNECT,
  TRACE,
  TRACK,
};

struct HttpMethodInfo {
  llvm::StringLiteral name;
  HttpMethod method;
  bool isBodiless;
  bool isForbidden;
};

class HttpMethodTable {
public:
  static const HttpMethodInfo *find(llvm::StringRef name);
  static llvm::StringRef getName(HttpMethod method);
};

}
