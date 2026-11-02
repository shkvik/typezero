#include "compiler/cli/script_command/script_command.h"
#include "compiler/cli/args/args.h"
#include "compiler/cli/exit_codes/exit_codes.h"
#include "compiler/packages/manifest/manifest.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include <cstdlib>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace tz {

namespace {

std::string shellWord(llvm::StringRef arg) {
#ifdef _WIN32
  if (!arg.empty() && arg.find_first_of(" \t\"&|<>^%()") == llvm::StringRef::npos) {
    return arg.str();
  }
  std::string out = "\"";
  for (char c : arg) {
    out += c == '"' ? std::string("\"\"") : std::string(1, c);
  }

  return out + "\"";
#else
  if (!arg.empty() && arg.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                            "0123456789-_./=:,+@%") == llvm::StringRef::npos) {
    return arg.str();
  }
  std::string out = "'";
  for (char c : arg) {
    out += c == '\'' ? std::string("'\\''") : std::string(1, c);
  }

  return out + "'";
#endif
}

void addToPath(llvm::StringRef dir) {
  const char *old = std::getenv("PATH");
#ifdef _WIN32
  std::string value = dir.str() + ";" + (old ? old : "");
  _putenv_s("PATH", value.c_str());
#else
  std::string value = dir.str() + ":" + (old ? old : "");
  setenv("PATH", value.c_str(), 1);
#endif
}

int spawn(const std::string &command) {
#ifdef _WIN32
  std::string line = "cmd.exe /d /s /c \"" + command + "\"";
  llvm::SmallVector<llvm::UTF16, 256> wide;
  if (!llvm::convertUTF8ToUTF16String(line, wide)) {
    llvm::errs() << "tz: the script is not UTF-8\n";
    return -1;
  }
  wide.push_back(0);
  STARTUPINFOW startup{};
  startup.cb = sizeof startup;
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(nullptr, reinterpret_cast<wchar_t *>(wide.data()), nullptr, nullptr, TRUE, 0,
                      nullptr, nullptr, &startup, &process)) {
    llvm::errs() << "tz: cannot start cmd.exe for the script (error " << GetLastError() << ")\n";
    return -1;
  }
  WaitForSingleObject(process.hProcess, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(process.hProcess, &code);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);

  return static_cast<int>(code);
#else
  pid_t child = fork();
  if (child < 0) {
    llvm::errs() << "tz: cannot start /bin/sh for the script\n";
    return -1;
  }
  if (child == 0) {
    execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char *>(nullptr));
    _exit(127);
  }
  int status = 0;
  while (waitpid(child, &status, 0) < 0) {
  }
  if (WIFSIGNALED(status)) {
    return 128 + WTERMSIG(status);
  }

  return WEXITSTATUS(status);
#endif
}

}

ScriptCommand::ScriptCommand(const char *argv0) : argv0_(argv0) {}

int ScriptCommand::run(const Args &args) const {
  if (args.programArgs.empty() || args.literalArgs) {
    return -1;
  }
  std::string manifestPath = pkg::findManifest("");
  if (manifestPath.empty()) {
    return -1;
  }
  pkg::Manifest manifest;
  if (!pkg::readManifest(manifestPath, manifest)) {
    return kExitError;
  }
  const std::string &name = args.programArgs.front();
  const std::string *script = nullptr;
  for (const auto &[key, command] : manifest.scripts) {
    if (key == name) {
      script = &command;
    }
  }
  if (!script) {
    return -1;
  }
  std::string command = *script;
  for (size_t i = 1; i < args.programArgs.size(); ++i) {
    command += " " + shellWord(args.programArgs[i]);
  }
  llvm::errs() << "$ " << command << "\n";
  llvm::errs().flush();
  std::string self = llvm::sys::fs::getMainExecutable(argv0_, (void *)(intptr_t)&Args::printUsage);
  if (!self.empty()) {
    addToPath(llvm::sys::path::parent_path(self));
  }
  if (std::error_code ec = llvm::sys::fs::set_current_path(manifest.dir)) {
    llvm::errs() << "tz: cannot go to " << manifest.dir << ": " << ec.message() << "\n";
    return kExitError;
  }
  int code = spawn(command);

  return code < 0 ? kExitError : code;
}

}
