#include "compiler/program/library/child_process/child_signal_table/child_signal_table.h"

namespace tz {

namespace {

const ChildSignalInfo kSignals[] = {
    {.name = "SIGHUP", .isOnWindows = true, .isOnPosix = true},
    {.name = "SIGINT", .isOnWindows = true, .isOnPosix = true},
    {.name = "SIGQUIT", .isOnWindows = true, .isOnPosix = true},
    {.name = "SIGILL", .isOnWindows = true, .isOnPosix = true},
    {.name = "SIGTRAP", .isOnWindows = false, .isOnPosix = true},
    {.name = "SIGABRT", .isOnWindows = true, .isOnPosix = true},
    {.name = "SIGIOT", .isOnWindows = false, .isOnPosix = true},
    {.name = "SIGBUS", .isOnWindows = false, .isOnPosix = true},
    {.name = "SIGFPE", .isOnWindows = true, .isOnPosix = true},
    {.name = "SIGKILL", .isOnWindows = true, .isOnPosix = true},
    {.name = "SIGUSR1", .isOnWindows = false, .isOnPosix = true},
    {.name = "SIGSEGV", .isOnWindows = true, .isOnPosix = true},
    {.name = "SIGUSR2", .isOnWindows = false, .isOnPosix = true},
    {.name = "SIGPIPE", .isOnWindows = false, .isOnPosix = true},
    {.name = "SIGALRM", .isOnWindows = false, .isOnPosix = true},
    {.name = "SIGTERM", .isOnWindows = true, .isOnPosix = true},
    {.name = "SIGCHLD", .isOnWindows = false, .isOnPosix = true},
    {.name = "SIGSTKFLT", .isOnWindows = false, .isOnPosix = true},
    {.name = "SIGCONT", .isOnWindows = false, .isOnPosix = true},
    {.name = "SIGSTOP", .isOnWindows = false, .isOnPosix = true},
    {.name = "SIGTSTP", .isOnWindows = false, .isOnPosix = true},
    {.name = "SIGTTIN", .isOnWindows = false, .isOnPosix = true},
    {.name = "SIGTTOU", .isOnWindows = false, .isOnPosix = true},
    {.name = "SIGURG", .isOnWindows = false, .isOnPosix = true},
    {.name = "SIGXCPU", .isOnWindows = false, .isOnPosix = true},
    {.name = "SIGXFSZ", .isOnWindows = false, .isOnPosix = true},
    {.name = "SIGVTALRM", .isOnWindows = false, .isOnPosix = true},
    {.name = "SIGPROF", .isOnWindows = false, .isOnPosix = true},
    {.name = "SIGWINCH", .isOnWindows = true, .isOnPosix = true},
    {.name = "SIGIO", .isOnWindows = false, .isOnPosix = true},
    {.name = "SIGPOLL", .isOnWindows = false, .isOnPosix = true},
    {.name = "SIGPWR", .isOnWindows = false, .isOnPosix = true},
    {.name = "SIGSYS", .isOnWindows = false, .isOnPosix = true},
    {.name = "SIGBREAK", .isOnWindows = true, .isOnPosix = false},
};

}

bool ChildSignalTable::isKnown(llvm::StringRef name, bool isWindows) {
  for (const ChildSignalInfo &info : kSignals) {
    if (info.name == name) {
      return isWindows ? info.isOnWindows : info.isOnPosix;
    }
  }

  return false;
}

}
