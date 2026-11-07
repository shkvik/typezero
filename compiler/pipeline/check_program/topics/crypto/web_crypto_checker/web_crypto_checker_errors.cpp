#include "compiler/pipeline/check_program/topics/crypto/web_crypto_checker/web_crypto_checker_errors.h"

namespace tz {

std::string ErrorTextSubtle::literal(llvm::StringRef name) {
  return "the hash or the cipher of '" + name.str() + "' is a literal of the direct layer";
}

std::string ErrorTextSubtle::noSubtle() {
  return "'crypto.subtle' is not available here";
}

}
