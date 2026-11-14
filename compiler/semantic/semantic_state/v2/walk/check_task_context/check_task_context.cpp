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

void CheckTaskContext::pushChecked(Expr *expr) {
  this->checked.push_back(expr);
}

Expr *CheckTaskContext::takeChecked() {
  Expr *expr = this->checked.back();
  this->checked.pop_back();

  return expr;
}

void CheckTaskContext::pushAnswer(const Type *type) {
  this->answers.push_back(type);
}

const Type *CheckTaskContext::takeAnswer() {
  const Type *answer = this->answers.back();
  this->answers.pop_back();

  return answer;
}

std::vector<const Type *> CheckTaskContext::takeAnswers(size_t count) {
  auto first = this->answers.end() - std::ptrdiff_t(count);
  auto taken = std::vector<const Type *>(first, this->answers.end());
  this->answers.erase(first, this->answers.end());

  return taken;
}

void CheckTaskContext::pushVerdict(bool isYes) {
  this->verdicts.push_back(isYes);
}

bool CheckTaskContext::takeVerdict() {
  bool isYes = this->verdicts.back();
  this->verdicts.pop_back();

  return isYes;
}

void CheckTaskContext::pushConditionFlows(ConditionFlows flows) {
  this->conditionFlows.push_back(std::move(flows));
}

ConditionFlows CheckTaskContext::takeConditionFlows() {
  auto flows = std::move(this->conditionFlows.back());
  this->conditionFlows.pop_back();

  return flows;
}

}
