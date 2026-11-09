#include "compiler/program/module/load_chain/load_chain_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextLoadChain::pending(llvm::StringRef name, bool started) {
  return (llvm::Twine(" -> (") + name + (started ? " already started)" : " not loaded yet)")).str();
}

}
