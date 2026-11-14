#include "compiler/semantic/semantic_state/v2/inference/return_inference_context/return_inference_context.h"
#include "llvm/ADT/STLExtras.h"

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

}
