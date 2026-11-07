#include "compiler/pipeline/check_program/check_state/program_use/program_use.h"

namespace tz {

ProgramUse::ProgramUse(SourceFile &program)
    : file(program) {}

void ProgramUse::markUsesAsync() {
  this->file.usesAsync = true;
}

void ProgramUse::markTopLevelAwait() {
  this->file.topLevelAwait = true;
}

void ProgramUse::markComparesFunctions() {
  this->file.comparesFunctions = true;
}

void ProgramUse::markUsesTls() {
  this->file.usesTls = true;
}

bool ProgramUse::isUsingTls() const {
  return this->file.usesTls;
}

void ProgramUse::addSubtype(const Type *type) {
  if (!llvm::is_contained(this->file.subtypes, type)) {
    this->file.subtypes.push_back(type);
  }
}

std::vector<const Type *> ProgramUse::getSubtypes() const {
  return this->file.subtypes;
}

void ProgramUse::addView(const Type *from, const Type *to) {
  std::pair<const Type *, const Type *> view(from, to);
  if (!llvm::is_contained(this->file.views, view)) {
    this->file.views.push_back(view);
  }
}

void ProgramUse::rememberNamedUse(const Expr *use, const NamedUse &named) {
  this->file.namedUses[use] = named;
}

void ProgramUse::addFieldName(llvm::StringRef name) {
  this->file.fieldNames.insert(name);
}

bool ProgramUse::hasFieldName(llvm::StringRef name) const {
  return this->file.fieldNames.contains(name);
}

std::vector<std::string> ProgramUse::getStringConstants(llvm::StringRef name) const {
  return this->file.stringConstants.lookup(name);
}

std::vector<Module *> ProgramUse::getModules() const {
  return this->file.modules;
}

void ProgramUse::addListenerReceiver(const Type *type) {
  this->file.listenerReceivers.push_back(type);
}

void ProgramUse::addInstance(FunctionDecl *fn) {
  this->file.instances.push_back(fn);
}

}
