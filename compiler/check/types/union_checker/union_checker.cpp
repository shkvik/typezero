#include "compiler/check/types/union_checker/union_checker.h"
#include "compiler/check/classes/class_bases/class_bases.h"
#include "compiler/check/classes/class_links/class_links.h"
#include "compiler/check/classes/class_member_checker/class_member_checker.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/flow/field_facts/field_facts.h"
#include "compiler/check/types/union_check_helpers/union_check_helpers.h"
#include "compiler/declare/class_declarer/class_declarer.h"
#include "compiler/infer/return_inferrer/return_inferrer.h"
#include "compiler/library/bytes/bytes_checker/bytes_checker.h"
#include "compiler/whole_program/flows/flows.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

llvm::ArrayRef<const Type *> membersOf(const Type *&type) {
  return type->isUnion() ? type->members() : llvm::ArrayRef<const Type *>(type);
}

bool isLiteralSet(const Type *type) {
  if (type->isLiteral()) {
    return true;
  }

  return type->isUnion() &&
         llvm::all_of(type->members(), [](const Type *m) { return m->isLiteral(); });
}

}

bool hasParamMember(const Type *type) {
  return type->isUnion() &&
         llvm::any_of(type->members(), [](const Type *m) { return m->isParam(); });
}

UnionChecker::UnionChecker(Checker &checker) : checker_(checker) {}

Expr *UnionChecker::checkUnionMember(MemberExpr *member) {
  const Type *type = member->object->type;
  if (type->isTagged()) {
    checker_.diags_.error(member->loc(), "property '" + member->name + "' does not exist on '" +
                                             type->name() +
                                             "': check what it holds first, e.g. 'typeof x == \"" +
                                             typeOfName(type->members().front()) + "\"'");
    return member;
  }
  if (llvm::all_of(type->members(), [](const Type *m) { return m->isBytes(); })) {
    return checker_.bytesChecker().checkBytesUnionMember(member);
  }
  std::vector<const Type *> fieldTypes;
  const Type *lacking = nullptr;
  for (const Type *m : type->members()) {
    if (ClassDecl *cls = ClassDeclarer::classOf(m)) {
      checker_.classDeclarer().completeClass(cls);
    }
    const Field *f = m->isObject() ? m->field(member->name) : nullptr;
    if (const ClassDecl *cls = ClassDeclarer::classOf(m)) {
      const ClassDecl *owner = nullptr;
      const ClassField *field = ClassBases::findField(cls, member->name, &owner);
      if (field && field->access != Access::Public &&
          !checker_.classMemberChecker().checkAccess(owner, field->access, member->name,
                                                     member->loc())) {
        return member;
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
  if (lacking && nowhere &&
      llvm::all_of(type->members(), [](const Type *m) { return m->isObject() && !m->isClass(); })) {
    std::string message = "'" + type->name().str() + "' has no field '" + member->name.str() + "'";
    if (member->name == "errno") {
      message += ": tz gives no errno; the reason is in 'kind', the system's code in 'code'";
    }
    checker_.diags_.error(member->loc(), message);
    member->type = Type::error();
    return member;
  }
  if (lacking) {
    std::string message = "property '" + member->name.str() + "' does not exist on '" +
                          lacking->name().str() + "', which '" + type->name().str() +
                          "' may be; narrow it first";
    bool hinted = false;
    std::string fallback;
    auto literals = [](const Type *t) {
      return t->isLiteral() || (t->isUnion() && llvm::all_of(t->members(), [](const Type *m) {
                                  return m->isLiteral();
                                }));
    };
    auto holds = [](const Type *set, const Type *literal) {
      return set == literal || (set->isUnion() && llvm::is_contained(set->members(), literal));
    };
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
        std::string hint =
            ", e.g. 'if (" + fieldText(member->object, f.name) + " == " + tag->name().str() + ")'";
        bool unique = llvm::none_of(type->members(), [&](const Type *other) {
          return !other->field(member->name) && holds(other->field(f.name)->type, tag);
        });
        if (unique) {
          message += hint;
          hinted = true;
        } else if (fallback.empty()) {
          fallback = hint;
        }
        break;
      }
      if (hinted) {
        break;
      }
    }
    if (!hinted && !fallback.empty()) {
      message += fallback;
      hinted = true;
    }
    if (!hinted) {
      const Type *with = nullptr;
      for (const Type *m : type->members()) {
        if (m->isObject() && m->field(member->name)) {
          with = m;
        }
      }
      if (with && with->isClass()) {
        message += ", e.g. 'if (x instanceof " + with->name().str() + ")'";
      } else if (with) {
        message += ", e.g. 'if (\"" + member->name.str() + "\" in x)'";
      }
    }
    checker_.diags_.error(member->loc(), message);
    return member;
  }
  const Type *result = Type::unionOf(fieldTypes);
  if (std::string problem = unionProblem(result); !problem.empty()) {
    checker_.diags_.error(member->loc(), "the members of '" + type->name() + "' have fields '" +
                                             member->name +
                                             "' of types that one value cannot hold: " + problem +
                                             "; narrow it first");
    return member;
  }
  member->type = result;

  return member;
}

const Type *UnionChecker::checkUnionFieldTarget(MemberExpr *member) {
  const Type *type = member->object->type;
  const Type *field = nullptr;
  for (const Type *m : type->members()) {
    if (ClassDecl *incomplete = ClassDeclarer::classOf(m)) {
      checker_.classDeclarer().completeClass(incomplete);
    }
    const Field *f = m->isObject() ? m->field(member->name) : nullptr;
    const ClassDecl *cls = ClassDeclarer::classOf(m);
    const ClassDecl *owner = nullptr;
    const ClassField *declared = cls ? ClassBases::findField(cls, member->name, &owner) : nullptr;
    if (!f || (cls && (!declared || declared->method))) {
      checker_.diags_.error(member->loc(), "property '" + member->name + "' does not exist on '" +
                                               m->name() + "', which '" + type->name() +
                                               "' may be; narrow it first");
      return Type::error();
    }
    if (declared && declared->access != Access::Public &&
        !checker_.classMemberChecker().checkAccess(owner, declared->access, member->name,
                                                   member->loc())) {
      return Type::error();
    }
    if (f->readonly) {
      checker_.diags_.error(member->loc(), "cannot assign to '" + member->name +
                                               "' because it is a read-only field of '" +
                                               m->name() + "'");
      return Type::error();
    }
    if (field && f->type != field) {
      checker_.diags_.error(member->loc(), "the members of '" + type->name() + "' have fields '" +
                                               member->name + "' of different types, '" +
                                               field->name() + "' and '" + f->type->name() +
                                               "', so no value fits them all; narrow it first");
      return Type::error();
    }
    field = f->type;
  }
  checker_.fieldFacts().fieldsAssigned(member->name);
  member->type = field;

  return field;
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
  if (!isLiteralSet(type)) {
    return std::nullopt;
  }
  std::vector<const Type *> left(membersOf(type).begin(), membersOf(type).end());
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

Expr *UnionChecker::checkUnionMethodCall(CallExpr *call, MemberExpr *member) {
  const Type *receiver = member->object->type;
  llvm::StringRef name = member->name;
  call->type = Type::error();
  member->type = Type::error();
  auto loosely = [&] {
    for (Expr *&arg : call->args) {
      arg = checker_.exprChecker().checkExpr(arg);
    }

    return call;
  };
  if (receiver->isTagged() ||
      llvm::any_of(receiver->members(), [](const Type *m) { return !m->isObject(); })) {
    checker_.diags_.error(member->loc(), "property '" + name + "' does not exist on '" +
                                             receiver->name() +
                                             "': check what it holds first, e.g. 'typeof x == \"" +
                                             typeOfName(receiver->members().front()) + "\"'");
    return loosely();
  }
  const Type *fnType = nullptr;
  std::vector<CallExpr::UnionMethod> methods;
  for (const Type *m : receiver->members()) {
    const Type *type = nullptr;
    CallExpr::UnionMethod entry{m, nullptr, false};
    if (ClassDecl *cls = ClassDeclarer::classOf(m)) {
      checker_.classDeclarer().completeClass(cls);
      const ClassDecl *owner = nullptr;
      const ClassField *field = ClassBases::findField(cls, name, &owner);
      if (field && !field->method) {
        if (field->access != Access::Public &&
            !checker_.classMemberChecker().checkAccess(owner, field->access, name, member->loc())) {
          return loosely();
        }
        type = m->field(name) ? m->field(name)->type : nullptr;
      } else if (std::vector<FunctionDecl *> variants =
                     ClassDeclarer::methodsNamed(cls, name, false);
                 variants.size() > 1) {
        checker_.diags_.error(member->loc(), "'" + name + "' of '" + m->name() +
                                                 "' has several variants, which a call on '" +
                                                 receiver->name() +
                                                 "' cannot choose from; narrow it first");
        return loosely();
      } else if (!variants.empty()) {
        if (!checker_.classMemberChecker().checkAccess(variants[0]->cls, variants[0]->access, name,
                                                       member->loc())) {
          return loosely();
        }
        FuncSymbol *sym = variants[0]->sym;
        checker_.returnInferrer().resolveParams(sym);
        if (FunctionDecl *impl = ClassLinks::soleImplementation(cls, sym->decl)) {
          entry.method = impl->sym;
        } else {
          entry.method = sym, entry.virtualCall = true;
        }
        type = checker_.returnInferrer().functionType(sym);
      }
    } else if (const Field *f = m->field(name); f && m->isBase()) {
      checker_.diags_.error(member->loc(),
                            "'" + name + "' is called on '" + receiver->name() + "', and '" +
                                m->name() +
                                "' is an interface that classes implement; narrow it "
                                "first, or declare the members to be one type with '" +
                                name + "'");
      return loosely();
    } else if (f) {
      type = f->type;
    }
    if (!type || !type->isFunction()) {
      checker_.diags_.error(member->loc(),
                            "'" + receiver->name() + "' may be a '" + m->name() + "', " +
                                (type ? "whose '" + name.str() + "' is not a function"
                                      : "which has no method '" + name.str() + "'") +
                                "; narrow it first");
      return loosely();
    }
    if (fnType && type != fnType) {
      checker_.diags_.error(member->loc(), "the members of '" + receiver->name() +
                                               "' have methods '" + name +
                                               "' of different types, '" + fnType->name() +
                                               "' and '" + type->name() + "'; narrow it first");
      return loosely();
    }
    fnType = type;
    methods.push_back(entry);
  }
  call->unionMethods = std::move(methods);
  member->type = fnType;
  checker_.argumentChecker().checkArguments(call, fnType->params(), fnType->minArgs(),
                                            fnType->hasRest(), member->loc());
  call->type = fnType->ret();

  return call;
}

}
