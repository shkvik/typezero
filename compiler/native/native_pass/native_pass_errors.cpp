#include "compiler/native/native_pass/native_pass_errors.h"

namespace tz {

std::string ErrorTextNativePass::twoFit(llvm::StringRef name, llvm::StringRef platform) {
  return "native '" + name.str() + "': two entries fit " + platform.str() +
         "; set 'os' and 'cpu' so that one does";
}

std::string ErrorTextNativePass::missing(llvm::StringRef name, llvm::StringRef what,
                                         llvm::StringRef path) {
  return "native '" + name.str() + "': " + what.str() + " '" + path.str() + "' is missing";
}

std::string ErrorTextNativePass::unreadable(llvm::StringRef name, llvm::StringRef path,
                                            llvm::StringRef why) {
  return "native '" + name.str() + "': cannot read the library '" + path.str() + "': " + why.str();
}

std::string ErrorTextNativePass::noFiles(llvm::StringRef name, llvm::StringRef pattern) {
  return "native '" + name.str() + "': '" + pattern.str() + "' names no file";
}

std::string ErrorTextNativePass::compile(llvm::StringRef name, llvm::StringRef output) {
  return "native '" + name.str() + "': the C code does not compile\n" + output.str();
}

std::string ErrorTextNativePass::join(llvm::StringRef name) {
  return "native '" + name.str() + "': cannot join the C code with the program";
}

}
