#include "compiler/semantic/semantic_state/v2/code/concat_chain_context/concat_chain_context.h"

namespace tz {

void ConcatChainContext::rememberConcatChain(BinaryExpr *chain) {
  this->concatChain = chain;
}

void ConcatChainContext::forgetConcatChain() {
  this->concatChain = nullptr;
}

BinaryExpr *ConcatChainContext::takeConcatChain() {
  BinaryExpr *chain = this->concatChain;
  this->concatChain = nullptr;

  return chain;
}

void ConcatChainContext::rememberMixedConcat(const BinaryExpr *mixed) {
  this->mixedConcat = mixed;
}

const BinaryExpr *ConcatChainContext::takeMixedConcat() {
  const BinaryExpr *mixed = this->mixedConcat;
  this->mixedConcat = nullptr;

  return mixed;
}

const BinaryExpr *ConcatChainContext::getMixedConcat() const {
  return this->mixedConcat;
}

void ConcatChainContext::enterModuleLoad() {
  auto saved = SavedConcatChain{.chain = this->concatChain, .mixed = this->mixedConcat};
  this->chainsBeforeLoads.push_back(saved);
  this->concatChain = nullptr;
  this->mixedConcat = nullptr;
}

void ConcatChainContext::leaveModuleLoad() {
  SavedConcatChain saved = this->chainsBeforeLoads.back();
  this->chainsBeforeLoads.pop_back();
  this->concatChain = saved.chain;
  this->mixedConcat = saved.mixed;
}

}
