#include "compiler/semantic/semantic_state/v2/declaration/class_list_context/class_list_context.h"

namespace tz {

unsigned ClassListContext::takeClassId() {
  this->classCount += 1;

  return this->classCount;
}

void ClassListContext::rememberClassCount(unsigned count) {
  this->classCount = count;
}

void ClassListContext::addClass(ClassDecl *classDecl) {
  this->classes.push_back(classDecl);
}

std::vector<ClassDecl *> ClassListContext::getClasses() const {
  return this->classes;
}

void ClassListContext::markClassesLinked() {
  this->classesLinked = true;
}

bool ClassListContext::hasLinkedClasses() const {
  return this->classesLinked;
}

void ClassListContext::markMethodsDeclared() {
  this->methodsDeclared = true;
}

bool ClassListContext::hasDeclaredMethods() const {
  return this->methodsDeclared;
}

void ClassListContext::enterCompletion(const ClassDecl *classDecl) {
  this->completing.insert(classDecl);
}

void ClassListContext::leaveCompletion(const ClassDecl *classDecl) {
  this->completing.erase(classDecl);
}

bool ClassListContext::isCompleting(const ClassDecl *classDecl) const {
  return this->completing.contains(classDecl);
}

}
