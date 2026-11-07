#pragma once

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz::pkg {

class GitClient {
public:
  static bool gitCheckout(const std::string &url, const std::string &commit, std::string &dir,
                          std::string &error);
  static bool gitResolve(const std::string &url, const std::string &ref, bool refresh,
                         std::string &commit, std::string &error);

private:
  static bool git(llvm::ArrayRef<std::string> args, std::string *output, std::string &error);
  static std::string repositoryKey(llvm::StringRef url);
};

}
