#include "compiler/semantic/bodies/classes/instance_of_checker/instance_of_checker.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/functions/function_checker/function_checker_errors.h"
#include "compiler/semantic/bodies/top_level_checker/top_level_checker.h"
#include "compiler/semantic/generics/generic_class_checker/generic_class_checker.h"
#include "compiler/semantic/library/bytes/bytes_check_helpers/bytes_check_helpers.h"
#include "compiler/semantic/library/bytes/bytes_checker/bytes_checker.h"
#include "compiler/semantic/names/scopes/scopes.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/unknown_checker/unknown_checker.h"
#include "llvm/ADT/STLExtras.h"

using llvm::dyn_cast;

namespace tz {

InstanceOfChecker::InstanceOfChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->scopes = &all.getScopes();
  this->callChecker = &all.getCallChecker();
  this->exprChecker = &all.getExprChecker();
  this->topLevelChecker = &all.getTopLevelChecker();
}

InstanceOfChecker::ResultInstanceTested InstanceOfChecker::checkInstanceOf(BinaryExpr *bin) {
  bin->lhs = this->exprChecker->checkExpr(bin->lhs);
  bin->type = Type::boolTy();
  auto *name = dyn_cast<NameExpr>(bin->rhs);
  ClassDecl *cls = name ? this->topLevelChecker->lookupValue(name->name).cls() : nullptr;
  for (llvm::StringRef builtin : {"Date", "Map", "Set"}) {
    if (cls || !this->callChecker->isGlobalName(bin->rhs, builtin)) {
      continue;
    }
    auto is = [&](const Type *t) {
      return builtin == "Date" ? t->isDate() : builtin == "Map" ? t->isMap() : t->isSet();
    };
    const Type *type = bin->lhs->type;
    const Type *base = type->nonOptional();
    std::string what = ErrorTextFunctionName::quoted(builtin);
    bool opaque = base->isUnknown() || base->isAnyObject();
    if (opaque && builtin != "Date") {
      llvm::StringRef example = builtin == "Map" ? "Map<string, int>" : "Set<string>";
      std::string message = ErrorTextInstanceOf::untypedMap(builtin, example);

      return err(InstanceOfCheckerCheckInstanceOfError{
          .code = InstanceOfCheckerCheckInstanceOfErrorCode::UNTYPED_COLLECTION,
          .message = message,
      });
    }
    if (opaque) {
      bin->rhs->type = Type::date();
      return ok(bin);
    }
    if (base->isUnion()) {
      std::vector<const Type *> matching;
      for (const Type *m : base->members()) {
        if (is(m)) {
          matching.push_back(m);
        }
      }
      if (matching.empty()) {
        std::string message = ErrorTextInstanceOf::noneIs(base->name(), what);

        return err(InstanceOfCheckerCheckInstanceOfError{
            .code = InstanceOfCheckerCheckInstanceOfErrorCode::NONE_MATCHES,
            .message = message,
        });
      }
      if (matching.size() == base->members().size() && !type->isOptional()) {
        std::string message = ErrorTextInstanceOf::allAre(base->name(), what, builtin);

        return err(InstanceOfCheckerCheckInstanceOfError{
            .code = InstanceOfCheckerCheckInstanceOfErrorCode::ALL_MATCH,
            .message = message,
        });
      }
      bin->rhs->type = Type::unionOf(matching);
      return ok(bin);
    }
    if (is(type)) {
      std::string message = ErrorTextInstanceOf::alwaysTrue(type->name(), builtin);

      return err(InstanceOfCheckerCheckInstanceOfError{
          .code = InstanceOfCheckerCheckInstanceOfErrorCode::ALWAYS_TRUE,
          .message = message,
      });
    }
    if (type->isOptional() && is(type->inner())) {
      std::string message = ErrorTextInstanceOf::orUndef(type->inner()->name());

      return err(InstanceOfCheckerCheckInstanceOfError{
          .code = InstanceOfCheckerCheckInstanceOfErrorCode::TEST_UNDEFINED,
          .message = message,
      });
    }
    if (!type->isError()) {
      std::string message = ErrorTextInstanceOf::neverIs(type->name(), what, builtin);

      return err(InstanceOfCheckerCheckInstanceOfError{
          .code = InstanceOfCheckerCheckInstanceOfErrorCode::ALWAYS_FALSE,
          .message = message,
      });
    }
    return ok(bin);
  }
  if (const Type *bytes = !cls && name && !this->scopes->lookup(name->name).var() &&
                                  !this->scopes->lookup(name->name).func()
                              ? BytesChecker::bytesTypeNamed(name->name)
                              : nullptr) {
    ResultBytesKindTested tested = this->checkBytesInstanceOf(bin, bytes);
    if (tested.isErr()) {
      return err(InstanceOfCheckerCheckInstanceOfError{
          .code = InstanceOfCheckerCheckInstanceOfErrorCode::BYTES_INSTANCE,
          .message = tested.error().message,
      });
    }
    return ok(tested.value());
  }
  if (!cls) {
    bin->rhs = this->exprChecker->checkExpr(bin->rhs);
    const Type *rhs = bin->rhs->type;
    ClassDecl *known = const_cast<ClassDecl *>(rhs->classRef());
    if (known && known->type) {
      cls = known;
    } else if (rhs->isConstructor()) {
      const Type *base = bin->lhs->type->nonOptional();
      bool object = base->isError() || base->isUnknown() || UnknownChecker::isObjectReference(base);
      if (!object) {
        std::string message = ErrorTextInstanceOf::notObject(bin->lhs->type->name());

        return err(InstanceOfCheckerCheckInstanceOfError{
            .code = InstanceOfCheckerCheckInstanceOfErrorCode::NOT_AN_OBJECT,
            .message = message,
        });
      }
      return ok(bin);
    } else if (!rhs->isError()) {
      std::string message = ErrorTextInstanceOf::notClass();

      return err(InstanceOfCheckerCheckInstanceOfError{
          .code = InstanceOfCheckerCheckInstanceOfErrorCode::NOT_A_CLASS,
          .message = message,
      });
    } else {
      return ok(bin);
    }
  }
  if (cls->isGeneric()) {
    const Type *type = bin->lhs->type;
    bin->rhs->type = Type::error();
    if (type->isError()) {
      return ok(bin);
    }
    const Type *base = type->nonOptional();
    llvm::ArrayRef<const Type *> members =
        base->isUnion() ? base->members() : llvm::ArrayRef<const Type *>(base);
    std::vector<const Type *> matching;
    for (const Type *m : members) {
      if (GenericClassChecker::genericInstanceOf(m, cls)) {
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
        std::string other =
            base->isUnknown() || base->isAnyObject() ? generic + "<string>" : base->name().str();
        std::string hint = cls->library() ? ErrorTextInstanceOf::extendHint(generic)
                                          : ErrorTextInstanceOf::unionHint(generic, other);
        std::string message = ErrorTextInstanceOf::cantTell(generic, base->name(), hint);

        return err(InstanceOfCheckerCheckInstanceOfError{
            .code = InstanceOfCheckerCheckInstanceOfErrorCode::GENERIC_UNKNOWN,
            .message = message,
        });
      }
      std::string message = ErrorTextInstanceOf::neverThat(base->name(), cls->name);

      return err(InstanceOfCheckerCheckInstanceOfError{
          .code = InstanceOfCheckerCheckInstanceOfErrorCode::NEVER_GENERIC,
          .message = message,
      });
    }
    if (base->isUnion()) {
      bin->rhs->type = matching.size() == 1 ? matching[0] : Type::unionOf(matching);
    } else {
      bin->rhs->type = GenericClassChecker::genericInstanceOf(base, cls)->type;
    }
    return ok(bin);
  }
  bin->rhs->type = cls->type;
  const Type *type = bin->lhs->type;
  if (type->isError()) {
    return ok(bin);
  }
  const Type *base = type->nonOptional();
  if (base->isUnknown() || base->isAnyObject()) {
    return ok(bin);
  }
  if (base->isUnion()) {
    bool may = llvm::any_of(base->members(), [&](const Type *m) {
      return m->isObject() && (cls->type->isSubtypeOf(m) || m->isSubtypeOf(cls->type));
    });
    if (!may) {
      std::string message = ErrorTextInstanceOf::neverUnion(base->name(), cls->name);

      return err(InstanceOfCheckerCheckInstanceOfError{
          .code = InstanceOfCheckerCheckInstanceOfErrorCode::NEVER_IN_UNION,
          .message = message,
      });
    }
    return ok(bin);
  }
  if (!base->isObject()) {
    std::string message = ErrorTextInstanceOf::notObject(type->name());

    return err(InstanceOfCheckerCheckInstanceOfError{
        .code = InstanceOfCheckerCheckInstanceOfErrorCode::NOT_AN_OBJECT,
        .message = message,
    });
  }
  bool related = cls->type->isSubtypeOf(base) || base->isSubtypeOf(cls->type);
  if (!related && base->isClass()) {
    std::string message = ErrorTextInstanceOf::neverClass(base->name(), cls->name);

    return err(InstanceOfCheckerCheckInstanceOfError{
        .code = InstanceOfCheckerCheckInstanceOfErrorCode::NEVER_CLASS,
        .message = message,
    });
  }
  if (!related && (!base->bases().empty() || base->isBase())) {
    std::string message = ErrorTextInstanceOf::notImpl(base->name(), cls->name);

    return err(InstanceOfCheckerCheckInstanceOfError{
        .code = InstanceOfCheckerCheckInstanceOfErrorCode::NOT_IMPLEMENTED,
        .message = message,
    });
  }
  if (!related) {
    std::string message = ErrorTextInstanceOf::neverMade(base->name(), cls->name);

    return err(InstanceOfCheckerCheckInstanceOfError{
        .code = InstanceOfCheckerCheckInstanceOfErrorCode::NEVER_CREATED,
        .message = message,
    });
  }

  return ok(bin);
}

InstanceOfChecker::ResultBytesKindTested
InstanceOfChecker::checkBytesInstanceOf(BinaryExpr *bin, const Type *bytes) {
  using Bytes = Type::Bytes;
  auto is = [&](const Type *t) {
    return t->isBytes() &&
           (t->bytes() == bytes->bytes() || (bytes->bytes() == Bytes::Uint8 && t->isBuffer()));
  };
  auto may = [&](const Type *t) { return bytes->isBuffer() && t->bytes() == Bytes::Uint8; };
  const Type *type = bin->lhs->type;
  if (type->isError()) {
    return ok(bin);
  }
  const Type *base = type->nonOptional();
  std::string what = ErrorTextFunctionName::quoted(bytes->name());
  bin->rhs->type = bytes;
  if (base->isUnknown() || base->isAnyObject()) {
    return ok(bin);
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
      std::string message = ErrorTextInstanceOf::noneIs(base->name(), what);

      return err(InstanceOfCheckerCheckBytesInstanceOfError{
          .code = InstanceOfCheckerCheckBytesInstanceOfErrorCode::NONE_MATCHES,
          .message = message,
      });
    }
    if (matching.size() == base->members().size() && !type->isOptional()) {
      std::string message = ErrorTextInstanceOf::allAre(base->name(), what, bytes->name());

      return err(InstanceOfCheckerCheckBytesInstanceOfError{
          .code = InstanceOfCheckerCheckBytesInstanceOfErrorCode::ALL_MATCH,
          .message = message,
      });
    }
    if (!bytes->isBuffer()) {
      bin->rhs->type = Type::unionOf(matching);
    }
    return ok(bin);
  }
  if (is(type)) {
    std::string message = ErrorTextInstanceOf::alwaysTrue(type->name(), bytes->name());

    return err(InstanceOfCheckerCheckBytesInstanceOfError{
        .code = InstanceOfCheckerCheckBytesInstanceOfErrorCode::ALWAYS_TRUE,
        .message = message,
    });
  }
  if (type->isOptional() && is(type->inner())) {
    std::string message = ErrorTextInstanceOf::orUndef(type->inner()->name());

    return err(InstanceOfCheckerCheckBytesInstanceOfError{
        .code = InstanceOfCheckerCheckBytesInstanceOfErrorCode::TEST_UNDEFINED,
        .message = message,
    });
  }
  if (!may(base)) {
    std::string message = ErrorTextInstanceOf::neverIs(type->name(), what, bytes->name());

    return err(InstanceOfCheckerCheckBytesInstanceOfError{
        .code = InstanceOfCheckerCheckBytesInstanceOfErrorCode::ALWAYS_FALSE,
        .message = message,
    });
  }

  return ok(bin);
}

}
