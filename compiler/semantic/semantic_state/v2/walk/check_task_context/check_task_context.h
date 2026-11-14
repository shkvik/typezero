#pragma once

#include "compiler/semantic/semantic_state/v2/walk/check_task_context/check_task.h"
#include <vector>

namespace tz {

class CheckTaskContext {
public:
  void pushChecked(Expr *expr);
  void pushTask(CheckTask task);
  void pushAnswer(const Type *type);
  void pushConditionFlows(ConditionFlows flows);


  bool hasTasks() const;


  Expr *takeChecked();
  CheckTask takeTask();
  const Type *takeAnswer();
  ConditionFlows takeConditionFlows();

private:
  std::vector<CheckTask> tasks;
  std::vector<Expr *> checked;
  std::vector<const Type *> answers;
  std::vector<ConditionFlows> conditionFlows;
};

}
