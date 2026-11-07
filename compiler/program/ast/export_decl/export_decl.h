#pragma once

#include "compiler/program/ast/module_name/module_name.h"
#include "compiler/union/union/union.h"
#include "llvm/ADT/ArrayRef.h"

namespace tz {

struct Module;

enum class ExportKind {
  LOCAL_EXPORT,
  RE_EXPORT,
  EXPORT_ALL,
};

struct ExportSource {
  std::string path;
  llvm::SMLoc pathLoc;
  Module *from = nullptr;
};

struct LocalExport {
  static constexpr ExportKind kind = ExportKind::LOCAL_EXPORT;
  std::vector<ModuleName> names;
};

struct ReExport {
  static constexpr ExportKind kind = ExportKind::RE_EXPORT;
  std::vector<ModuleName> names;
  ExportSource source;
};

struct ExportAll {
  static constexpr ExportKind kind = ExportKind::EXPORT_ALL;
  ExportSource source;
};

using ExportForm = Union<LocalExport, ReExport, ExportAll>;

struct ExportDecl {
  llvm::SMLoc loc;
  ExportForm form = LocalExport{};

  llvm::ArrayRef<ModuleName> names() const;
  ExportSource *source();
  const ExportSource *source() const;
};

}
