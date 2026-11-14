#include "compiler/semantic/code/loops/code_loops_registry.h"

namespace tz {

CodeLoopsRegistry::CodeLoopsRegistry(SemanticState &owner)
    : plainLoops(owner),
      forOfLoops(owner) {}

void CodeLoopsRegistry::checkJump(const CheckJump &task) {
  return this->plainLoops.checkJump(task);
}

void CodeLoopsRegistry::walkLabeled(const WalkLabeled &task) {
  return this->plainLoops.walkLabeled(task);
}

void CodeLoopsRegistry::finishLabeledBlock() {
  return this->plainLoops.finishLabeledBlock();
}

void CodeLoopsRegistry::leaveLoopBody(const LeaveLoopBody &task) {
  return this->plainLoops.leaveLoopBody(task);
}

void CodeLoopsRegistry::restoreLoopFlow(const RestoreLoopFlow &task) {
  return this->plainLoops.restoreLoopFlow(task);
}

void CodeLoopsRegistry::walkWhile(const WalkWhile &task) {
  return this->plainLoops.walkWhile(task);
}

void CodeLoopsRegistry::afterWhileCondition(const AfterWhileCondition &task) {
  return this->plainLoops.afterWhileCondition(task);
}

void CodeLoopsRegistry::walkDoWhile(const WalkDoWhile &task) {
  return this->plainLoops.walkDoWhile(task);
}

void CodeLoopsRegistry::afterDoWhileBody(const AfterDoWhileBody &task) {
  return this->plainLoops.afterDoWhileBody(task);
}

void CodeLoopsRegistry::afterDoWhileCondition(const AfterDoWhileCondition &task) {
  return this->plainLoops.afterDoWhileCondition(task);
}

void CodeLoopsRegistry::walkFor(const WalkFor &task) {
  return this->plainLoops.walkFor(task);
}

void CodeLoopsRegistry::afterForInit(const AfterForInit &task) {
  return this->plainLoops.afterForInit(task);
}

void CodeLoopsRegistry::afterForCondition(const AfterForCondition &task) {
  return this->plainLoops.afterForCondition(task);
}

void CodeLoopsRegistry::afterForBody(const AfterForBody &task) {
  return this->plainLoops.afterForBody(task);
}

void CodeLoopsRegistry::afterForStep(const AfterForStep &task) {
  return this->plainLoops.afterForStep(task);
}

void CodeLoopsRegistry::finishFor(const FinishFor &task) {
  return this->plainLoops.finishFor(task);
}

void CodeLoopsRegistry::walkForOf(const WalkForOf &task) {
  return this->forOfLoops.walkForOf(task);
}

void CodeLoopsRegistry::afterForAwait(const AfterForAwait &task) {
  return this->forOfLoops.afterForAwait(task);
}

void CodeLoopsRegistry::afterIterable(const AfterIterable &task) {
  return this->forOfLoops.afterIterable(task);
}

void CodeLoopsRegistry::afterForInObject(const AfterForInObject &task) {
  return this->forOfLoops.afterForInObject(task);
}

void CodeLoopsRegistry::afterForOfSource(const AfterForOfSource &task) {
  return this->forOfLoops.afterForOfSource(task);
}

void CodeLoopsRegistry::bindForOf(const BindForOf &task) {
  return this->forOfLoops.bindForOf(task);
}

void CodeLoopsRegistry::afterForOfBinding(const AfterForOfBinding &task) {
  return this->forOfLoops.afterForOfBinding(task);
}

}
