#include "compiler/pipeline/check_program/types/union_checker/union_checker.h"
#include "compiler/pipeline/check_program/declare/class_declarer/class_declarer.h"
#include "compiler/pipeline/check_program/flow/field_facts/field_facts.h"
#include "compiler/pipeline/check_program/infer/return_inferrer/return_inferrer.h"
#include "compiler/pipeline/check_program/topics/bytes/bytes_checker/bytes_checker.h"
#include "compiler/pipeline/check_program/topics/classes/class_bases/class_bases.h"
#include "compiler/pipeline/check_program/topics/classes/class_links/class_links.h"
#include "compiler/pipeline/check_program/topics/classes/class_member_checker/class_member_checker.h"
#include "compiler/pipeline/check_program/topics/expr/argument_checker/argument_checker.h"
#include "compiler/pipeline/check_program/topics/expr/expr_checker/expr_checker.h"
#include "compiler/pipeline/check_program/topics/objects/object_checker/object_checker.h"
#include "compiler/pipeline/check_program/types/union_check_helpers/union_check_helpers.h"
#include "compiler/pipeline/check_program/whole_program/flows/flows.h"

namespace tz {

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

UnionChecker::UnionChecker(CheckState &owner)
    : state(owner) {}

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
    const char *typeOf = typeOfName(type->members().front());
    std::string message = ErrorTextUnionMember::narrow(member->name, type->name(), typeOf);

    return err(UnionCheckerCheckUnionMemberError{
        .code = UnionCheckerCheckUnionMemberErrorCode::TAGGED_UNION,
        .message = message,
    });
  }
  if (llvm::all_of(type->members(), [](const Type *m) { return m->isBytes(); })) {
    Expr *bytes = BytesChecker(this->state).checkBytesUnionMember(member);

    return ok(bytes);
  }
  std::vector<const Type *> fieldTypes;
  const Type *lacking = nullptr;
  for (const Type *m : type->members()) {
    if (ClassDecl *cls = ClassDeclarer::classOf(m)) {
      ClassDeclarer(this->state).completeClass(cls);
    }
    const Field *f = m->isObject() ? m->field(member->name) : nullptr;
    if (const ClassDecl *cls = ClassDeclarer::classOf(m)) {
      const ClassDecl *owner = nullptr;
      const ClassField *field = ClassBases::findField(cls, member->name, &owner);
      ClassMemberChecker::ResultMemberAccessible accessible =
          field ? ClassMemberChecker(this->state).checkAccess(owner, field->access, member->name)
                : ClassMemberChecker::ResultMemberAccessible(ok(cls));
      if (accessible.isErr()) {
        return err(UnionCheckerCheckUnionMemberError{
            .code = UnionCheckerCheckUnionMemberErrorCode::NOT_ACCESSIBLE,
            .message = accessible.error().message,
        });
      }
      if (!field) {
        f = nullptr;
      }
    }
    if (!f) {
      lacking = m;
      break;
    }
    if (!llvm::is_contained(fieldTypes, f->type)) {
      fieldTypes.push_back(f->type);
    }
  }
  bool nowhere = llvm::none_of(type->members(), [&](const Type *m) {
    return m->isObject() && !m->isClass() && m->field(member->name);
  });
  bool plainObjects =
      llvm::all_of(type->members(), [](const Type *m) { return m->isObject() && !m->isClass(); });
  if (lacking && nowhere && plainObjects) {
    std::string message = ErrorTextUnionMember::noField(type->name(), member->name);

    return err(UnionCheckerCheckUnionMemberError{
        .code = UnionCheckerCheckUnionMemberErrorCode::NO_SUCH_FIELD,
        .message = message,
    });
  }
  if (lacking) {
    std::string hint = UnionChecker::lackingHint(member, type);
    std::string message =
        ErrorTextUnionMember::mayLack(member->name, lacking->name(), type->name(), hint);

    return err(UnionCheckerCheckUnionMemberError{
        .code = UnionCheckerCheckUnionMemberErrorCode::MAY_LACK_FIELD,
        .message = message,
    });
  }
  const Type *result = Type::unionOf(fieldTypes);
  std::string problem = unionProblem(result);
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

UnionChecker::ResultUnionFieldTarget UnionChecker::checkUnionFieldTarget(MemberExpr *member) {
  const Type *type = member->object->type;
  const Type *field = nullptr;
  for (const Type *m : type->members()) {
    if (ClassDecl *incomplete = ClassDeclarer::classOf(m)) {
      ClassDeclarer(this->state).completeClass(incomplete);
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
        declared
            ? ClassMemberChecker(this->state).checkAccess(owner, declared->access, member->name)
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
  FieldFacts(this->state).fieldsAssigned(member->name);
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
    if (const Type *literal = writtenLiteral(c.test)) {
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
  llvm::StringRef name = member->name;
  call->type = Type::error();
  member->type = Type::error();
  bool notNarrowed = receiver->isTagged() || llvm::any_of(receiver->members(), [](const Type *m) {
                       return !m->isObject();
                     });
  if (notNarrowed) {
    ArgumentChecker(this->state).checkArgsLoosely(call);
    const char *typeOf = typeOfName(receiver->members().front());
    std::string message = ErrorTextUnionMember::narrow(name, receiver->name(), typeOf);

    return err(UnionCheckerCheckUnionMethodCallError{
        .code = UnionCheckerCheckUnionMethodCallErrorCode::NOT_NARROWED,
        .message = message,
    });
  }
  const Type *fnType = nullptr;
  std::vector<UnionMethod> methods;
  for (const Type *m : receiver->members()) {
    const Type *type = nullptr;
    UnionMethod entry{m, nullptr, false};
    if (ClassDecl *cls = ClassDeclarer::classOf(m)) {
      ClassDeclarer(this->state).completeClass(cls);
      const ClassDecl *owner = nullptr;
      const ClassField *field = ClassBases::findField(cls, name, &owner);
      if (field && !field->method()) {
        ClassMemberChecker::ResultMemberAccessible accessible =
            ClassMemberChecker(this->state).checkAccess(owner, field->access, name);
        if (accessible.isErr()) {
          ArgumentChecker(this->state).checkArgsLoosely(call);
          return err(UnionCheckerCheckUnionMethodCallError{
              .code = UnionCheckerCheckUnionMethodCallErrorCode::NOT_ACCESSIBLE,
              .message = accessible.error().message,
          });
        }
        type = m->field(name) ? m->field(name)->type : nullptr;
      } else if (std::vector<FunctionDecl *> variants =
                     ClassDeclarer::methodsNamed(cls, name, false);
                 variants.size() > 1) {
        ArgumentChecker(this->state).checkArgsLoosely(call);
        std::string message = ErrorTextUnionMethod::variants(name, m->name(), receiver->name());

        return err(UnionCheckerCheckUnionMethodCallError{
            .code = UnionCheckerCheckUnionMethodCallErrorCode::SEVERAL_VARIANTS,
            .message = message,
        });
      } else if (!variants.empty()) {
        ClassMemberChecker::ResultMemberAccessible accessible =
            ClassMemberChecker(this->state)
                .checkAccess(variants[0]->cls(), variants[0]->access(), name);
        if (accessible.isErr()) {
          ArgumentChecker(this->state).checkArgsLoosely(call);
          return err(UnionCheckerCheckUnionMethodCallError{
              .code = UnionCheckerCheckUnionMethodCallErrorCode::NOT_ACCESSIBLE,
              .message = accessible.error().message,
          });
        }
        FuncSymbol *sym = variants[0]->sym;
        ReturnInferrer(this->state).resolveParams(sym);
        if (FunctionDecl *impl = ClassLinks::soleImplementation(cls, sym->decl)) {
          entry.method = impl->sym;
        } else {
          entry.method = sym, entry.virtualCall = true;
        }
        type = ReturnInferrer(this->state).functionType(sym);
      }
    } else if (const Field *f = m->field(name); f && m->isBase()) {
      ArgumentChecker(this->state).checkArgsLoosely(call);
      std::string message = ErrorTextUnionMethod::isBase(name, receiver->name(), m->name());

      return err(UnionCheckerCheckUnionMethodCallError{
          .code = UnionCheckerCheckUnionMethodCallErrorCode::INTERFACE_MEMBER,
          .message = message,
      });
    } else if (f) {
      type = f->type;
    }
    if (!type || !type->isFunction()) {
      ArgumentChecker(this->state).checkArgsLoosely(call);
      bool field = type != nullptr;
      std::string message =
          ErrorTextUnionMethod::notMethod(receiver->name(), m->name(), name, field);

      return err(UnionCheckerCheckUnionMethodCallError{
          .code = UnionCheckerCheckUnionMethodCallErrorCode::NOT_A_METHOD,
          .message = message,
      });
    }
    if (fnType && type != fnType) {
      ArgumentChecker(this->state).checkArgsLoosely(call);
      std::string message =
          ErrorTextUnionMethod::differ(receiver->name(), name, fnType->name(), type->name());

      return err(UnionCheckerCheckUnionMethodCallError{
          .code = UnionCheckerCheckUnionMethodCallErrorCode::METHOD_TYPES_DIFFER,
          .message = message,
      });
    }
    fnType = type;
    methods.push_back(entry);
  }
  call->target = UnionMethodCall{.unionMethods = std::move(methods)};
  member->type = fnType;
  ArgumentChecker(this->state)
      .checkArguments(call, fnType->params(), fnType->minArgs(), fnType->hasRest(), member->loc());
  call->type = fnType->ret();

  return ok(call);
}

}
