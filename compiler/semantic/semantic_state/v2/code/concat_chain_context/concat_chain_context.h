#pragma once

#include "compiler/program/ast/ast.h"
#include <vector>

namespace tz {

struct SavedConcatChain {
  BinaryExpr *chain = nullptr;
  const BinaryExpr *mixed = nullptr;
};

class ConcatChainContext {
public:
  void enterModuleLoad();
  void leaveModuleLoad();
  void forgetConcatChain();
  void rememberConcatChain(BinaryExpr *chain);
  void rememberMixedConcat(const BinaryExpr *mixed);


  const BinaryExpr *takeMixedConcat();
  const BinaryExpr *getMixedConcat() const;


  BinaryExpr *takeConcatChain();

private:
  BinaryExpr *concatChain = nullptr;
  const BinaryExpr *mixedConcat = nullptr;
  std::vector<SavedConcatChain> chainsBeforeLoads;
};

}
