#include "compiler/check/classes/instance_of_checker/instance_of_checker.h"
#include "compiler/check/scopes/scopes.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/top_level_checker/top_level_checker.h"
#include "compiler/check/types/unknown_checker/unknown_checker.h"
#include "compiler/library/bytes/bytes_checker/bytes_checker.h"
#include "compiler/library/bytes/bytes_check_helpers/bytes_check_helpers.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::dyn_cast_or_null;

namespace tz {

InstanceOfChecker::InstanceOfChecker(Sema &sema) : sema_(sema) {}

// `x instanceof C` compares the number of x's class with the range of
// numbers of C and the classes down its chain. An object whose type is not
// up C's chain (or down it) is never a C.
Expr *InstanceOfChecker::checkInstanceOf(BinaryExpr *bin) {
  bin->lhs = sema_.exprChecker().checkExpr(bin->lhs);
  bin->type = Type::boolTy();
  auto *name = dyn_cast<NameExpr>(bin->rhs);
  ClassDecl *cls = name ? sema_.topLevelChecker().lookupValue(name->name).cls : nullptr;
  // `x instanceof Date`, `Map` or `Set`: of a union, the members that are
  // one; anywhere else the type says it already. The right side gets the
  // type of those members, whose numbers the check compares (see
  // CodeGen::emitInstanceOf).
  for (llvm::StringRef builtin : {"Date", "Map", "Set"}) {
    if (cls || !sema_.callChecker().isGlobalName(bin->rhs, builtin)) {
      continue;
    }
    auto is = [&](const Type *t) {
      return builtin == "Date" ? t->isDate() : builtin == "Map" ? t->isMap() : t->isSet();
    };
    const Type *type = bin->lhs->type;
    const Type *base = type->nonOptional();
    std::string what = "'" + builtin.str() + "'";
    // An unknown or an object may hold a Date, which the check shows; the
    // types in a Map or a Set no check can tell.
    if (base->isUnknown() || base->isAnyObject()) {
      if (builtin == "Date") {
        bin->rhs->type = Type::date();
      } else {
        sema_.diags_.error(bin->loc(), "'instanceof " + builtin.str() +
                                     "' cannot tell the types of the keys and values, which "
                                     "tz needs to use a " +
                                     builtin.str() + "; keep it in a variable of its type, e.g. '" +
                                     (builtin == "Map" ? "Map<string, int>" : "Set<string>") + "'");
      }
      return bin;
    }
    if (base->isUnion()) {
      std::vector<const Type *> matching;
      for (const Type *m : base->members()) {
        if (is(m)) {
          matching.push_back(m);
        }
      }
      if (matching.empty()) {
        sema_.diags_.error(bin->loc(), "a '" + base->name() + "' is never a " + what +
                                     ": none of its members is one, so this is always false");
      } else if (matching.size() == base->members().size() && !type->isOptional()) {
        sema_.diags_.error(bin->loc(), "every member of '" + base->name() + "' is a " + what +
                                     ", so 'instanceof " + builtin.str() + "' is always true");
      } else {
        bin->rhs->type = Type::unionOf(matching);
      }
      return bin;
    }
    if (is(type)) {
      sema_.diags_.error(bin->loc(), "the value is a '" + type->name() + "', so 'instanceof " +
                                   builtin.str() + "' is always true");
    } else if (type->isOptional() && is(type->inner())) {
      sema_.diags_.error(bin->loc(), "the value is a '" + type->inner()->name() +
                                   "' or undefined; test it with '!= undefined'");
    } else if (!type->isError()) {
      sema_.diags_.error(bin->loc(), "a '" + type->name() + "' is never a " + what + ", so 'instanceof " +
                                   builtin.str() + "' is always false");
    }
    return bin;
  }
  // `x instanceof Uint8Array` and the other types of binary data, as for
  // Date; but a Uint8Array may hold a Buffer, which only the check tells.
  if (const Type *bytes = !cls && name && !sema_.scopes().lookup(name->name).var && !sema_.scopes().lookup(name->name).func
                              ? BytesChecker::bytesTypeNamed(name->name)
                              : nullptr) {
    return checkBytesInstanceOf(bin, bytes);
  }
  if (!cls) {
    // A class as a value: one known when compiling is that class; of one
    // known only when the program runs, its number is compared then
    // (docs/decorators.md, section 6), and the value is a T.
    bin->rhs = sema_.exprChecker().checkExpr(bin->rhs);
    const Type *rhs = bin->rhs->type;
    if (ClassDecl *known = const_cast<ClassDecl *>(rhs->classRef()); known && known->type) {
      cls = known;
    } else if (rhs->isConstructor()) {
      const Type *base = bin->lhs->type->nonOptional();
      if (!base->isError() && !base->isUnknown() && !UnknownChecker::isObjectReference(base)) {
        sema_.diags_.error(bin->lhs->loc(), "'instanceof' tests an object, found '" +
                                          bin->lhs->type->name() +
                                          "', which is never an instance of a class");
      }
      return bin;
    } else {
      if (!rhs->isError()) {
        sema_.diags_.error(bin->rhs->loc(), "the right side of 'instanceof' must be a class");
      }
      return bin;
    }
  }
  // A generic class: the instance the type of the value says, which is a
  // class of its own (of a union, the members that are one).
  if (cls->isGeneric()) {
    const Type *type = bin->lhs->type;
    bin->rhs->type = Type::error();
    if (type->isError()) {
      return bin;
    }
    const Type *base = type->nonOptional();
    llvm::ArrayRef<const Type *> members =
        base->isUnion() ? base->members() : llvm::ArrayRef<const Type *>(base);
    std::vector<const Type *> matching;
    for (const Type *m : members) {
      if (genericInstanceOf(m, cls)) {
        matching.push_back(m);
      }
    }
    if (matching.empty()) {
      // An unknown or an object may be any of them.
      bool related = base->isUnknown() || base->isAnyObject() ||
                     (base->isObject() && cls->type->isSubtypeOf(base));
      for (const Type *m : members) {
        related |= m->isObject() && cls->type->isSubtypeOf(m);
      }
      if (related) {
        std::string generic = cls->name.str();
        std::string hint;
        if (cls->library) { // an emitter of the library: a class that extends it
          hint = "test for a class that extends it, e.g. 'x instanceof Bus' with 'class Bus "
                 "extends " +
                 generic + " {}'";
        } else {
          hint = "keep the value in a variable of a union of the classes it may be, e.g. '" +
                 generic + "<int> | " +
                 (base->isUnknown() || base->isAnyObject() ? generic + "<string>"
                                                           : base->name().str()) +
                 "'";
        }
        sema_.diags_.error(bin->loc(), "'instanceof " + generic + "' cannot tell which '" + generic +
                                     "' a '" + base->name() +
                                     "' would be: each list of type "
                                     "arguments of '" +
                                     generic + "' makes a class of its own; " + hint);
      } else {
        sema_.diags_.error(bin->loc(), "a '" + base->name() + "' is never a '" + cls->name +
                                     "', so this is always false");
      }
      return bin;
    }
    if (base->isUnion()) {
      bin->rhs->type = matching.size() == 1 ? matching[0] : Type::unionOf(matching);
    } else {
      bin->rhs->type = genericInstanceOf(base, cls)->type;
    }
    return bin;
  }
  bin->rhs->type = cls->type;
  const Type *type = bin->lhs->type;
  if (type->isError()) {
    return bin;
  }
  const Type *base = type->nonOptional();
  // An unknown or an object may be of any class (see addUnknownFacts).
  if (base->isUnknown() || base->isAnyObject()) {
    return bin;
  }
  // Of a union: some member must be able to be one (docs/objects.md,
  // section 7); then the check is one of the class numbers.
  if (base->isUnion()) {
    bool may = llvm::any_of(base->members(), [&](const Type *m) {
      return m->isObject() && (cls->type->isSubtypeOf(m) || m->isSubtypeOf(cls->type));
    });
    if (!may) {
      sema_.diags_.error(bin->loc(), "a '" + base->name() + "' is never a '" + cls->name +
                                   "': none of its members is or extends it, so this is always "
                                   "false");
    }
    return bin;
  }
  if (!base->isObject()) {
    sema_.diags_.error(bin->lhs->loc(), "'instanceof' tests an object, found '" + type->name() +
                                      "', which is never an instance of a class");
    return bin;
  }
  if (!cls->type->isSubtypeOf(base) && !base->isSubtypeOf(cls->type)) {
    if (base->isClass()) {
      sema_.diags_.error(bin->loc(), "a '" + base->name() + "' is never a '" + cls->name +
                                   "': classes are told apart by their names, and neither extends "
                                   "the other, so this is always false");
    } else if (!base->bases().empty() || base->isBase()) {
      sema_.diags_.error(bin->loc(), "a '" + base->name() + "' is never a '" + cls->name + "': '" +
                                   cls->name + "' does not implement it, so this is always false");
    } else {
      sema_.diags_.error(bin->loc(), "an object of type '" + base->name() + "' is never a '" + cls->name +
                                   "': only 'new " + cls->name +
                                   "(...)' creates one, so this is always false");
    }
  }

  return bin;
}

Expr *InstanceOfChecker::checkBytesInstanceOf(BinaryExpr *bin, const Type *bytes) {
  using Bytes = Type::Bytes;
  // What a value of type t is for sure, and what it may be.
  auto is = [&](const Type *t) {
    return t->isBytes() &&
           (t->bytes() == bytes->bytes() || (bytes->bytes() == Bytes::Uint8 && t->isBuffer()));
  };
  auto may = [&](const Type *t) { return bytes->isBuffer() && t->bytes() == Bytes::Uint8; };
  const Type *type = bin->lhs->type;
  if (type->isError()) {
    return bin;
  }
  const Type *base = type->nonOptional();
  std::string what = "'" + bytes->name().str() + "'";
  // The right side gets the types the check compares the number of the
  // object with (see CodeGen::emitInstanceOf): the members that are one,
  // or Buffer.
  bin->rhs->type = bytes;
  // An unknown or an object may be one (see addUnknownFacts).
  if (base->isUnknown() || base->isAnyObject()) {
    return bin;
  }
  if (base->isUnion()) {
    std::vector<const Type *> matching;
    bool maybe = false;
    for (const Type *m : base->members()) {
      if (is(m)) {
        matching.push_back(m);
      }
      maybe |= may(m);
    }
    if (matching.empty() && !maybe) {
      sema_.diags_.error(bin->loc(), "a '" + base->name() + "' is never a " + what +
                                   ": none of its members is one, so this is always false");
    } else if (matching.size() == base->members().size() && !type->isOptional()) {
      sema_.diags_.error(bin->loc(), "every member of '" + base->name() + "' is a " + what +
                                   ", so 'instanceof " + bytes->name() + "' is always true");
    } else if (!bytes->isBuffer()) {
      bin->rhs->type = Type::unionOf(matching);
    }
    return bin;
  }
  if (is(type)) {
    sema_.diags_.error(bin->loc(), "the value is a '" + type->name() + "', so 'instanceof " +
                                 bytes->name() + "' is always true");
  } else if (type->isOptional() && is(type->inner())) {
    sema_.diags_.error(bin->loc(), "the value is a '" + type->inner()->name() +
                                 "' or undefined; test it with '!= undefined'");
  } else if (!may(base)) {
    sema_.diags_.error(bin->loc(), "a '" + type->name() + "' is never a " + what + ", so 'instanceof " +
                                 bytes->name() + "' is always false");
  }

  return bin;
}

} // namespace tz
