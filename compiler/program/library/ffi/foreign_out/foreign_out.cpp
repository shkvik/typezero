#include "compiler/program/library/ffi/foreign_out/foreign_out.h"
#include "compiler/program/ast/class_decl/class_decl.h"
#include "compiler/program/library/builtin_names/library_class_table/library_class_table.h"
#include "compiler/program/types/type/type.h"

namespace tz {

bool ForeignOut::isOut(const Type *type) const {
  const ClassDecl *classDecl = type->classDecl();
  if (classDecl == nullptr) {
    return false;
  }
  const ClassDecl *origin = classDecl->generic() != nullptr ? classDecl->generic() : classDecl;

  return origin->library() && LibraryClassTable::isName(origin->name, LibraryClass::FOREIGN_OUT);
}

const Type *ForeignOut::getElement(const Type *type) const {
  auto args = type->typeArgs();

  return args.empty() ? nullptr : args.front();
}

}
