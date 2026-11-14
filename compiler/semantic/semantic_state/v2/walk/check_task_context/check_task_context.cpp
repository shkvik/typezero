#include "compiler/semantic/semantic_state/v2/walk/check_task_context/check_task_context.h"

namespace tz {

void CheckTaskContext::pushTask(CheckTask task) {
  this->tasks.push_back(std::move(task));
}

bool CheckTaskContext::hasTasks() const {
  return !this->tasks.empty();
}

CheckTask CheckTaskContext::takeTask() {
  auto task = std::move(this->tasks.back());
  this->tasks.pop_back();

  return task;
}

}
