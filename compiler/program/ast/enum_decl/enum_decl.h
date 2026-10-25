#pragma once

#include "compiler/program/ast/enum_member/enum_member.h"
#include "compiler/program/ast/stmt/stmt.h"

namespace tz {

// `enum Name { A, B = 5, C }`. The members are constants: numbers counting up
// from 0 or from the last initializer, or strings. As a type, the enum is
// `int`, or `string` when its members are strings.
struct EnumDecl : Stmt {
  EnumDecl(llvm::SMLoc loc, llvm::StringRef name) : Stmt(Kind::Enum, loc), name(name) {}
  static bool classof(const Stmt *s) { return s->kind() == Kind::Enum; }

  const EnumMember *find(llvm::StringRef member) const {
    for (const EnumMember &m : members) {
      if (m.name == member) {
        return &m;
      }
    }

    return nullptr;
  }

  llvm::StringRef name;
  std::vector<EnumMember> members;
  // Set by Sema.
  bool isString = false;
  bool resolved = false;
  bool resolving = false;
};

} // namespace tz
