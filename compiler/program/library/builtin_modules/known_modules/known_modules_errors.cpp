#include "compiler/program/library/builtin_modules/known_modules/known_modules_errors.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

std::string ErrorTextBuiltinModule::none(llvm::StringRef path) {
  std::string quoted = "'" + path.str() + "'";
  if (path.starts_with("tz:")) {
    return "there is no built-in module " + quoted +
           "; tz has 'tz:io', 'tz:fs', 'tz:crypto', 'tz:http', 'tz:http2' and 'tz:sync'";
  }

  return "there is no built-in module " + quoted;
}

std::string ErrorTextBuiltinModule::notYet(llvm::StringRef path) {
  llvm::StringRef bare = path;
  bare.consume_front("node:");
  std::string message = "built-in module '" + path.str() + "' is not supported yet";
  if (bare == "timers") {
    return message + "; setTimeout and the other timers are global, as in Node: drop the import";
  }
  if (bare == "http" || bare == "https") {
    return message + " (docs/net.md, section 7); serve requests with 'Server' from \"tz:http\", "
                     "and make them with the global 'fetch'";
  }
  if (bare == "http2") {
    return message + " (docs/net.md, section 7); HTTP/2 streams are in \"tz:http2\": "
                     "'Http2Server' and 'Http2Client'";
  }
  static const llvm::StringLiteral network[] = {"tls", "dns", "dns/promises"};
  if (llvm::is_contained(network, bare)) {
    return message + " (docs/net.md, section 7)";
  }
  if (bare == "events") {
    return message + " (docs/events.md)";
  }
  if (bare.starts_with("stream")) {
    return message + " (docs/streams.md)";
  }

  return message;
}

}
