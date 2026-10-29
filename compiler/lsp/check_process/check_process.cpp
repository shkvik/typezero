#include "compiler/lsp/check_process/check_process.h"
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <signal.h>
#endif

namespace tz {

bool CheckProcess::start(llvm::StringRef program, llvm::ArrayRef<llvm::StringRef> args,
                         llvm::StringRef log, std::string &error) {
  std::vector<std::optional<llvm::StringRef>> redirects = {llvm::StringRef(""), log, log};
  bool failed = false;
  info_ = llvm::sys::ExecuteNoWait(program, args, std::nullopt, redirects, 0, &error, &failed);
  running_ = !failed && info_.Pid != 0;

  return running_;
}

std::optional<int> CheckProcess::poll() {
  if (!running_) {
    return std::nullopt;
  }
  std::string error;
  llvm::sys::ProcessInfo done = llvm::sys::Wait(info_, 0, &error, nullptr, true);
  if (done.Pid == 0) {
    return std::nullopt;
  }
  running_ = false;

  return done.ReturnCode;
}

void CheckProcess::kill() {
  if (!running_) {
    return;
  }
#ifdef _WIN32
  TerminateProcess(info_.Process, 1);
#else
  ::kill(info_.Pid, SIGKILL);
#endif
  std::string error;
  llvm::sys::Wait(info_, std::nullopt, &error);
  running_ = false;
}

}
