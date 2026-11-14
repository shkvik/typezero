#include "compiler/semantic/semantic_state/v2/library/file_system_context/file_system_context.h"

namespace tz {

void FileSystemContext::rememberStatsClass(ClassDecl *classDecl) {
  this->statsClass = classDecl;
}

void FileSystemContext::rememberDirentClass(ClassDecl *classDecl) {
  this->direntClass = classDecl;
}

ClassDecl *FileSystemContext::getStatsClass() const {
  return this->statsClass;
}

ClassDecl *FileSystemContext::getDirentClass() const {
  return this->direntClass;
}

bool FileSystemContext::isStatsClass(const ClassDecl *classDecl) const {
  return classDecl != nullptr && classDecl == this->statsClass;
}

bool FileSystemContext::isFileClass(const ClassDecl *classDecl) const {
  return classDecl != nullptr && (classDecl == this->statsClass || classDecl == this->direntClass);
}

void FileSystemContext::enterPromiseCall() {
  this->promiseCallDepth += 1;
}

void FileSystemContext::leavePromiseCall() {
  this->promiseCallDepth -= 1;
}

bool FileSystemContext::isInsidePromiseCall() const {
  return this->promiseCallDepth > 0;
}

}
