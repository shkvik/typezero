#pragma once

#include "compiler/program/ast/enum_member/enum_member.h"
#include "compiler/program/ast/stmt/stmt.h"

namespace tz {

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
  bool isString = false;
  bool resolved = false;
  bool resolving = false;
};

}
