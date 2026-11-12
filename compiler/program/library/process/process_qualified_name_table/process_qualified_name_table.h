#pragma once

#include "compiler/program/library/process/process_member_table/process_member_table.h"
#include "llvm/ADT/StringRef.h"

namespace tz {

struct ProcessQualifiedNameInfo {
  ProcessMember member;
  llvm::StringLiteral qualifiedName;
};

class ProcessQualifiedNameTable {
public:
  static llvm::StringRef getQualifiedName(ProcessMember member);
};

}
