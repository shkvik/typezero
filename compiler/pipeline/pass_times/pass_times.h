#pragma once

#include "llvm/ADT/StringRef.h"
#include <chrono>
#include <string>
#include <vector>

namespace tz {

class PassTimes {
public:
  void start();
  void stop(llvm::StringRef pass);

  const std::vector<std::pair<std::string, double>> &passes() const { return passes_; }
  std::string text() const;

private:
  std::chrono::steady_clock::time_point started;
  std::vector<std::pair<std::string, double>> passes_;
};

}
