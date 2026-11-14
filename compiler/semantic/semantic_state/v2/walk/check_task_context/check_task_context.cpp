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

void CheckTaskContext::pushAnswer(const Type *type) {
  this->answers.push_back(type);
}

const Type *CheckTaskContext::takeAnswer() {
  const Type *answer = this->answers.back();
  this->answers.pop_back();

  return answer;
}

}
