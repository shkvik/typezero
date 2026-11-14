#pragma once

#include "compiler/semantic/semantic_state/v2/walk/check_task_context/check_task.h"
#include <vector>

namespace tz {

class CheckTaskContext {
public:
  void pushTask(CheckTask task);


  bool hasTasks() const;


  CheckTask takeTask();

private:
  std::vector<CheckTask> tasks;
};

}
