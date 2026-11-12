#include "compiler/semantic/bodies/classes/instance_of_checker/instance_of_checker.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
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
  this->scopes = &all.scopes;
  this->callChecker = &all.callChecker;
  this->exprChecker = &all.exprChecker;
  this->bytesChecker = &all.bytesChecker;
  this->topLevelChecker = &all.topLevelChecker;
}

InstanceOfChecker::ResultInstanceTested InstanceOfChecker::checkInstanceOf(BinaryExpr *binary) {
  binary->lhs = this->exprChecker->checkExpr(binary->lhs);
  binary->type = Type::boolTy();
  auto *name = dyn_cast<NameExpr>(binary->rhs);
  ClassDecl *classDecl =
      name != nullptr ? this->topLevelChecker->lookupValue(name->name).cls() : nullptr;
  std::optional<GlobalName> global =
      classDecl == nullptr ? this->collectionGlobal(binary->rhs) : std::nullopt;
  if (global.has_value()) {
    return this->checkCollectionInstanceOf(binary, *global);
  }
  const Type *bytes = classDecl == nullptr ? this->namedBytesType(name) : nullptr;
  if (bytes != nullptr) {
    auto tested = this->checkBytesInstanceOf(binary, bytes);
    if (tested.isErr()) {
      return err(InstanceOfCheckerCheckInstanceOfError{
          .code = InstanceOfCheckerCheckInstanceOfErrorCode::BYTES_INSTANCE,
          .message = tested.error().message,
      });
    }
    return ok(tested.value());
  }
  if (classDecl == nullptr) {
    binary->rhs = this->exprChecker->checkExpr(binary->rhs);
    classDecl = this->knownClassOf(binary->rhs->type);
    if (classDecl == nullptr) {
      return this->checkUnknownRight(binary);
    }
  }
  if (classDecl->isGeneric()) {
    return this->checkGenericInstanceOf(binary, classDecl);
  }

  return this->checkClassInstanceOf(binary, classDecl);
}

std::optional<GlobalName> InstanceOfChecker::collectionGlobal(const Expr *rhs) const {
  static const GlobalName collections[] = {GlobalName::DATE, GlobalName::MAP, GlobalName::SET};
  for (GlobalName global : collections) {
    if (this->callChecker->isGlobalName(rhs, global)) {
      return global;
    }
  }

  return std::nullopt;
}

bool InstanceOfChecker::isCollection(GlobalName global, const Type *type) const {
  if (global == GlobalName::DATE) {
    return type->isDate();
  }
  if (global == GlobalName::MAP) {
    return type->isMap();
  }

  return type->isSet();
}

InstanceOfChecker::ResultInstanceTested
InstanceOfChecker::checkCollectionInstanceOf(BinaryExpr *binary, GlobalName global) const {
  auto builtin = GlobalNameTable::getName(global);
  const Type *type = binary->lhs->type;
  const Type *base = type->nonOptional();
  auto what = ErrorTextFunctionName::quoted(builtin);
  bool isOpaque = base->isUnknown() || base->isAnyObject();
  if (isOpaque && global != GlobalName::DATE) {
    llvm::StringRef example = global == GlobalName::MAP ? "Map<string, int>" : "Set<string>";
    auto message = ErrorTextInstanceOf::untypedMap(builtin, example);

    return err(InstanceOfCheckerCheckInstanceOfError{
        .code = InstanceOfCheckerCheckInstanceOfErrorCode::UNTYPED_COLLECTION,
        .message = message,
    });
  }
  if (isOpaque) {
    binary->rhs->type = Type::date();
    return ok(binary);
  }
  if (base->isUnion()) {
    return this->checkCollectionUnion(binary, global);
  }
  if (this->isCollection(global, type)) {
    auto message = ErrorTextInstanceOf::alwaysTrue(type->name(), builtin);

    return err(InstanceOfCheckerCheckInstanceOfError{
        .code = InstanceOfCheckerCheckInstanceOfErrorCode::ALWAYS_TRUE,
        .message = message,
    });
  }
  if (type->isOptional() && this->isCollection(global, type->inner())) {
    auto message = ErrorTextInstanceOf::orUndef(type->inner()->name());

    return err(InstanceOfCheckerCheckInstanceOfError{
        .code = InstanceOfCheckerCheckInstanceOfErrorCode::TEST_UNDEFINED,
        .message = message,
    });
  }
  if (!type->isError()) {
    auto message = ErrorTextInstanceOf::neverIs(type->name(), what, builtin);

    return err(InstanceOfCheckerCheckInstanceOfError{
        .code = InstanceOfCheckerCheckInstanceOfErrorCode::ALWAYS_FALSE,
        .message = message,
    });
  }

  return ok(binary);
}

InstanceOfChecker::ResultInstanceTested
InstanceOfChecker::checkCollectionUnion(BinaryExpr *binary, GlobalName global) const {
  auto builtin = GlobalNameTable::getName(global);
  const Type *type = binary->lhs->type;
  const Type *base = type->nonOptional();
  auto what = ErrorTextFunctionName::quoted(builtin);
  auto matching = std::vector<const Type *>();
  for (const Type *member : base->members()) {
    if (this->isCollection(global, member)) {
      matching.push_back(member);
    }
  }
  if (matching.empty()) {
    auto message = ErrorTextInstanceOf::noneIs(base->name(), what);

    return err(InstanceOfCheckerCheckInstanceOfError{
        .code = InstanceOfCheckerCheckInstanceOfErrorCode::NONE_MATCHES,
        .message = message,
    });
  }
  if (matching.size() == base->members().size() && !type->isOptional()) {
    auto message = ErrorTextInstanceOf::allAre(base->name(), what, builtin);

    return err(InstanceOfCheckerCheckInstanceOfError{
        .code = InstanceOfCheckerCheckInstanceOfErrorCode::ALL_MATCH,
        .message = message,
    });
  }
  binary->rhs->type = Type::unionOf(matching);

  return ok(binary);
}

const Type *InstanceOfChecker::namedBytesType(const NameExpr *name) const {
  if (name == nullptr) {
    return nullptr;
  }
  bool isLocal = this->scopes->lookup(name->name).var() != nullptr ||
                 this->scopes->lookup(name->name).func() != nullptr;

  return isLocal ? nullptr : this->bytesChecker->bytesTypeNamed(name->name);
}

ClassDecl *InstanceOfChecker::knownClassOf(const Type *rhs) const {
  auto *known = const_cast<ClassDecl *>(rhs->classRef());

  return known != nullptr && known->type != nullptr ? known : nullptr;
}

InstanceOfChecker::ResultInstanceTested
InstanceOfChecker::checkUnknownRight(BinaryExpr *binary) const {
  const Type *rhs = binary->rhs->type;
  if (rhs->isConstructor()) {
    const Type *base = binary->lhs->type->nonOptional();
    bool isObject = base->isError() || base->isUnknown() || UnknownChecker::isObjectReference(base);
    if (!isObject) {
      auto message = ErrorTextInstanceOf::notObject(binary->lhs->type->name());

      return err(InstanceOfCheckerCheckInstanceOfError{
          .code = InstanceOfCheckerCheckInstanceOfErrorCode::NOT_AN_OBJECT,
          .message = message,
      });
    }
    return ok(binary);
  }
  if (!rhs->isError()) {
    auto message = ErrorTextInstanceOf::notClass();

    return err(InstanceOfCheckerCheckInstanceOfError{
        .code = InstanceOfCheckerCheckInstanceOfErrorCode::NOT_A_CLASS,
        .message = message,
    });
  }

  return ok(binary);
}

InstanceOfChecker::ResultInstanceTested
InstanceOfChecker::checkGenericInstanceOf(BinaryExpr *binary, const ClassDecl *classDecl) const {
  const Type *type = binary->lhs->type;
  binary->rhs->type = Type::error();
  if (type->isError()) {
    return ok(binary);
  }
  const Type *base = type->nonOptional();
  llvm::ArrayRef<const Type *> members =
      base->isUnion() ? base->members() : llvm::ArrayRef<const Type *>(base);
  auto matching = std::vector<const Type *>();
  for (const Type *member : members) {
    if (GenericClassChecker::genericInstanceOf(member, classDecl) != nullptr) {
      matching.push_back(member);
    }
  }
  if (matching.empty()) {
    return this->genericMismatch(base, members, classDecl);
  }
  if (base->isUnion()) {
    binary->rhs->type = matching.size() == 1 ? matching[0] : Type::unionOf(matching);
  } else {
    binary->rhs->type = GenericClassChecker::genericInstanceOf(base, classDecl)->type;
  }

  return ok(binary);
}

InstanceOfChecker::ResultInstanceTested
InstanceOfChecker::genericMismatch(const Type *base, llvm::ArrayRef<const Type *> members,
                                   const ClassDecl *classDecl) const {
  bool isRelated = base->isUnknown() || base->isAnyObject() ||
                   (base->isObject() && classDecl->type->isSubtypeOf(base));
  for (const Type *member : members) {
    isRelated |= member->isObject() && classDecl->type->isSubtypeOf(member);
  }
  if (isRelated) {
    std::string generic = classDecl->name.str();
    std::string other =
        base->isUnknown() || base->isAnyObject() ? generic + "<string>" : base->name().str();
    std::string hint = classDecl->library() ? ErrorTextInstanceOf::extendHint(generic)
                                            : ErrorTextInstanceOf::unionHint(generic, other);
    auto message = ErrorTextInstanceOf::cantTell(generic, base->name(), hint);

    return err(InstanceOfCheckerCheckInstanceOfError{
        .code = InstanceOfCheckerCheckInstanceOfErrorCode::GENERIC_UNKNOWN,
        .message = message,
    });
  }
  auto message = ErrorTextInstanceOf::neverThat(base->name(), classDecl->name);

  return err(InstanceOfCheckerCheckInstanceOfError{
      .code = InstanceOfCheckerCheckInstanceOfErrorCode::NEVER_GENERIC,
      .message = message,
  });
}

bool InstanceOfChecker::mayHoldClass(const Type *base, const ClassDecl *classDecl) const {
  for (const Type *member : base->members()) {
    if (member->isObject() &&
        (classDecl->type->isSubtypeOf(member) || member->isSubtypeOf(classDecl->type))) {
      return true;
    }
  }

  return false;
}

InstanceOfChecker::ResultInstanceTested
InstanceOfChecker::checkClassInstanceOf(BinaryExpr *binary, const ClassDecl *classDecl) const {
  binary->rhs->type = classDecl->type;
  const Type *type = binary->lhs->type;
  if (type->isError()) {
    return ok(binary);
  }
  const Type *base = type->nonOptional();
  if (base->isUnknown() || base->isAnyObject()) {
    return ok(binary);
  }
  if (base->isUnion() && !this->mayHoldClass(base, classDecl)) {
    auto message = ErrorTextInstanceOf::neverUnion(base->name(), classDecl->name);

    return err(InstanceOfCheckerCheckInstanceOfError{
        .code = InstanceOfCheckerCheckInstanceOfErrorCode::NEVER_IN_UNION,
        .message = message,
    });
  }
  if (base->isUnion()) {
    return ok(binary);
  }
  if (!base->isObject()) {
    auto message = ErrorTextInstanceOf::notObject(type->name());

    return err(InstanceOfCheckerCheckInstanceOfError{
        .code = InstanceOfCheckerCheckInstanceOfErrorCode::NOT_AN_OBJECT,
        .message = message,
    });
  }

  return this->checkRelatedClass(binary, base, classDecl);
}

InstanceOfChecker::ResultInstanceTested
InstanceOfChecker::checkRelatedClass(BinaryExpr *binary, const Type *base,
                                     const ClassDecl *classDecl) const {
  bool isRelated = classDecl->type->isSubtypeOf(base) || base->isSubtypeOf(classDecl->type);
  if (!isRelated && base->isClass()) {
    auto message = ErrorTextInstanceOf::neverClass(base->name(), classDecl->name);

    return err(InstanceOfCheckerCheckInstanceOfError{
        .code = InstanceOfCheckerCheckInstanceOfErrorCode::NEVER_CLASS,
        .message = message,
    });
  }
  if (!isRelated && (!base->bases().empty() || base->isBase())) {
    auto message = ErrorTextInstanceOf::notImpl(base->name(), classDecl->name);

    return err(InstanceOfCheckerCheckInstanceOfError{
        .code = InstanceOfCheckerCheckInstanceOfErrorCode::NOT_IMPLEMENTED,
        .message = message,
    });
  }
  if (!isRelated) {
    auto message = ErrorTextInstanceOf::neverMade(base->name(), classDecl->name);

    return err(InstanceOfCheckerCheckInstanceOfError{
        .code = InstanceOfCheckerCheckInstanceOfErrorCode::NEVER_CREATED,
        .message = message,
    });
  }

  return ok(binary);
}

InstanceOfChecker::ResultBytesKindTested
InstanceOfChecker::checkBytesInstanceOf(BinaryExpr *binary, const Type *bytes) {
  using Bytes = Type::Bytes;
  auto is = [&](const Type *candidate) {
    return candidate->isBytes() && (candidate->bytes() == bytes->bytes() ||
                                    (bytes->bytes() == Bytes::Uint8 && candidate->isBuffer()));
  };
  auto may = [&](const Type *candidate) {
    return bytes->isBuffer() && candidate->bytes() == Bytes::Uint8;
  };
  const Type *type = binary->lhs->type;
  if (type->isError()) {
    return ok(binary);
  }
  const Type *base = type->nonOptional();
  auto what = ErrorTextFunctionName::quoted(bytes->name());
  binary->rhs->type = bytes;
  if (base->isUnknown() || base->isAnyObject()) {
    return ok(binary);
  }
  if (base->isUnion()) {
    auto matching = std::vector<const Type *>();
    bool isMaybe = false;
    for (const Type *member : base->members()) {
      if (is(member)) {
        matching.push_back(member);
      }
      isMaybe |= may(member);
    }
    if (matching.empty() && !isMaybe) {
      auto message = ErrorTextInstanceOf::noneIs(base->name(), what);

      return err(InstanceOfCheckerCheckBytesInstanceOfError{
          .code = InstanceOfCheckerCheckBytesInstanceOfErrorCode::NONE_MATCHES,
          .message = message,
      });
    }
    if (matching.size() == base->members().size() && !type->isOptional()) {
      auto message = ErrorTextInstanceOf::allAre(base->name(), what, bytes->name());

      return err(InstanceOfCheckerCheckBytesInstanceOfError{
          .code = InstanceOfCheckerCheckBytesInstanceOfErrorCode::ALL_MATCH,
          .message = message,
      });
    }
    if (!bytes->isBuffer()) {
      binary->rhs->type = Type::unionOf(matching);
    }
    return ok(binary);
  }
  if (is(type)) {
    auto message = ErrorTextInstanceOf::alwaysTrue(type->name(), bytes->name());

    return err(InstanceOfCheckerCheckBytesInstanceOfError{
        .code = InstanceOfCheckerCheckBytesInstanceOfErrorCode::ALWAYS_TRUE,
        .message = message,
    });
  }
  if (type->isOptional() && is(type->inner())) {
    auto message = ErrorTextInstanceOf::orUndef(type->inner()->name());

    return err(InstanceOfCheckerCheckBytesInstanceOfError{
        .code = InstanceOfCheckerCheckBytesInstanceOfErrorCode::TEST_UNDEFINED,
        .message = message,
    });
  }
  if (!may(base)) {
    auto message = ErrorTextInstanceOf::neverIs(type->name(), what, bytes->name());

    return err(InstanceOfCheckerCheckBytesInstanceOfError{
        .code = InstanceOfCheckerCheckBytesInstanceOfErrorCode::ALWAYS_FALSE,
        .message = message,
    });
  }

  return ok(binary);
}

}
