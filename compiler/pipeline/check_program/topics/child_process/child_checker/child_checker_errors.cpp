#include "compiler/pipeline/check_program/topics/child_process/child_checker/child_checker_errors.h"

namespace tz {

std::string ErrorTextChildRun::zeroFile(bool exec) {
  return "could not run it: the " + std::string(exec ? "command" : "program name") +
         " has a zero character in it";
}

std::string ErrorTextChildRun::emptyName() {
  return "could not run a program: its name is empty";
}

std::string ErrorTextChildRun::zeroArg(size_t index) {
  return "could not run it: args[" + std::to_string(index) + "] has a zero character in it";
}

std::string ErrorTextChildRun::optionsAt(llvm::StringRef fn) {
  return "the options of '" + fn.str() + "' come after its arguments, e.g. '" + fn.str() +
         "(file, [\"--version\"], { cwd })'";
}

std::string ErrorTextChildRun::zeroCwd() {
  return "could not run it: cwd has a zero character in it";
}

std::string ErrorTextChildRun::negative(llvm::StringRef limit) {
  return "could not run it: " + limit.str() + " must be 0 or more";
}

std::string ErrorTextChildRun::unixId(llvm::StringRef id) {
  return "'" + id.str() +
         "' works only on Linux and macOS: Windows has "
         "no user and group numbers";
}

std::string ErrorTextChildRun::noSignal(llvm::StringRef signal) {
  return "could not run it: there is no signal " + signal.str() + " (killSignal)";
}

std::string ErrorTextChildOption::encoding(llvm::StringRef fn) {
  return "the option 'encoding' of '" + fn.str() +
         "' is written as a literal, e.g. 'utf8': it decides whether "
         "the output is a string or a Buffer";
}

std::string ErrorTextChildOption::stdio(llvm::StringRef fn) {
  return "the option 'stdio' of '" + fn.str() +
         "' is written as a literal, e.g. \"inherit\" or [\"pipe\", "
         "\"inherit\", \"pipe\"]: whether there is output depends "
         "on it";
}

std::string ErrorTextChildOption::bufferText() {
  return "text input with the encoding 'buffer' is a TypeError in Node "
         "(Unknown encoding: buffer); pass its bytes, "
         "'Buffer.from(text)', or drop the encoding";
}

std::string ErrorTextChildOption::input(llvm::StringRef fn, llvm::StringRef type) {
  return "'input' of '" + fn.str() +
         "' is the text or the bytes the program reads (a string, a "
         "Buffer or a Uint8Array); found '" +
         type.str() + "'";
}

std::string ErrorTextChildOption::killSignal(llvm::StringRef type) {
  return "'killSignal' is the name of a signal, e.g. \"SIGKILL\", or "
         "its number; found '" +
         type.str() + "'";
}

std::string ErrorTextChildOption::execShell() {
  return "'execSync' always runs the command through a shell; "
         "'shell' may name another one, e.g. shell: \"powershell.exe\", "
         "and to run a program without one, use 'execFileSync'";
}

std::string ErrorTextChildOption::shellFlag(llvm::StringRef fn) {
  return "the option 'shell' of '" + fn.str() +
         "' is written as a literal, true or false: what can go wrong, "
         "and so the type of the error, depends on it";
}

std::string ErrorTextChildOption::shell(llvm::StringRef type) {
  return "'shell' is true, false or the path of a shell; found '" + type.str() + "'";
}

std::string ErrorTextChildStdio::noFd(uint64_t fd) {
  return "there is no file descriptor " + std::to_string(fd);
}

std::string ErrorTextChildStdio::literal() {
  return "each stream of 'stdio' is written as a literal: \"pipe\", "
         "\"inherit\", \"ignore\" or a file descriptor such as 2";
}

std::string ErrorTextChildStdio::ipc() {
  return "'ipc' is the channel to another Node that fork starts, which tz "
         "does not have (docs/process.md, section 4)";
}

std::string ErrorTextChildStdio::unknown(llvm::StringRef text) {
  return "unknown stdio '" + text.str() +
         "'; a stream is \"pipe\", \"inherit\", \"ignore\" or a file "
         "descriptor such as 2";
}

std::string ErrorTextChildStdio::three() {
  return "only stdin, stdout and stderr are supported in 'stdio'; more streams come "
         "with the async functions (docs/process.md, step 2)";
}

std::string ErrorTextChildStdio::spread() {
  return "write each stream of 'stdio' in the literal";
}

}
