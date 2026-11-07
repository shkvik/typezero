#include "compiler/program/ast/export_decl/export_decl.h"

namespace tz {

llvm::ArrayRef<ModuleName> ExportDecl::names() const {
  llvm::ArrayRef<ModuleName> exported;
  switch (this->form.kind()) {
    case ExportKind::LOCAL_EXPORT:
      exported = this->form.as<LocalExport>()->names;
      break;
    case ExportKind::RE_EXPORT:
      exported = this->form.as<ReExport>()->names;
      break;
    case ExportKind::EXPORT_ALL:
      break;
  }

  return exported;
}

ExportSource *ExportDecl::source() {
  ExportSource *from = nullptr;
  switch (this->form.kind()) {
    case ExportKind::LOCAL_EXPORT:
      break;
    case ExportKind::RE_EXPORT:
      from = &this->form.as<ReExport>()->source;
      break;
    case ExportKind::EXPORT_ALL:
      from = &this->form.as<ExportAll>()->source;
      break;
  }

  return from;
}

const ExportSource *ExportDecl::source() const {
  const ExportSource *from = nullptr;
  switch (this->form.kind()) {
    case ExportKind::LOCAL_EXPORT:
      break;
    case ExportKind::RE_EXPORT:
      from = &this->form.as<ReExport>()->source;
      break;
    case ExportKind::EXPORT_ALL:
      from = &this->form.as<ExportAll>()->source;
      break;
  }

  return from;
}

}
