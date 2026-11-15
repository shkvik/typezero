#include "compiler/semantic/library_v2/common/library_common_registry_errors.h"

namespace tz {

std::string ErrorTextNodeOptions::literal(llvm::StringRef fn, llvm::StringRef first) {
  return "the options of '" + fn.str() +
         "' are written in the call, as an object literal, e.g. '{ " + first.str() +
         ": ... }': the type of what it gives may depend on them";
}

std::string ErrorTextNodeOptions::byName(llvm::StringRef fn, llvm::StringRef first) {
  return "write each option of '" + fn.str() + "' by its name, e.g. '{ " + first.str() + ": ... }'";
}

std::string ErrorTextNodeOptions::unknown(llvm::StringRef fn, llvm::StringRef name,
                                          llvm::ArrayRef<llvm::StringRef> allowed) {
  auto list = std::string();
  for (size_t i = 0; i < allowed.size(); ++i) {
    list += (i == 0 ? "" : i + 1 == allowed.size() ? " and " : ", ") + allowed[i].str();
  }

  return "'" + fn.str() + "' has no option '" + name.str() + "'; it takes " + list;
}

std::string ErrorTextNodeOptions::flag(llvm::StringRef fn, llvm::StringRef option) {
  return "the option '" + option.str() + "' of '" + fn.str() +
         "' is written as a literal, true or false: the type of what "
         "it gives depends on it";
}

std::string ErrorTextBufferEncoding::unknown(llvm::StringRef name) {
  return "unknown encoding '" + name.str() +
         "' (node throws a TypeError); Buffer knows utf8, utf16le, "
         "latin1, ascii, base64, base64url and hex";
}

std::string ErrorTextBufferEncoding::notKnown() {
  return "an encoding known only when the program runs may be one Buffer does "
         "not know, which node would throw a TypeError for; check it first, "
         "e.g. 'if (enc == \"hex\" || enc == \"base64\")', or give it the type "
         "'BufferEncoding'";
}

std::string ErrorTextIntrinsicCall::wrong(llvm::StringRef name) {
  return "'" + name.str() + "' is no intrinsic of tz, or is called wrongly";
}

}
