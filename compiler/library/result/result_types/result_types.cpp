#include "compiler/library/result/result_types/result_types.h"
#include "compiler/check/scopes/scopes.h"
#include "compiler/check/types/literal_types/literal_types.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/check/types/type_resolver/type_resolver.h"
#include "compiler/declare/type_declarer/type_declarer.h"
#include "llvm/ADT/STLExtras.h"


namespace tz {

namespace {

const llvm::StringLiteral kResultNames[] = {"ok", "err", "Result", "Ok", "Err"};

const llvm::StringLiteral kResultTypes[] = {"Result", "Ok", "Err"};

}

llvm::StringRef ResultTypes::findName(llvm::ArrayRef<llvm::StringLiteral> names,
                                      llvm::StringRef name) {
  for (llvm::StringLiteral n : names) {
    if (n == name) {
      return n;
    }
  }

  return {};
}

ResultTypes::ResultTypes(Checker &owner)
    : checker(owner) {}

llvm::StringRef ResultTypes::resultName(llvm::StringRef name) const {
  ScopeEntry entry = this->checker.scopes().lookup(name);
  if (entry.found()) {
    return {};
  }

  return ResultTypes::findName(kResultNames, name);
}

llvm::StringRef ResultTypes::resultTypeName(llvm::StringRef name) const {
  if (this->checker.typeDeclarer().lookupType(name)) {
    return {};
  }

  return ResultTypes::findName(kResultTypes, name);
}

void ResultTypes::rejectNeverthrowImport(const ImportDecl &decl) {
  this->checker.diags_.error(
      decl.pathLoc, "Result, ok and err are built in: remove the import from 'neverthrow'");
}

const Type *ResultTypes::resolveResultType(const TypeRef &ref, llvm::StringRef name) {
  if (ref.args.size() != 2) {
    this->checker.diags_.error(
        ref.loc, "'" + ref.name + "' takes two type arguments, the value and the error, e.g. '" +
                     ref.name + "<int, string>'");
    return Type::error();
  }
  auto half = [&](const TypeRef &arg) -> const Type * {
    if (arg.kind() == TypeRefKind::NAME_REF && arg.name == "never") {
      return Type::never();
    }

    return this->checker.typeResolver().resolveTypeInner(arg);
  };
  const Type *value = half(ref.args[0]);
  const Type *error = half(ref.args[1]);
  if (name == "Ok") {
    error = Type::never();
  } else if (name == "Err") {
    value = Type::never();
  }
  return Type::result(value, error);
}

const Type *ResultTypes::unifyHalf(const Type *a, const Type *b) {
  if (a == b || b->isNever()) {
    return a;
  }
  if (a->isNever()) {
    return b;
  }
  if (a->isVoid() || b->isVoid()) {
    return nullptr;
  }
  const Type *common = LiteralTypes::unionUnify(a, b);

  return common && !common->isError() ? common : nullptr;
}

void ResultTypes::neverOperands(BinaryExpr *bin) {
  const Type *l = bin->lhs->type;
  const Type *r = bin->rhs->type;
  if (l->isNever() && r->isNever()) {
    bin->lhs = this->checker.typeRelations().convert(bin->lhs, Type::doubleTy());
    bin->rhs = this->checker.typeRelations().convert(bin->rhs, Type::doubleTy());
  } else if (l->isNever() && !r->isError() && !r->isVoid()) {
    bin->lhs = this->checker.typeRelations().convert(bin->lhs, r);
  } else if (r->isNever() && !l->isError() && !l->isVoid()) {
    bin->rhs = this->checker.typeRelations().convert(bin->rhs, l);
  }
}

}
