#pragma once

#include "compiler/program/errors/errors.h"
#include <memory>

namespace tz {

class LoadOrder;
class WideWrites;
class PoolChecker;
class ContextChecker;
class SemanticExternalFacade;
struct Program;

class InterproceduralPass {
public:
  InterproceduralPass(SemanticExternalFacade &owner, Program &checked);
  ~InterproceduralPass();

  void checkLoadOrder();
  Errors checkValueFlows();

private:
  Program &program;
  std::unique_ptr<LoadOrder> loadOrder;
  std::unique_ptr<WideWrites> wideWrites;
  std::unique_ptr<PoolChecker> poolChecker;
  std::unique_ptr<ContextChecker> contextChecker;
};

}
