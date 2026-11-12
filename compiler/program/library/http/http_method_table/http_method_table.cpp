#include "compiler/program/library/http/http_method_table/http_method_table.h"
#include "llvm/Support/ErrorHandling.h"

namespace tz {

namespace {

const HttpMethodInfo kMethods[] = {
    {.name = "GET", .method = HttpMethod::GET, .isBodiless = true, .isForbidden = false},
    {.name = "HEAD", .method = HttpMethod::HEAD, .isBodiless = true, .isForbidden = false},
    {.name = "CONNECT", .method = HttpMethod::CONNECT, .isBodiless = false, .isForbidden = true},
    {.name = "TRACE", .method = HttpMethod::TRACE, .isBodiless = false, .isForbidden = true},
    {.name = "TRACK", .method = HttpMethod::TRACK, .isBodiless = false, .isForbidden = true},
};

}

const HttpMethodInfo *HttpMethodTable::find(llvm::StringRef name) {
  for (const HttpMethodInfo &info : kMethods) {
    if (info.name == name) {
      return &info;
    }
  }

  return nullptr;
}

llvm::StringRef HttpMethodTable::getName(HttpMethod method) {
  for (const HttpMethodInfo &info : kMethods) {
    if (info.method == method) {
      return info.name;
    }
  }
  llvm_unreachable("http method missing from the table");
}

}
