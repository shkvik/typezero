#include "compiler/semantic/semantic_state/v2/inference/return_inference_context/return_inference_context.h"
#include "compiler/semantic/flow/placed_nodes/placed_nodes.h"
#include "llvm/ADT/STLExtras.h"

using llvm::dyn_cast;

namespace tz {

void ReturnInferenceContext::enterInference(const FuncSymbol *function) {
  this->inferring.push_back(function);
}

void ReturnInferenceContext::leaveInference() {
  this->inferring.pop_back();
}

void ReturnInferenceContext::enterModuleLoad() {
  this->suspendedBeforeLoads.push_back(this->suspended);
  this->suspended.insert(this->inferring.begin(), this->inferring.end());
}

void ReturnInferenceContext::leaveModuleLoad() {
  this->suspended = std::move(this->suspendedBeforeLoads.back());
  this->suspendedBeforeLoads.pop_back();
}

bool ReturnInferenceContext::isSuspended(const FuncSymbol *function) const {
  return this->suspended.contains(function);
}

void ReturnInferenceContext::forgetSuspended(const FuncSymbol *function) {
  this->suspended.erase(function);
}

void ReturnInferenceContext::rememberReturnSource(const FunctionDecl *functionDecl,
                                                  FunctionDecl *source) {
  this->returnSources[functionDecl] = source;
}

bool ReturnInferenceContext::hasReturnSource(const FunctionDecl *functionDecl) const {
  return this->returnSources.count(functionDecl) > 0;
}

FunctionDecl *ReturnInferenceContext::findReturnSource(const FunctionDecl *functionDecl) const {
  return this->returnSources.lookup(functionDecl);
}

bool ReturnInferenceContext::isInferring(const FuncSymbol *function) const {
  return llvm::is_contained(this->inferring, function);
}

void ReturnInferenceContext::rememberInheritedReturn(const FunctionDecl *functionDecl) {
  this->inheritedReturns.insert(functionDecl);
}

bool ReturnInferenceContext::hasInheritedReturn(const FunctionDecl *functionDecl) const {
  return this->inheritedReturns.contains(functionDecl);
}

void ReturnInferenceContext::pushSavedBody(Checkpoint saved) {
  this->savedBodies.push_back(std::move(saved));
}

Checkpoint ReturnInferenceContext::takeSavedBody() {
  Checkpoint saved = std::move(this->savedBodies.back());
  this->savedBodies.pop_back();

  return saved;
}

CollectedReturns &ReturnInferenceContext::openCollection() {
  this->collections.push_back(CollectedReturns{.returns = {}, .yields = {}});

  return this->collections.back();
}

CollectedReturns ReturnInferenceContext::takeCollection() {
  CollectedReturns collected = std::move(this->collections.back());
  this->collections.pop_back();

  return collected;
}

bool ReturnInferenceContext::isReturningValue(const Stmt *body) const {
  auto nodes = PlacedNodes::collect({body}, {});
  for (const PlacedStmt &placed : nodes.getStmts()) {
    auto *returnStmt = dyn_cast<ReturnStmt>(placed.stmt);
    bool isOwn = returnStmt != nullptr && !placed.place.isInsideFunction();
    if (isOwn && this->isGivingValue(returnStmt)) {
      return true;
    }
  }

  return false;
}

bool ReturnInferenceContext::isGivingValue(const ReturnStmt *returnStmt) const {
  if (returnStmt->value == nullptr) {
    return false;
  }
  auto *unary = dyn_cast<UnaryExpr>(returnStmt->value);
  bool isDiscarded = unary != nullptr && unary->op == UnaryOp::Void;

  return !isDiscarded;
}

}
