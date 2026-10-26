#include "compiler/check/classes/instance_of_checker/instance_of_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/scopes/scopes.h"
#include "compiler/check/top_level_checker/top_level_checker.h"
#include "compiler/check/types/unknown_checker/unknown_checker.h"
#include "compiler/library/bytes/bytes_check_helpers/bytes_check_helpers.h"
#include "compiler/library/bytes/bytes_checker/bytes_checker.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

InstanceOfChecker::InstanceOfChecker(Checker &checker) : checker_(checker) {}

Expr *InstanceOfChecker::checkInstanceOf(BinaryExpr *bin) {
  bin->lhs = checker_.exprChecker().checkExpr(bin->lhs);
  bin->type = Type::boolTy();
  auto *name = dyn_cast<NameExpr>(bin->rhs);
  ClassDecl *cls = name ? checker_.topLevelChecker().lookupValue(name->name).cls : nullptr;
  for (llvm::StringRef builtin : {"Date", "Map", "Set"}) {
    if (cls || !checker_.callChecker().isGlobalName(bin->rhs, builtin)) {
      continue;
    }
    auto is = [&](const Type *t) {
      return builtin == "Date" ? t->isDate() : builtin == "Map" ? t->isMap() : t->isSet();
    };
    const Type *type = bin->lhs->type;
    const Type *base = type->nonOptional();
    std::string what = "'" + builtin.str() + "'";
    if (base->isUnknown() || base->isAnyObject()) {
      if (builtin == "Date") {
        bin->rhs->type = Type::date();
      } else {
        checker_.diags_.error(bin->loc(),
                              "'instanceof " + builtin.str() +
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
        checker_.diags_.error(bin->loc(),
                              "a '" + base->name() + "' is never a " + what +
                                  ": none of its members is one, so this is always false");
      } else if (matching.size() == base->members().size() && !type->isOptional()) {
        checker_.diags_.error(bin->loc(), "every member of '" + base->name() + "' is a " + what +
                                              ", so 'instanceof " + builtin.str() +
                                              "' is always true");
      } else {
        bin->rhs->type = Type::unionOf(matching);
      }
      return bin;
    }
    if (is(type)) {
      checker_.diags_.error(bin->loc(), "the value is a '" + type->name() + "', so 'instanceof " +
                                            builtin.str() + "' is always true");
    } else if (type->isOptional() && is(type->inner())) {
      checker_.diags_.error(bin->loc(), "the value is a '" + type->inner()->name() +
                                            "' or undefined; test it with '!= undefined'");
    } else if (!type->isError()) {
      checker_.diags_.error(bin->loc(), "a '" + type->name() + "' is never a " + what +
                                            ", so 'instanceof " + builtin.str() +
                                            "' is always false");
    }
    return bin;
  }
  if (const Type *bytes = !cls && name && !checker_.scopes().lookup(name->name).var &&
                                  !checker_.scopes().lookup(name->name).func
                              ? BytesChecker::bytesTypeNamed(name->name)
                              : nullptr) {
    return checkBytesInstanceOf(bin, bytes);
  }
  if (!cls) {
    bin->rhs = checker_.exprChecker().checkExpr(bin->rhs);
    const Type *rhs = bin->rhs->type;
    if (ClassDecl *known = const_cast<ClassDecl *>(rhs->classRef()); known && known->type) {
      cls = known;
    } else if (rhs->isConstructor()) {
      const Type *base = bin->lhs->type->nonOptional();
      if (!base->isError() && !base->isUnknown() && !UnknownChecker::isObjectReference(base)) {
        checker_.diags_.error(bin->lhs->loc(), "'instanceof' tests an object, found '" +
                                                   bin->lhs->type->name() +
                                                   "', which is never an instance of a class");
      }
      return bin;
    } else {
      if (!rhs->isError()) {
        checker_.diags_.error(bin->rhs->loc(), "the right side of 'instanceof' must be a class");
      }
      return bin;
    }
  }
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
      bool related = base->isUnknown() || base->isAnyObject() ||
                     (base->isObject() && cls->type->isSubtypeOf(base));
      for (const Type *m : members) {
        related |= m->isObject() && cls->type->isSubtypeOf(m);
      }
      if (related) {
        std::string generic = cls->name.str();
        std::string hint;
        if (cls->library) {
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
        checker_.diags_.error(bin->loc(), "'instanceof " + generic + "' cannot tell which '" +
                                              generic + "' a '" + base->name() +
                                              "' would be: each list of type "
                                              "arguments of '" +
                                              generic + "' makes a class of its own; " + hint);
      } else {
        checker_.diags_.error(bin->loc(), "a '" + base->name() + "' is never a '" + cls->name +
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
  if (base->isUnknown() || base->isAnyObject()) {
    return bin;
  }
  if (base->isUnion()) {
    bool may = llvm::any_of(base->members(), [&](const Type *m) {
      return m->isObject() && (cls->type->isSubtypeOf(m) || m->isSubtypeOf(cls->type));
    });
    if (!may) {
      checker_.diags_.error(bin->loc(),
                            "a '" + base->name() + "' is never a '" + cls->name +
                                "': none of its members is or extends it, so this is always "
                                "false");
    }
    return bin;
  }
  if (!base->isObject()) {
    checker_.diags_.error(bin->lhs->loc(), "'instanceof' tests an object, found '" + type->name() +
                                               "', which is never an instance of a class");
    return bin;
  }
  if (!cls->type->isSubtypeOf(base) && !base->isSubtypeOf(cls->type)) {
    if (base->isClass()) {
      checker_.diags_.error(bin->loc(),
                            "a '" + base->name() + "' is never a '" + cls->name +
                                "': classes are told apart by their names, and neither extends "
                                "the other, so this is always false");
    } else if (!base->bases().empty() || base->isBase()) {
      checker_.diags_.error(bin->loc(), "a '" + base->name() + "' is never a '" + cls->name +
                                            "': '" + cls->name +
                                            "' does not implement it, so this is always false");
    } else {
      checker_.diags_.error(bin->loc(), "an object of type '" + base->name() + "' is never a '" +
                                            cls->name + "': only 'new " + cls->name +
                                            "(...)' creates one, so this is always false");
    }
  }

  return bin;
}

Expr *InstanceOfChecker::checkBytesInstanceOf(BinaryExpr *bin, const Type *bytes) {
  using Bytes = Type::Bytes;
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
  bin->rhs->type = bytes;
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
      checker_.diags_.error(bin->loc(),
                            "a '" + base->name() + "' is never a " + what +
                                ": none of its members is one, so this is always false");
    } else if (matching.size() == base->members().size() && !type->isOptional()) {
      checker_.diags_.error(bin->loc(), "every member of '" + base->name() + "' is a " + what +
                                            ", so 'instanceof " + bytes->name() +
                                            "' is always true");
    } else if (!bytes->isBuffer()) {
      bin->rhs->type = Type::unionOf(matching);
    }
    return bin;
  }
  if (is(type)) {
    checker_.diags_.error(bin->loc(), "the value is a '" + type->name() + "', so 'instanceof " +
                                          bytes->name() + "' is always true");
  } else if (type->isOptional() && is(type->inner())) {
    checker_.diags_.error(bin->loc(), "the value is a '" + type->inner()->name() +
                                          "' or undefined; test it with '!= undefined'");
  } else if (!may(base)) {
    checker_.diags_.error(bin->loc(), "a '" + type->name() + "' is never a " + what +
                                          ", so 'instanceof " + bytes->name() +
                                          "' is always false");
  }

  return bin;
}

}
