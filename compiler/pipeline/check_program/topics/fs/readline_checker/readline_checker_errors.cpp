#include "compiler/pipeline/check_program/topics/fs/readline_checker/readline_checker_errors.h"

namespace tz {

std::string ErrorTextReadline::typeOnly(llvm::StringRef fn, bool promises) {
  const char *name = promises ? "node:readline/promises" : "node:readline";

  return "'" + fn.str() + "' of '" + name + "' can only be used as a type; call 'createInterface'";
}

std::string ErrorTextReadline::option(llvm::StringRef name) {
  return "the option '" + name.str() + "' of 'createInterface' is not supported yet";
}

std::string ErrorTextReadline::noInput() {
  return "'createInterface' needs the input: '{ input: "
         "process.stdin }'";
}

std::string ErrorTextReadline::notStdin() {
  return "readline reads only stdin yet: the input is 'process.stdin'; for "
         "the lines of a file, 'readFile' it and split them";
}

std::string ErrorTextReadline::output() {
  return "the output of readline is 'process.stdout' or "
         "'process.stderr'";
}

}
