#include "compiler/load/partial_classes/partial_classes.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

using llvm::dyn_cast;

namespace tz {

PartialClasses::PartialClasses(Diagnostics &owner)
    : diagnostics(owner) {}

void PartialClasses::joinParts(const std::vector<Module *> &modules) {
  for (Module *module : modules) {
    this->walkPartLists(module);
  }
  for (Module *module : modules) {
    this->joinModuleParts(module);
  }
  for (Module *module : modules) {
    this->separateParts(module);
  }
}

void PartialClasses::walkPartLists(Module *module) {
  for (const ImportDecl &importDecl : module->file.imports) {
    auto listed = this->checkListHasPart(module, importDecl);
    bool isShown = listed.isErr() &&
                   listed.error().code == PartialClassesListHasPartErrorCode::NO_PART_OF_THIS_FILE;
    if (isShown) {
      this->diagnostics.error(importDecl.loc, listed.error().message);
    }
  }
}

PartialClasses::ResultListHasPart
PartialClasses::checkListHasPart(const Module *module, const ImportDecl &importDecl) const {
  if (!importDecl.isPartList) {
    return ok(&importDecl);
  }
  const Module *listed = importDecl.from;
  if (listed == nullptr) {
    return err(PartialClassesListHasPartError{
        .code = PartialClassesListHasPartErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  for (const Stmt *item : listed->file.items) {
    const auto *part = dyn_cast<ClassDecl>(item);
    if (part != nullptr && this->isPartOf(part, listed, module)) {
      return ok(&importDecl);
    }
  }
  auto message = ErrorTextListHasPart::noPart(importDecl.path);

  return err(PartialClassesListHasPartError{
      .code = PartialClassesListHasPartErrorCode::NO_PART_OF_THIS_FILE,
      .message = message,
  });
}

bool PartialClasses::isPartOf(const ClassDecl *part, const Module *module,
                              const Module *classModule) const {
  const auto *importDecl = this->findImportOf(module, part->name);

  return part->isPartial && importDecl != nullptr && importDecl->from == classModule;
}

void PartialClasses::joinModuleParts(Module *module) {
  for (Stmt *item : module->file.items) {
    auto *part = dyn_cast<ClassDecl>(item);
    if (part != nullptr && part->isPartial) {
      this->joinPart(module, part);
    }
  }
}

void PartialClasses::joinPart(Module *module, ClassDecl *part) {
  auto target = this->findPartClass(module, part);
  bool isShown =
      target.isErr() && target.error().code != PartialClassesPartClassErrorCode::ALREADY_REPORTED;
  if (isShown) {
    this->diagnostics.error(part->loc(), target.error().message);
  }
  if (target.isErr()) {
    return;
  }
  this->walkPartShape(part);
  auto typeParams = this->checkTypeParams(part, target.value().classDecl);
  if (typeParams.isErr()) {
    return this->diagnostics.error(part->loc(), typeParams.error().message);
  }

  return this->joinMethods(module, part, target.value());
}

PartialClasses::ResultClassOfPart PartialClasses::findPartClass(const Module *module,
                                                                const ClassDecl *part) const {
  const auto *importDecl = this->findImportOf(module, part->name);
  if (importDecl == nullptr) {
    auto message = ErrorTextPartClass::notImported(part->name);

    return err(PartialClassesPartClassError{
        .code = PartialClassesPartClassErrorCode::NOT_IMPORTED,
        .message = message,
    });
  }
  Module *classModule = importDecl->from;
  if (classModule == nullptr) {
    return err(PartialClassesPartClassError{
        .code = PartialClassesPartClassErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  if (classModule->builtin) {
    auto message = ErrorTextPartClass::library(part->name, importDecl->path);

    return err(PartialClassesPartClassError{
        .code = PartialClassesPartClassErrorCode::LIBRARY_CLASS,
        .message = message,
    });
  }
  const auto *imported = this->findImportedName(*importDecl, part->name);
  auto *classDecl = this->findDeclaredClass(classModule, imported->name);
  if (classDecl == nullptr) {
    auto message = ErrorTextPartClass::undeclared(part->name, importDecl->path);

    return err(PartialClassesPartClassError{
        .code = PartialClassesPartClassErrorCode::NOT_DECLARED_THERE,
        .message = message,
    });
  }
  if (!this->isListingPart(classModule, module)) {
    auto file = this->fileName(classModule);
    auto path = this->importPath(classModule, module);
    auto message = ErrorTextPartClass::notListed(part->name, file, path);

    return err(PartialClassesPartClassError{
        .code = PartialClassesPartClassErrorCode::NOT_LISTED,
        .message = message,
    });
  }
  if (classDecl->baseExpr != nullptr) {
    auto message = ErrorTextPartClass::fromValue(part->name);

    return err(PartialClassesPartClassError{
        .code = PartialClassesPartClassErrorCode::EXTENDS_VALUE,
        .message = message,
    });
  }

  return ok(PartTarget{
      .classDecl = classDecl,
      .module = classModule,
  });
}

const ImportDecl *PartialClasses::findImportOf(const Module *module, llvm::StringRef name) const {
  for (const ImportDecl &importDecl : module->file.imports) {
    if (this->findImportedName(importDecl, name) != nullptr) {
      return &importDecl;
    }
  }

  return nullptr;
}

const ModuleName *PartialClasses::findImportedName(const ImportDecl &importDecl,
                                                   llvm::StringRef name) const {
  for (const ModuleName &imported : importDecl.names) {
    if (imported.as == name) {
      return &imported;
    }
  }

  return nullptr;
}

ClassDecl *PartialClasses::findDeclaredClass(const Module *module, llvm::StringRef name) const {
  for (Stmt *item : module->file.items) {
    auto *classDecl = dyn_cast<ClassDecl>(item);
    if (classDecl != nullptr && !classDecl->isPartial && classDecl->name == name) {
      return classDecl;
    }
  }

  return nullptr;
}

bool PartialClasses::isListingPart(const Module *classModule, const Module *module) const {
  for (const ImportDecl &importDecl : classModule->file.imports) {
    if (importDecl.isPartList && importDecl.from == module) {
      return true;
    }
  }

  return false;
}

void PartialClasses::walkPartShape(const ClassDecl *part) {
  auto fields = this->checkNoFields(part);
  if (fields.isErr()) {
    this->diagnostics.error(part->fields.front().loc, fields.error().message);
  }
  auto constructors = this->checkNoConstructors(part);
  if (constructors.isErr()) {
    this->diagnostics.error(part->constructors.front()->loc(), constructors.error().message);
  }
  auto header = this->checkNoHeader(part);
  if (header.isErr()) {
    this->diagnostics.error(part->loc(), header.error().message);
  }
  auto decorators = this->checkNoDecorators(part);
  if (decorators.isErr()) {
    this->diagnostics.error(part->decorators.front().loc, decorators.error().message);
  }
}

PartialClasses::ResultPartShapeOk PartialClasses::checkNoFields(const ClassDecl *part) const {
  if (!part->fields.empty()) {
    auto message = ErrorTextPartShape::field(part->name, part->fields.front().name);

    return err(PartialClassesPartShapeError{
        .code = PartialClassesPartShapeErrorCode::HAS_FIELD,
        .message = message,
    });
  }

  return ok(part);
}

PartialClasses::ResultPartShapeOk PartialClasses::checkNoConstructors(const ClassDecl *part) const {
  if (!part->constructors.empty()) {
    auto message = ErrorTextPartShape::ctor(part->name);

    return err(PartialClassesPartShapeError{
        .code = PartialClassesPartShapeErrorCode::HAS_CONSTRUCTOR,
        .message = message,
    });
  }

  return ok(part);
}

PartialClasses::ResultPartShapeOk PartialClasses::checkNoHeader(const ClassDecl *part) const {
  bool hasHeader = part->base.has_value() || part->baseExpr != nullptr || !part->interfaces.empty();
  if (hasHeader) {
    auto message = ErrorTextPartShape::header(part->name);

    return err(PartialClassesPartShapeError{
        .code = PartialClassesPartShapeErrorCode::HAS_HEADER,
        .message = message,
    });
  }

  return ok(part);
}

PartialClasses::ResultPartShapeOk PartialClasses::checkNoDecorators(const ClassDecl *part) const {
  if (!part->decorators.empty()) {
    auto message = ErrorTextPartShape::decorators(part->name);

    return err(PartialClassesPartShapeError{
        .code = PartialClassesPartShapeErrorCode::HAS_DECORATORS,
        .message = message,
    });
  }

  return ok(part);
}

PartialClasses::ResultPartShapeOk
PartialClasses::checkTypeParams(const ClassDecl *part, const ClassDecl *classDecl) const {
  if (!this->hasSameTypeParams(part, classDecl)) {
    auto params = this->typeParamsText(classDecl);
    auto message = ErrorTextPartShape::typeParams(part->name, params);

    return err(PartialClassesPartShapeError{
        .code = PartialClassesPartShapeErrorCode::OTHER_TYPE_PARAMS,
        .message = message,
    });
  }

  return ok(part);
}

bool PartialClasses::hasSameTypeParams(const ClassDecl *part, const ClassDecl *classDecl) const {
  if (part->typeParams.size() != classDecl->typeParams.size()) {
    return false;
  }
  for (size_t index = 0; index < part->typeParams.size(); ++index) {
    const TypeParamDecl &written = part->typeParams[index];
    bool isSame = written.name == classDecl->typeParams[index].name &&
                  !written.constraint.has_value() && !written.defaultType.has_value();
    if (!isSame) {
      return false;
    }
  }

  return true;
}

std::string PartialClasses::typeParamsText(const ClassDecl *classDecl) const {
  if (classDecl->typeParams.empty()) {
    return "";
  }
  std::string text = "<";
  for (const TypeParamDecl &param : classDecl->typeParams) {
    text += text.size() > 1 ? ", " : "";
    text += param.name.str();
  }

  return text + ">";
}

void PartialClasses::joinMethods(Module *module, const ClassDecl *part, const PartTarget &target) {
  for (FunctionDecl *method : part->methods) {
    auto isNew = this->checkMethodIsNew(target, module, method);
    if (isNew.isErr()) {
      this->diagnostics.error(method->loc(), isNew.error().message);
    } else {
      this->joinMethod(module, method, target.classDecl);
    }
  }
}

PartialClasses::ResultMethodIsNew
PartialClasses::checkMethodIsNew(const PartTarget &target, const Module *module,
                                 const FunctionDecl *method) const {
  const auto *existing = this->findMethodNamed(target.classDecl, method->name);
  if (existing == nullptr) {
    return ok(method);
  }
  const auto *file = this->findMethodFile(existing, target.module);
  if (file != module) {
    auto fileText = this->fileName(file);
    auto message = ErrorTextMethodIsNew::twice(method->name, target.classDecl->name, fileText);

    return err(PartialClassesMethodIsNewError{
        .code = PartialClassesMethodIsNewErrorCode::DECLARED_IN_OTHER_FILE,
        .message = message,
    });
  }

  return ok(method);
}

const FunctionDecl *PartialClasses::findMethodNamed(const ClassDecl *classDecl,
                                                    llvm::StringRef name) const {
  for (const FunctionDecl *method : classDecl->methods) {
    if (method->name == name) {
      return method;
    }
  }

  return nullptr;
}

const Module *PartialClasses::findMethodFile(const FunctionDecl *method,
                                             const Module *classModule) const {
  const Module *partModule = method->member()->partModule;

  return partModule != nullptr ? partModule : classModule;
}

void PartialClasses::joinMethod(Module *module, FunctionDecl *method, ClassDecl *classDecl) {
  this->addMethod(module, method, classDecl);
  bool hasPristine = classDecl->pristine != nullptr && method->pristine != nullptr;
  if (hasPristine) {
    auto *pristineMethod = const_cast<FunctionDecl *>(method->pristine);
    auto *pristineClass = const_cast<ClassDecl *>(classDecl->pristine);
    this->addMethod(module, pristineMethod, pristineClass);
  }
}

void PartialClasses::addMethod(Module *module, FunctionDecl *method, ClassDecl *classDecl) {
  ClassMember *member = method->member();
  member->cls = classDecl;
  member->partModule = module;
  classDecl->methods.push_back(method);
}

void PartialClasses::separateParts(Module *module) {
  auto kept = std::vector<Stmt *>();
  for (Stmt *item : module->file.items) {
    auto *part = dyn_cast<ClassDecl>(item);
    if (part != nullptr && part->isPartial) {
      module->file.parts.push_back(part);
    } else {
      kept.push_back(item);
    }
  }
  module->file.items = std::move(kept);
}

std::string PartialClasses::fileName(const Module *module) const {
  return llvm::sys::path::filename(module->path).str();
}

std::string PartialClasses::importPath(const Module *from, const Module *to) const {
  auto base = llvm::SmallString<256>(llvm::StringRef(from->path));
  auto target = llvm::SmallString<256>(llvm::StringRef(to->path));
  llvm::sys::fs::make_absolute(base);
  llvm::sys::fs::make_absolute(target);
  llvm::sys::path::remove_dots(base, true);
  llvm::sys::path::remove_dots(target, true);
  llvm::sys::path::remove_filename(base);
  auto baseParts =
      std::vector<llvm::StringRef>(llvm::sys::path::begin(base), llvm::sys::path::end(base));
  auto targetParts =
      std::vector<llvm::StringRef>(llvm::sys::path::begin(target), llvm::sys::path::end(target));
  size_t common = 0;
  while (common < baseParts.size() && common < targetParts.size() &&
         baseParts[common] == targetParts[common]) {
    ++common;
  }
  std::string text = common == baseParts.size() ? "./" : "";
  for (size_t index = common; index < baseParts.size(); ++index) {
    text += "../";
  }
  for (size_t index = common; index < targetParts.size(); ++index) {
    text += targetParts[index].str();
    text += index + 1 < targetParts.size() ? "/" : "";
  }

  return text;
}

}
