#include "compiler/pipeline/check_program/topics/process/process_checker/process_checker_errors.h"

namespace tz {

std::string ErrorTextProcessEvent::noThrown() {
  return "there is nothing to catch: nothing is thrown or rejected in tz, an error is a Result "
         "that the code which gets it handles (docs/errors.md, section 4)";
}

std::string ErrorTextProcessCall::onlyNow(llvm::StringRef object, llvm::StringRef name) {
  return "'" + object.str() + "." + name.str() + "' is not supported yet; only '" + object.str() +
         ".now()' is";
}

std::string ErrorTextProcessCall::ttyValue(llvm::StringRef stream) {
  return "'" + stream.str() + ".isTTY' is a boolean, not a function";
}

std::string ErrorTextProcessCall::streamCall(llvm::StringRef stream, llvm::StringRef name) {
  return "'" + stream.str() + "." + name.str() +
         "' is not supported yet; only 'write(text)' and 'isTTY' are";
}

std::string ErrorTextProcessCall::notFunc(llvm::StringRef name) {
  return "'process." + name.str() + "' is not a function";
}

std::string ErrorTextProcessCall::notYet(llvm::StringRef name) {
  return "'process." + name.str() + "' is not supported yet";
}

std::string ErrorTextProcessCall::signal(llvm::StringRef name, llvm::StringRef event) {
  return "'process." + name.str() +
         "' listens only to the signals 'SIGINT', 'SIGTERM', "
         "'SIGHUP' and 'SIGBREAK', not to '" +
         event.str() + "'";
}

std::string ErrorTextProcessStream::name(bool out) {
  return out ? "process.stdout" : "process.stderr";
}

std::string ErrorTextProcessMember::writeCall(llvm::StringRef stream) {
  return "'" + stream.str() + ".write' can only be called: '" + stream.str() + ".write(text)'";
}

std::string ErrorTextProcessMember::streamPart(llvm::StringRef stream, llvm::StringRef name) {
  return "'" + stream.str() + "." + name.str() + "' is not supported yet; only " +
         "'isTTY', 'write()' and the rest of a Writable of " + "node:stream are";
}

std::string ErrorTextProcessMember::perfNow() {
  return "'performance.now' can only be called: 'performance.now()'";
}

std::string ErrorTextProcessMember::perfOther(llvm::StringRef name) {
  return "'performance." + name.str() + "' is not supported yet; only 'performance.now()' is";
}

std::string ErrorTextProcessMember::onlyCalled(llvm::StringRef name) {
  return "'process." + name.str() + "' can only be called: 'process." + name.str() +
         (name == "exit" ? "(code)'" : "()'");
}

std::string ErrorTextProcessMember::streamUse(llvm::StringRef name) {
  return "'process." + name.str() + "' can only be used as 'process." + name.str() +
         ".write(text)' or 'process." + name.str() + ".isTTY'";
}

std::string ErrorTextProcessAssign::exitCode() {
  return "only '=' can set 'process.exitCode'";
}

std::string ErrorTextProcessAssign::assigned(llvm::StringRef name) {
  return "'process." + name.str() + "' cannot be assigned to";
}

std::string ErrorTextProcessAssign::changed(llvm::StringRef name) {
  return "'process." + name.str() + "' cannot be changed";
}

}
