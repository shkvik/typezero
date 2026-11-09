#include "compiler/program/module/load_chain/load_chain.h"
#include "compiler/program/module/load_chain/load_chain_errors.h"
#include "compiler/program/module/module.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

std::string LoadChain::describe(const Module *reader, const Module *source) {
  std::string chain;
  for (const Module *module : reader->loadPath) {
    chain += (chain.empty() ? "" : " -> ") + module->name;
  }
  if (source != reader) {
    bool started = llvm::is_contained(reader->loadPath, source);
    chain += ErrorTextLoadChain::pending(source->name, started);
  }

  return chain;
}

}
