#pragma once

#include "compiler/semantic/code/loops/for_of_loops/for_of_loops.h"
#include "compiler/semantic/code/loops/plain_loops/plain_loops.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"

namespace tz {

class CodeLoopsRegistry {
public:
  explicit CodeLoopsRegistry(SemanticState &owner);

  void finishLabeledBlock();
  void walkFor(const WalkFor &task);
  void checkJump(const CheckJump &task);
  void walkWhile(const WalkWhile &task);
  void finishFor(const FinishFor &task);
  void walkForOf(const WalkForOf &task);
  void bindForOf(const BindForOf &task);
  void walkLabeled(const WalkLabeled &task);
  void walkDoWhile(const WalkDoWhile &task);
  void afterForInit(const AfterForInit &task);
  void afterForBody(const AfterForBody &task);
  void afterForStep(const AfterForStep &task);
  void leaveLoopBody(const LeaveLoopBody &task);
  void afterForAwait(const AfterForAwait &task);
  void afterIterable(const AfterIterable &task);
  void restoreLoopFlow(const RestoreLoopFlow &task);
  void afterDoWhileBody(const AfterDoWhileBody &task);
  void afterForInObject(const AfterForInObject &task);
  void afterForOfSource(const AfterForOfSource &task);
  void afterForCondition(const AfterForCondition &task);
  void afterForOfBinding(const AfterForOfBinding &task);
  void afterWhileCondition(const AfterWhileCondition &task);
  void afterDoWhileCondition(const AfterDoWhileCondition &task);

private:
  PlainLoops plainLoops;
  ForOfLoops forOfLoops;
};

}
