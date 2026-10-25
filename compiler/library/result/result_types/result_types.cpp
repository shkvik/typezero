#include "compiler/library/result/result_types/result_types.h"
#include "compiler/check/scopes/scopes.h"
#include "compiler/check/types/type_resolver/type_resolver.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/declare/type_declarer/type_declarer.h"
#include "compiler/check/types/literal_types/literal_types.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::dyn_cast_or_null;

namespace tz {

namespace {

// The names neverthrow exports. Those after safeTry come with exceptions or
// with async.
const llvm::StringLiteral kNeverthrowNames[] = {"ok",
                                                "err",
                                                "Result",
                                                "Ok",
                                                "Err",
                                                "safeTry",
                                                "fromThrowable",
                                                "fromAsyncThrowable",
                                                "ResultAsync",
                                                "okAsync",
                                                "errAsync",
                                                "fromPromise",
                                                "fromSafePromise"};

const llvm::StringLiteral kResultTypes[] = {"Result", "Ok", "Err", "ResultAsync"};

llvm::StringRef findName(llvm::ArrayRef<llvm::StringLiteral> names, llvm::StringRef name) {
  for (llvm::StringLiteral n : names) {
    if (n == name) {
      return n;
    }
  }

  return {};
}

} // namespace

ResultTypes::ResultTypes(Sema &sema) : sema_(sema) {}

llvm::StringRef ResultTypes::neverthrowName(llvm::StringRef name) const {
  Sema::ScopeEntry entry = sema_.scopes().lookup(name);
  if (entry.var || entry.func || entry.enumDecl || entry.cls) {
    return {};
  }
  if (sema_.module_) {
    const llvm::StringMap<llvm::StringRef> &imported = sema_.moduleStates_[sema_.module_->order].neverthrow;
    if (auto found = imported.find(name); found != imported.end()) {
      return found->second;
    }
  }

  return findName(kNeverthrowNames, name);
}

llvm::StringRef ResultTypes::neverthrowTypeName(llvm::StringRef name) const {
  if (sema_.typeDeclarer().lookupType(name)) {
    return {};
  }
  if (sema_.module_) {
    const llvm::StringMap<llvm::StringRef> &imported = sema_.moduleStates_[sema_.module_->order].neverthrow;
    if (auto found = imported.find(name); found != imported.end()) {
      return findName(kResultTypes, found->second);
    }
  }

  return findName(kResultTypes, name);
}

void ResultTypes::importNeverthrow(Module *module, const ImportDecl &decl) {
  Sema::ModuleState &here = sema_.state(module);
  for (const ModuleName &name : decl.names) {
    llvm::StringRef known = findName(kNeverthrowNames, name.name);
    if (known.empty()) {
      sema_.diags_.error(name.loc, "'neverthrow' has no export named '" + name.name + "'");
      continue;
    }
    if (auto own = here.declared.find(name.as); own != here.declared.end()) {
      sema_.diags_.error(name.asLoc, "import '" + name.as + "' conflicts with the declaration of '" +
                                   name.as +
                                   "' in this file; import it under another name, e.g. "
                                   "'import { " +
                                   name.name + " as " + name.name + "2 } from \"neverthrow\"'");
      sema_.diags_.note(own->second, "'" + name.as + "' is declared here");
      continue;
    }
    auto [it, inserted] = here.neverthrow.try_emplace(name.as, known);
    if (!inserted) {
      sema_.diags_.error(name.asLoc, "duplicate import of '" + name.as + "'");
    }
  }
}

const Type *ResultTypes::resolveResultType(const TypeRef &ref, llvm::StringRef name) {
  if (ref.args.size() != 2) {
    sema_.diags_.error(ref.loc, "'" + ref.name +
                              "' takes two type arguments, the value and the error, e.g. '" +
                              ref.name + "<int, string>'");
    return Type::error();
  }
  auto half = [&](const TypeRef &arg) -> const Type * {
    if (arg.kind == TypeRef::Kind::Name && arg.name == "never") {
      return Type::never();
    }

    return sema_.typeResolver().resolveTypeInner(arg); // void and undefined are halves without a value
  };
  const Type *value = half(ref.args[0]), *error = half(ref.args[1]);
  // An Ok is a Result that cannot be an Err, and an Err one that cannot be an
  // Ok.
  if (name == "Ok") {
    error = Type::never();
  } else if (name == "Err") {
    value = Type::never();
  }
  // A ResultAsync is a Promise of a Result: `await` gives the Result.
  if (name == "ResultAsync") {
    return Type::promise(Type::result(value, error));
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
  // Errors from different places may be of different types: `E | F`.
  const Type *common = LiteralTypes::unionUnify(a, b);

  return common && !common->isError() ? common : nullptr;
}

void ResultTypes::neverOperands(BinaryExpr *bin) {
  const Type *l = bin->lhs->type, *r = bin->rhs->type;
  if (l->isNever() && r->isNever()) {
    bin->lhs = sema_.typeRelations().convert(bin->lhs, Type::doubleTy());
    bin->rhs = sema_.typeRelations().convert(bin->rhs, Type::doubleTy());
  } else if (l->isNever() && !r->isError() && !r->isVoid()) {
    bin->lhs = sema_.typeRelations().convert(bin->lhs, r);
  } else if (r->isNever() && !l->isError() && !l->isVoid()) {
    bin->rhs = sema_.typeRelations().convert(bin->rhs, l);
  }
}

} // namespace tz
