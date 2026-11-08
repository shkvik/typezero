#include "compiler/semantic/semantic_state/file_system_store/file_system_store.h"

namespace tz {

void FileSystemStore::rememberStatsClass(ClassDecl *cls) {
  this->statsClass = cls;
}

void FileSystemStore::rememberDirentClass(ClassDecl *cls) {
  this->direntClass = cls;
}

ClassDecl *FileSystemStore::getStatsClass() const {
  return this->statsClass;
}

ClassDecl *FileSystemStore::getDirentClass() const {
  return this->direntClass;
}

ClassDecl *FileSystemStore::findClassNamed(llvm::StringRef name) const {
  if (this->statsClass && this->statsClass->name == name) {
    return this->statsClass;
  }
  if (this->direntClass && this->direntClass->name == name) {
    return this->direntClass;
  }

  return nullptr;
}

bool FileSystemStore::isStatsClass(const ClassDecl *cls) const {
  return cls && cls == this->statsClass;
}

bool FileSystemStore::isFileClass(const ClassDecl *cls) const {
  return cls && (cls == this->statsClass || cls == this->direntClass);
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
