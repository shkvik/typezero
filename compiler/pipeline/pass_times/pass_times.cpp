#include "compiler/pipeline/pass_times/pass_times.h"

namespace tz {

void PassTimes::start() { started_ = std::chrono::steady_clock::now(); }

void PassTimes::stop(llvm::StringRef pass) {
  auto now = std::chrono::steady_clock::now();
  double ms = std::chrono::duration<double, std::milli>(now - started_).count();
  started_ = now;
  for (auto &[name, total] : passes_) {
    if (name == pass) {
      total += ms;
      return;
    }
  }
  passes_.push_back({pass.str(), ms});
}

std::string PassTimes::text() const {
  std::string out;
  for (const auto &[name, total] : passes_) {
    out +=
        (out.empty() ? "" : ", ") + name + " " + std::to_string(static_cast<long>(total)) + " ms";
  }

  return out + "; " + std::to_string(rounds_) + (rounds_ == 1 ? " round" : " rounds");
}

}
