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
  void round() { ++rounds_; }

  unsigned rounds() const { return rounds_; }
  const std::vector<std::pair<std::string, double>> &passes() const { return passes_; }
  std::string text() const;

private:
  std::chrono::steady_clock::time_point started_;
  std::vector<std::pair<std::string, double>> passes_;
  unsigned rounds_ = 0;
};

}
