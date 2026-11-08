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
  this->info = llvm::sys::ExecuteNoWait(program, args, std::nullopt, redirects, 0, &error, &failed);
  this->running = !failed && this->info.Pid != 0;

  return this->running;
}

std::optional<int> CheckProcess::poll() {
  if (!this->running) {
    return std::nullopt;
  }
  std::string error;
  llvm::sys::ProcessInfo done = llvm::sys::Wait(this->info, 0, &error, nullptr, true);
  if (done.Pid == 0) {
    return std::nullopt;
  }
  this->running = false;

  return done.ReturnCode;
}

void CheckProcess::kill() {
  if (!this->running) {
    return;
  }
#ifdef _WIN32
  TerminateProcess(this->info.Process, 1);
#else
  ::kill(this->info.Pid, SIGKILL);
#endif
  std::string error;
  llvm::sys::Wait(this->info, std::nullopt, &error);
  this->running = false;
}

}
