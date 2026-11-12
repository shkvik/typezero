#include "compiler/semantic/library/fs/fs_checker/fs_checker_errors.h"

namespace tz {

std::string ErrorTextFsOptions::literal(llvm::StringRef fn, llvm::StringRef first) {
  return "the options of '" + fn.str() +
         "' are written in the call, as an object literal, e.g. '{ " + first.str() +
         ": ... }': the type of what it gives may depend on them";
}

std::string ErrorTextFsOptions::byName(llvm::StringRef fn, llvm::StringRef first) {
  return "write each option of '" + fn.str() + "' by its name, e.g. '{ " + first.str() + ": ... }'";
}

std::string ErrorTextFsOptions::unknown(llvm::StringRef fn, llvm::StringRef name,
                                        llvm::ArrayRef<llvm::StringRef> allowed) {
  auto list = std::string();
  for (size_t i = 0; i < allowed.size(); ++i) {
    list += (i == 0 ? "" : i + 1 == allowed.size() ? " and " : ", ") + allowed[i].str();
  }

  return "'" + fn.str() + "' has no option '" + name.str() + "'; it takes " + list;
}

std::string ErrorTextFsOptions::flag(llvm::StringRef fn, llvm::StringRef option) {
  return "the option '" + option.str() + "' of '" + fn.str() +
         "' is written as a literal, true or false: the type of what "
         "it gives depends on it";
}

std::string ErrorTextFsEncoding::literal(llvm::StringRef fn) {
  return "the encoding of '" + fn.str() + "' is written as a literal, 'utf8'";
}

std::string ErrorTextFsEncoding::utf8Only(llvm::StringRef fn, llvm::StringRef encoding) {
  return "'" + fn.str() + "' gives names in utf8 only; drop the encoding '" + encoding.str() + "'";
}

std::string ErrorTextFsEncoding::dependsOn(llvm::StringRef fn) {
  return "the encoding of '" + fn.str() +
         "' is written as a literal, 'utf8': the type of what it gives "
         "depends on it";
}

std::string ErrorTextFsEncoding::forText(llvm::StringRef fn) {
  return "an encoding is for text; '" + fn.str() +
         "' writes the bytes as they are: drop the encoding";
}

std::string ErrorTextFsCall::notFunc(llvm::StringRef fn) {
  return "'" + fn.str() + "' of 'node:fs' is not a function";
}

std::string ErrorTextFsCall::zeroPath() {
  return "the path has a zero character in it, which no file "
         "name can have";
}

std::string ErrorTextFsCall::flag(llvm::StringRef fn, llvm::StringRef written, bool literal) {
  std::string known = "'r', 'r+', 'w', 'wx', 'w+', 'wx+', 'a', 'ax', 'a+', 'ax+', and with 's' "
                      "for synchronous writes 'rs', 'rs+', 'as', 'as+'";

  return literal
             ? "'" + written.str() + "' is not a flag of '" + fn.str() + "'; the flags are " + known
             : "the flag of '" + fn.str() + "' is written as a literal: " + known;
}

std::string ErrorTextFsCall::data(llvm::StringRef fn, const Type *type) {
  std::string hint = type->isNumeric() || type->isBool() ? "; convert it first, e.g. 'String(x)'"
                     : type->isBytes() ? "; write a view of it, e.g. 'new Uint8Array(buffer)'"
                                       : "";

  return "'" + fn.str() +
         "' writes a string or bytes (a Uint8Array or a "
         "Buffer); found '" +
         type->name().str() + "'" + hint;
}

std::string ErrorTextFsCall::chmod() {
  return "the mode of 'chmodSync' is a number, e.g. 0o644, or "
         "octal digits in a string, e.g. \"644\"";
}

std::string ErrorTextFsCall::bigint() {
  return "'bigint' is not supported: tz has no bigint; "
         "the numbers of Stats are doubles";
}

std::string ErrorTextFsCall::rmdir() {
  return "the options of 'rmdirSync' are deprecated in Node; to "
         "delete a directory with what is in it, use 'rmSync(path, "
         "{ recursive: true })'";
}

std::string ErrorTextFsCall::copyMode(llvm::StringRef fn) {
  return "'" + fn.str() +
         "' never replaces a file that is already there, so it takes no "
         "mode: drop it; to replace a file on purpose, copy it to a "
         "temporary name and move that over the file with 'replaceSync' "
         "from \"tz:fs\"";
}

std::string ErrorTextFsConstant::value() {
  return "'constants' of 'node:fs' is used as 'constants.F_OK' and the like";
}

std::string ErrorTextFsConstant::notYet(llvm::StringRef name) {
  return "'constants." + name.str() +
         "' of 'node:fs' is not supported yet; tz has F_OK, "
         "R_OK, W_OK, X_OK and the COPYFILE_ ones";
}

}
