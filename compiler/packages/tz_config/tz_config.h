#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz::pkg {

struct JsonValue;

struct TzConfig {
  std::string path;
  std::string dir;
  std::string outDir;
};

class TzConfigFile {
public:
  static std::string findTzConfig(llvm::StringRef dir);
  static bool readTzConfig(llvm::StringRef file, TzConfig &out);

private:
  static bool readOptions(TzConfig &out, llvm::StringRef text, const JsonValue &options);
};

}
