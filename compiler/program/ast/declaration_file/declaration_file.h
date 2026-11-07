#pragma once

#include "compiler/program/ast/access/access.h"
#include "compiler/program/ast/param/param.h"
#include "compiler/program/ast/stmt/stmt.h"
#include "compiler/program/ast/type_param_decl/type_param_decl.h"
#include "compiler/program/ast/type_ref/type_ref.h"
#include "compiler/union/union/union.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/SMLoc.h"
#include <optional>
#include <string>
#include <vector>

namespace tz {

enum class DeclaredMemberKind {
  DECLARED_FIELD,
  DECLARED_METHOD,
  DECLARED_CONSTRUCTOR,
};

struct DeclaredSignature {
  std::vector<TypeParamDecl> typeParams;
  std::vector<Param> params;
};

struct DeclaredField {
  static constexpr DeclaredMemberKind kind = DeclaredMemberKind::DECLARED_FIELD;
  bool readonly = false;
  bool optional = false;
};

struct DeclaredMethod {
  static constexpr DeclaredMemberKind kind = DeclaredMemberKind::DECLARED_METHOD;
  DeclaredSignature signature;
};

struct DeclaredConstructor {
  static constexpr DeclaredMemberKind kind = DeclaredMemberKind::DECLARED_CONSTRUCTOR;
  DeclaredSignature signature;
};

struct DeclaredMember {
  llvm::StringRef name;
  llvm::SMLoc loc;
  Access access = Access::Public;
  bool accessWritten = false;
  bool isStatic = false;
  Union<DeclaredField, DeclaredMethod, DeclaredConstructor> form = DeclaredField{};
  std::optional<TypeRef> type;
};

struct DeclaredClass {
  llvm::StringRef name;
  llvm::SMLoc loc;
  bool isAbstract = false;
  std::vector<TypeParamDecl> typeParams;
  std::vector<DeclaredMember> members;
};

struct DeclarationFile {
  std::string path;
  std::vector<Stmt *> items;
  std::vector<DeclaredClass> classes;
};

}
