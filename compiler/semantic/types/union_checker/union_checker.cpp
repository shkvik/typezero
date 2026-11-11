#include "compiler/semantic/types/union_checker/union_checker.h"
#include "compiler/semantic/bodies/classes/class_bases/class_bases.h"
#include "compiler/semantic/bodies/classes/class_links/class_links.h"
#include "compiler/semantic/bodies/classes/class_member_checker/class_member_checker.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/objects/object_checker/object_checker.h"
#include "compiler/semantic/declare/class_declarer/class_declarer.h"
#include "compiler/semantic/flow/field_facts/field_facts.h"
#include "compiler/semantic/infer/return_inferrer/return_inferrer.h"
#include "compiler/semantic/library/bytes/bytes_checker/bytes_checker.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/union_check_helpers/union_check_helpers.h"

namespace tz {

UnionChecker::UnionChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->fieldFacts = &all.fieldFacts;
  this->bytesChecker = &all.bytesChecker;
  this->classDeclarer = &all.classDeclarer;
  this->returnInferrer = &all.returnInferrer;
  this->argumentChecker = &all.argumentChecker;
  this->classMemberChecker = &all.classMemberChecker;
}

llvm::ArrayRef<const Type *> UnionChecker::membersOf(const Type *&type) {
  return type->isUnion() ? type->members() : llvm::ArrayRef<const Type *>(type);
}

bool UnionChecker::isLiteralSet(const Type *type) {
  if (type->isLiteral()) {
    return true;
  }

  return type->isUnion() &&
         llvm::all_of(type->members(), [](const Type *m) { return m->isLiteral(); });
}

std::string UnionChecker::lackingHint(const MemberExpr *member, const Type *type) {
  auto literals = [](const Type *t) {
    return t->isLiteral() || (t->isUnion() && llvm::all_of(t->members(), [](const Type *m) {
                                return m->isLiteral();
                              }));
  };
  auto holds = [](const Type *set, const Type *literal) {
    return set == literal || (set->isUnion() && llvm::is_contained(set->members(), literal));
  };
  std::string fallback;
  for (const Field &f : type->members().front()->fields()) {
    bool tags = llvm::all_of(type->members(), [&](const Type *m) {
      const Field *g = m->field(f.name);
      return g && literals(g->type);
    });
    if (!tags) {
      continue;
    }
    for (const Type *m : type->members()) {
      const Type *tag = m->field(f.name)->type;
      if (!m->field(member->name) || !tag->isLiteral()) {
        continue;
      }
      std::string field = ObjectChecker::fieldText(member->object, f.name);
      std::string hint = ErrorTextUnionMember::tagHint(field, tag->name());
      bool unique = llvm::none_of(type->members(), [&](const Type *other) {
        return !other->field(member->name) && holds(other->field(f.name)->type, tag);
      });
      if (unique) {
        return hint;
      }
      if (fallback.empty()) {
        fallback = hint;
      }
      break;
    }
  }
  if (!fallback.empty()) {
    return fallback;
  }
  const Type *with = nullptr;
  for (const Type *m : type->members()) {
    if (m->isObject() && m->field(member->name)) {
      with = m;
    }
  }
  if (with && with->isClass()) {
    return ErrorTextUnionMember::classHint(with->name());
  }
  if (with) {
    return ErrorTextUnionMember::inHint(member->name);
  }

  return "";
}

UnionChecker::ResultUnionMemberTyped UnionChecker::checkUnionMember(MemberExpr *member) {
  const Type *type = member->object->type;
  if (type->isTagged()) {
    const char *typeOf = TypeSpelling::typeOfName(type->members().front());
    std::string message = ErrorTextUnionMember::narrow(member->name, type->name(), typeOf);

    return err(UnionCheckerCheckUnionMemberError{
        .code = UnionCheckerCheckUnionMemberErrorCode::TAGGED_UNION,
        .message = message,
    });
  }
  if (UnionChecker::isAllBytes(type)) {
    Expr *bytes = this->bytesChecker->checkBytesUnionMember(member);

    return ok(bytes);
  }
  std::vector<const Type *> fieldTypes;
  const Type *lacking = nullptr;
  for (const Type *m : type->members()) {
    ClassDecl *cls = ClassDeclarer::classOf(m);
    if (cls != nullptr) {
      this->classDeclarer->completeClass(cls);
    }
    std::optional<std::string> hidden = this->accessProblem(member, cls);
    if (hidden.has_value()) {
      return err(UnionCheckerCheckUnionMemberError{
          .code = UnionCheckerCheckUnionMemberErrorCode::NOT_ACCESSIBLE,
          .message = *hidden,
      });
    }
    const Field *f = UnionChecker::unionMemberField(member, m, cls);
    if (f == nullptr) {
      lacking = m;
      break;
    }
    if (!llvm::is_contained(fieldTypes, f->type)) {
      fieldTypes.push_back(f->type);
    }
  }
  if (lacking != nullptr) {
    return UnionChecker::lackingFieldError(member, type, lacking);
  }
  const Type *result = Type::unionOf(fieldTypes);
  std::string problem = UnionSlots::unionProblem(result);
  if (!problem.empty()) {
    std::string message = ErrorTextUnionMember::clash(type->name(), member->name, problem);

    return err(UnionCheckerCheckUnionMemberError{
        .code = UnionCheckerCheckUnionMemberErrorCode::FIELD_TYPES_CLASH,
        .message = message,
    });
  }
  member->type = result;

  return ok(member);
}

bool UnionChecker::isAllBytes(const Type *type) {
  for (const Type *m : type->members()) {
    if (!m->isBytes()) {
      return false;
    }
  }

  return true;
}

std::optional<std::string> UnionChecker::accessProblem(const MemberExpr *member,
                                                       const ClassDecl *cls) {
  if (cls == nullptr) {
    return std::nullopt;
  }
  const ClassDecl *owner = nullptr;
  const ClassField *field = ClassBases::findField(cls, member->name, &owner);
  if (field == nullptr) {
    return std::nullopt;
  }
  ClassMemberChecker::ResultMemberAccessible accessible =
      this->classMemberChecker->checkAccess(owner, field->access, member->name);
  if (accessible.isOk()) {
    return std::nullopt;
  }

  return accessible.error().message;
}

const Field *UnionChecker::unionMemberField(const MemberExpr *member, const Type *m,
                                            const ClassDecl *cls) {
  const Field *f = m->isObject() ? m->field(member->name) : nullptr;
  if (cls == nullptr) {
    return f;
  }

  return ClassBases::findField(cls, member->name) != nullptr ? f : nullptr;
}

UnionChecker::ResultUnionMemberTyped
UnionChecker::lackingFieldError(const MemberExpr *member, const Type *type, const Type *lacking) {
  bool nowhere = true;
  bool plainObjects = true;
  for (const Type *m : type->members()) {
    bool plain = m->isObject() && !m->isClass();
    nowhere &= !(plain && m->field(member->name) != nullptr);
    plainObjects &= plain;
  }
  if (nowhere && plainObjects) {
    std::string message = ErrorTextUnionMember::noField(type->name(), member->name);

    return err(UnionCheckerCheckUnionMemberError{
        .code = UnionCheckerCheckUnionMemberErrorCode::NO_SUCH_FIELD,
        .message = message,
    });
  }
  std::string hint = UnionChecker::lackingHint(member, type);
  std::string message =
      ErrorTextUnionMember::mayLack(member->name, lacking->name(), type->name(), hint);

  return err(UnionCheckerCheckUnionMemberError{
      .code = UnionCheckerCheckUnionMemberErrorCode::MAY_LACK_FIELD,
      .message = message,
  });
}

UnionChecker::ResultUnionFieldTarget UnionChecker::checkUnionFieldTarget(MemberExpr *member) {
  const Type *type = member->object->type;
  const Type *field = nullptr;
  for (const Type *m : type->members()) {
    if (ClassDecl *incomplete = ClassDeclarer::classOf(m)) {
      this->classDeclarer->completeClass(incomplete);
    }
    const Field *f = m->isObject() ? m->field(member->name) : nullptr;
    const ClassDecl *cls = ClassDeclarer::classOf(m);
    const ClassDecl *owner = nullptr;
    const ClassField *declared = cls ? ClassBases::findField(cls, member->name, &owner) : nullptr;
    bool missing = !f || (cls && (!declared || declared->method()));
    if (missing) {
      std::string message =
          ErrorTextUnionMember::mayLack(member->name, m->name(), type->name(), "");

      return err(UnionCheckerCheckUnionFieldTargetError{
          .code = UnionCheckerCheckUnionFieldTargetErrorCode::MAY_LACK_FIELD,
          .message = message,
      });
    }
    ClassMemberChecker::ResultMemberAccessible accessible =
        declared ? this->classMemberChecker->checkAccess(owner, declared->access, member->name)
                 : ClassMemberChecker::ResultMemberAccessible(ok(cls));
    if (accessible.isErr()) {
      return err(UnionCheckerCheckUnionFieldTargetError{
          .code = UnionCheckerCheckUnionFieldTargetErrorCode::NOT_ACCESSIBLE,
          .message = accessible.error().message,
      });
    }
    if (f->readonly) {
      std::string message = ErrorTextUnionTarget::readOnly(member->name, m->name());

      return err(UnionCheckerCheckUnionFieldTargetError{
          .code = UnionCheckerCheckUnionFieldTargetErrorCode::READ_ONLY_FIELD,
          .message = message,
      });
    }
    if (field && f->type != field) {
      std::string message =
          ErrorTextUnionTarget::differ(type->name(), member->name, field->name(), f->type->name());

      return err(UnionCheckerCheckUnionFieldTargetError{
          .code = UnionCheckerCheckUnionFieldTargetErrorCode::FIELD_TYPES_DIFFER,
          .message = message,
      });
    }
    field = f->type;
  }
  this->fieldFacts->fieldsAssigned(member->name);
  member->type = field;

  return ok(field);
}

const Type *UnionChecker::commonBaseWith(const Type *type, llvm::StringRef name) const {
  auto chain = [](const Type *t) {
    std::vector<const Type *> up{t};
    for (size_t i = 0; i < up.size(); ++i) {
      for (const Type *b : up[i]->bases()) {
        if (!llvm::is_contained(up, b)) {
          up.push_back(b);
        }
      }
    }

    return up;
  };
  llvm::ArrayRef<const Type *> members = type->members();
  if (llvm::any_of(members, [](const Type *m) { return !m->isObject(); })) {
    return nullptr;
  }
  for (const Type *candidate : chain(members.front())) {
    if (llvm::is_contained(members, candidate) && members.size() > 1) {
      continue;
    }
    bool all = llvm::all_of(members.drop_front(),
                            [&](const Type *m) { return m->isSubtypeOf(candidate); });
    if (!all) {
      continue;
    }
    if (const ClassDecl *cls = ClassDeclarer::classOf(candidate)) {
      if (ClassBases::findField(cls, name) ||
          !ClassDeclarer::methodsNamed(cls, name, false).empty()) {
        return candidate;
      }
      continue;
    }
    if (candidate->field(name) || candidate->overload(name)) {
      return candidate;
    }
  }

  return nullptr;
}

std::optional<std::vector<const Type *>>
UnionChecker::unhandledCases(const SwitchStmt *stmt) const {
  const Type *type = stmt->discriminant->type;
  if (!type || type->isError() || type->isOptional()) {
    return std::nullopt;
  }
  if (!UnionChecker::isLiteralSet(type)) {
    return std::nullopt;
  }
  std::vector<const Type *> left(UnionChecker::membersOf(type).begin(),
                                 UnionChecker::membersOf(type).end());
  for (const SwitchCase &c : stmt->cases) {
    if (!c.test) {
      return std::nullopt;
    }
    if (const Type *literal = UnionCheckHelpers::writtenLiteral(c.test)) {
      llvm::erase(left, literal);
    } else if (c.test->type && c.test->type->isLiteral()) {
      llvm::erase(left, c.test->type);
    }
  }

  return left;
}

UnionChecker::ResultUnionMethodTyped UnionChecker::checkUnionMethodCall(CallExpr *call,
                                                                        MemberExpr *member) {
  const Type *receiver = member->object->type;
  call->type = Type::error();
  member->type = Type::error();
  if (receiver->isTagged() || !UnionChecker::isAllObjects(receiver)) {
    this->argumentChecker->checkArgsLoosely(call);
    const char *typeOf = TypeSpelling::typeOfName(receiver->members().front());
    std::string message = ErrorTextUnionMember::narrow(member->name, receiver->name(), typeOf);

    return err(UnionCheckerCheckUnionMethodCallError{
        .code = UnionCheckerCheckUnionMethodCallErrorCode::NOT_NARROWED,
        .message = message,
    });
  }
  const Type *fnType = nullptr;
  std::vector<UnionMethod> methods;
  for (const Type *m : receiver->members()) {
    ResultUnionMethodPart part = this->memberMethod(member, m);
    if (part.isErr()) {
      this->argumentChecker->checkArgsLoosely(call);
      return err(part.error());
    }
    const Type *type = part.value().type;
    if (type == nullptr || !type->isFunction()) {
      this->argumentChecker->checkArgsLoosely(call);
      bool field = type != nullptr;
      std::string message =
          ErrorTextUnionMethod::notMethod(receiver->name(), m->name(), member->name, field);

      return err(UnionCheckerCheckUnionMethodCallError{
          .code = UnionCheckerCheckUnionMethodCallErrorCode::NOT_A_METHOD,
          .message = message,
      });
    }
    if (fnType != nullptr && type != fnType) {
      this->argumentChecker->checkArgsLoosely(call);
      std::string message = ErrorTextUnionMethod::differ(receiver->name(), member->name,
                                                         fnType->name(), type->name());

      return err(UnionCheckerCheckUnionMethodCallError{
          .code = UnionCheckerCheckUnionMethodCallErrorCode::METHOD_TYPES_DIFFER,
          .message = message,
      });
    }
    fnType = type;
    methods.push_back(part.value().entry);
  }
  call->target = UnionMethodCall{.unionMethods = std::move(methods)};
  member->type = fnType;
  this->argumentChecker->checkArguments(call, fnType->params(), fnType->minArgs(),
                                        fnType->hasRest(), member->loc());
  call->type = fnType->ret();

  return ok(call);
}

bool UnionChecker::isAllObjects(const Type *type) {
  for (const Type *m : type->members()) {
    if (!m->isObject()) {
      return false;
    }
  }

  return true;
}

UnionChecker::ResultUnionMethodPart UnionChecker::memberMethod(const MemberExpr *member,
                                                               const Type *m) {
  ClassDecl *cls = ClassDeclarer::classOf(m);
  if (cls != nullptr) {
    return this->classMemberMethod(member, m, cls);
  }
  const Field *f = m->field(member->name);
  if (f != nullptr && m->isBase()) {
    const Type *receiver = member->object->type;
    std::string message = ErrorTextUnionMethod::isBase(member->name, receiver->name(), m->name());

    return err(UnionCheckerCheckUnionMethodCallError{
        .code = UnionCheckerCheckUnionMethodCallErrorCode::INTERFACE_MEMBER,
        .message = message,
    });
  }

  return ok(UnionMethodPart{
      .type = f != nullptr ? f->type : nullptr,
      .entry = UnionMethod{m, nullptr, false},
  });
}

UnionChecker::ResultUnionMethodPart UnionChecker::classMemberMethod(const MemberExpr *member,
                                                                    const Type *m, ClassDecl *cls) {
  llvm::StringRef name = member->name;
  this->classDeclarer->completeClass(cls);
  const ClassDecl *owner = nullptr;
  const ClassField *field = ClassBases::findField(cls, name, &owner);
  if (field != nullptr && !field->method()) {
    ClassMemberChecker::ResultMemberAccessible accessible =
        this->classMemberChecker->checkAccess(owner, field->access, name);
    if (accessible.isErr()) {
      return err(UnionCheckerCheckUnionMethodCallError{
          .code = UnionCheckerCheckUnionMethodCallErrorCode::NOT_ACCESSIBLE,
          .message = accessible.error().message,
      });
    }
    const Field *f = m->field(name);
    return ok(UnionMethodPart{
        .type = f != nullptr ? f->type : nullptr,
        .entry = UnionMethod{m, nullptr, false},
    });
  }
  std::vector<FunctionDecl *> variants = ClassDeclarer::methodsNamed(cls, name, false);
  if (variants.size() > 1) {
    const Type *receiver = member->object->type;
    std::string message = ErrorTextUnionMethod::variants(name, m->name(), receiver->name());

    return err(UnionCheckerCheckUnionMethodCallError{
        .code = UnionCheckerCheckUnionMethodCallErrorCode::SEVERAL_VARIANTS,
        .message = message,
    });
  }
  if (variants.empty()) {
    return ok(UnionMethodPart{
        .type = nullptr,
        .entry = UnionMethod{m, nullptr, false},
    });
  }

  return this->classVariantMethod(member, m, cls, variants[0]);
}

UnionChecker::ResultUnionMethodPart UnionChecker::classVariantMethod(const MemberExpr *member,
                                                                     const Type *m, ClassDecl *cls,
                                                                     FunctionDecl *variant) {
  ClassMemberChecker::ResultMemberAccessible accessible =
      this->classMemberChecker->checkAccess(variant->cls(), variant->access(), member->name);
  if (accessible.isErr()) {
    return err(UnionCheckerCheckUnionMethodCallError{
        .code = UnionCheckerCheckUnionMethodCallErrorCode::NOT_ACCESSIBLE,
        .message = accessible.error().message,
    });
  }
  FuncSymbol *sym = variant->sym;
  UnionMethod entry{m, nullptr, false};
  FunctionDecl *impl = ClassLinks::soleImplementation(cls, sym->decl);
  if (impl != nullptr) {
    entry.method = impl->sym;
  } else {
    entry.method = sym;
    entry.virtualCall = true;
  }

  return ok(UnionMethodPart{
      .type = this->returnInferrer->functionType(sym),
      .entry = entry,
  });
}

}
