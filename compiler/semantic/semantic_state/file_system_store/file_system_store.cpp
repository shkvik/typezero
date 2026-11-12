#include "compiler/semantic/semantic_state/file_system_store/file_system_store.h"

namespace tz {

void FileSystemStore::rememberStatsClass(ClassDecl *classDecl) {
  this->statsClass = classDecl;
}

void FileSystemStore::rememberDirentClass(ClassDecl *classDecl) {
  this->direntClass = classDecl;
}

ClassDecl *FileSystemStore::getStatsClass() const {
  return this->statsClass;
}

ClassDecl *FileSystemStore::getDirentClass() const {
  return this->direntClass;
}

ClassDecl *FileSystemStore::findClassNamed(llvm::StringRef name) const {
  if (this->statsClass != nullptr && this->statsClass->name == name) {
    return this->statsClass;
  }
  if (this->direntClass != nullptr && this->direntClass->name == name) {
    return this->direntClass;
  }

  return nullptr;
}

bool FileSystemStore::isStatsClass(const ClassDecl *classDecl) const {
  return classDecl != nullptr && classDecl == this->statsClass;
}

bool FileSystemStore::isFileClass(const ClassDecl *classDecl) const {
  return classDecl != nullptr && (classDecl == this->statsClass || classDecl == this->direntClass);
}

void FileSystemStore::enterPromiseCall() {
  this->promiseCallDepth += 1;
}

void FileSystemStore::leavePromiseCall() {
  this->promiseCallDepth -= 1;
}

bool FileSystemStore::isInsidePromiseCall() const {
  return this->promiseCallDepth > 0;
}

}
