#include "compiler/library/crypto/subtle_call_checker/subtle_call_checker.h"
#include "compiler/check/classes/method_call_checker/method_call_checker.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/scopes/scopes.h"
#include "compiler/declare/class_declarer/class_declarer.h"
#include "compiler/infer/return_inferrer/return_inferrer.h"
#include "compiler/library/crypto/crypto_api/crypto_api.h"
#include "compiler/library/crypto/subtle_algorithms/subtle_algorithms.h"
#include "compiler/library/events/event_target_checker/event_target_checker.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include <functional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

std::string utf8(const std::u16string &text) {
  std::string out;
  llvm::convertUTF16ToUTF8String(
      llvm::ArrayRef<llvm::UTF16>(reinterpret_cast<const llvm::UTF16 *>(text.data()), text.size()),
      out);

  return out;
}

std::optional<double> numberLiteral(const Expr *e) {
  if (auto *lit = dyn_cast_or_null<IntLitExpr>(e)) {
    return double(lit->value);
  }
  if (auto *lit = dyn_cast_or_null<FloatLitExpr>(e)) {
    return lit->value;
  }

  return std::nullopt;
}

const ObjectLitExpr::Property *property(const ObjectLitExpr *lit, llvm::StringRef name) {
  for (const ObjectLitExpr::Property &prop : lit->props) {
    if (!prop.spread && !prop.key && prop.name == name) {
      return &prop;
    }
  }

  return nullptr;
}

}

SubtleCallChecker::SubtleCallChecker(Checker &checker) : checker_(checker) {}

Expr *SubtleCallChecker::checkSubtleCall(CallExpr *call, MemberExpr *member, const Type *expected) {
  llvm::StringRef method = member->name;
  std::vector<Expr *> &args = call->args;
  if (call->hasSpread()) {
    return nullptr;
  }
  auto arg = [&](size_t i) -> Expr * { return i < args.size() ? args[i] : nullptr; };
  auto algorithm = [&](size_t i) -> const Expr * {
    Expr *e = arg(i);
    if (auto *name = dyn_cast_or_null<NameExpr>(e)) {
      if (VarSymbol *var = checker_.scopes().lookup(name->name).var) {
        if (const Expr *lit = checker_.constLiterals_.lookup(var)) {
          return lit;
        }
      }
    }

    return e;
  };
  bool bad = false;

  auto format = [&](size_t i) -> std::string {
    std::optional<std::string> f = stringLiteral(arg(i));
    if (!f) {
      return "";
    }
    static const llvm::StringLiteral formats[] = {"raw", "pkcs8", "spki", "jwk"};
    if (!llvm::is_contained(formats, *f)) {
      bad = true;
      checker_.diags_.error(arg(i)->loc(), "unknown key format '" + *f +
                                               "'; it is 'raw', 'pkcs8', 'spki' or 'jwk'");
      return "";
    }

    return *f;
  };

  auto usages = [&](size_t i, llvm::StringRef name, int isPublic, bool creates) {
    auto *lit = dyn_cast_or_null<ArrayLitExpr>(arg(i));
    if (!lit) {
      return;
    }
    std::vector<llvm::StringLiteral> allowed;
    if (!name.empty()) {
      if (isPublic != 0) {
        allowed = usagesOf(name, true);
      }
      if (isPublic != 1) {
        for (llvm::StringLiteral u : usagesOf(name, false)) {
          if (!llvm::is_contained(allowed, u)) {
            allowed.push_back(u);
          }
        }
      }
    }
    for (const ArrayLitExpr::Element &e : lit->elements) {
      std::optional<std::string> u = e.spread ? std::nullopt : stringLiteral(e.expr);
      if (!u) {
        return;
      }
      if (!llvm::is_contained(kUsages, *u)) {
        bad = true;
        checker_.diags_.error(e.expr->loc(), "usage '" + *u + "' does not exist; the usages are " +
                                                 quotedList(kUsages, "and"));
        return;
      }
      if (!name.empty() && !llvm::is_contained(allowed, *u)) {
        bad = true;
        std::string an = llvm::StringRef("EHRAX").contains(name.front()) ? "an " : "a ";
        std::string what =
            isPublic == 1 ? "a public " + name.str() + " key"
            : isPublic == 0 && name != "HMAC" && familyOf(name) != "Aes" && familyOf(name) != "Kdf"
                ? "a private " + name.str() + " key"
                : an + name.str() + " key";
        checker_.diags_.error(e.expr->loc(),
                              "'" + *u + "' is no usage of " + what + "; " +
                                  (allowed.empty() ? std::string("it takes none: pass []")
                                                   : "it takes " + quotedList(allowed, "and")));
        return;
      }
    }
    if (!creates || name.empty() || isPublic == 1) {
      return;
    }
    std::vector<llvm::StringLiteral> own = usagesOf(name, false);
    bool any = false;
    for (const ArrayLitExpr::Element &e : lit->elements) {
      any |= llvm::is_contained(own, *stringLiteral(e.expr));
    }
    if (any) {
      return;
    }
    bad = true;
    if (lit->elements.empty() || isPublic == 0) {
      checker_.diags_.error(lit->loc(), "a key without usages can do nothing, and node rejects it "
                                        "('Usages cannot be empty'); give its usages, e.g. " +
                                            quotedList(own, "and"));
    } else {
      checker_.diags_.error(lit->loc(),
                            "the private key of the pair would have no usages, which node "
                            "rejects ('Usages cannot be empty'); add " +
                                quotedList(own, "or"));
    }
  };

  std::string keyFormat;
  auto keyDataFits = [&]() {
    Expr *data = arg(1);
    if (!data || keyFormat.empty() || !data->type || data->type->isError()) {
      return true;
    }
    while (auto *conv = dyn_cast<CastExpr>(data)) {
      if (!conv->isImplicit()) {
        break;
      }
      data = conv->operand;
    }
    const Type *type = data->type;
    if (!type || type->isError()) {
      return true;
    }
    bool bytes = type->isBytes() ||
                 (type->isUnion() &&
                  llvm::all_of(type->members(), [](const Type *t) { return t->isBytes(); }));
    bool jwk = !type->isBytes() && type->isObject() && !ClassDeclarer::classOf(type) &&
               !type->isArray() && !type->isTuple();
    if (keyFormat == "jwk" && bytes) {
      checker_.diags_.error(data->loc(), "the key data of 'jwk' is a JsonWebKey; found '" +
                                             type->name().str() + "'");
      return false;
    }
    if (keyFormat != "jwk" && jwk) {
      checker_.diags_.error(data->loc(), "the key data of '" + keyFormat +
                                             "' is bytes (a BufferSource); found '" +
                                             type->name().str() + "'");
      return false;
    }

    return true;
  };

  std::string target;
  std::vector<bool> checkedArgs(args.size(), false);
  std::function<bool(const Expr *)> isJsonValue = [&](const Expr *e) -> bool {
    if (auto *name = dyn_cast_or_null<NameExpr>(e)) {
      VarSymbol *var = checker_.scopes().lookup(name->name).var;
      return var && var->type && var->type->nonOptional()->isJson();
    }
    if (auto *m = dyn_cast_or_null<MemberExpr>(e)) {
      return isJsonValue(m->object);
    }
    if (auto *index = dyn_cast_or_null<IndexExpr>(e)) {
      return isJsonValue(index->object);
    }
    if (auto *c = dyn_cast_or_null<CallExpr>(e)) {
      if (auto *m = dyn_cast<MemberExpr>(c->callee)) {
        return m->name == "parse" && m->typeArgs.empty() &&
               checker_.callChecker().isGlobalName(m->object, "JSON");
      }
    }

    return false;
  };
  if (method == "digest") {
    literalAlgorithm(checker_.diags_, algorithm(0), "digest", "", bad);
  } else if (method == "generateKey") {
    std::string name = literalAlgorithm(checker_.diags_, algorithm(0), "generateKey", "", bad);
    llvm::StringRef family = familyOf(name);
    usages(2, name, family == "Hmac" || family == "Aes" ? 0 : -1, true);
    if (!name.empty()) {
      target = "subtleGenerate" + familyOf(name).str();
    }
  } else if (method == "importKey") {
    std::string f = format(0);
    std::string name = literalAlgorithm(checker_.diags_, algorithm(2), "importKey", "", bad);
    if (!f.empty() && !name.empty()) {
      std::vector<llvm::StringLiteral> formats = formatsOf(familyOf(name));
      if (!llvm::is_contained(formats, f)) {
        bad = true;
        checker_.diags_.error(arg(0)->loc(), "'" + name + "' keys are not imported as '" + f +
                                                 "' (node: 'Unable to import " + name + " using " +
                                                 f + " format'); it takes " +
                                                 quotedList(formats, "or"));
      }
    }
    int isPublic = f == "spki" || f == "raw" ? 1 : f == "pkcs8" ? 0 : -1;
    if (familyOf(name) == "Hmac" || familyOf(name) == "Aes" || familyOf(name) == "Kdf") {
      isPublic = 0;
    }
    usages(4, name, isPublic, isPublic == 0);
    keyFormat = f;
    if (!name.empty()) {
      target = "subtleImport" + familyOf(name).str();
    }
    if ((f.empty() || f == "jwk") && isJsonValue(arg(1)) && isa<NameExpr>(member->object) &&
        checker_.libFunctions_.lookup("subtleJwkOf")) {
      Expr *json = checker_.exprChecker().checkExpr(arg(1));
      auto *callee = checker_.ctx_.make<NameExpr>(json->loc(), "subtleJwkOf");
      auto *wrap = checker_.ctx_.make<CallExpr>(json->loc(), callee, std::vector<Expr *>{json});
      args[1] = checker_.eventTargetChecker().callLibraryFunction(wrap, "subtleJwkOf", {}, {true},
                                                                  json->loc());
      checkedArgs[1] = true;
      if (target.empty()) {
        target = "subtleImportAny";
      }
    }
  } else if (method == "exportKey") {
    std::string f = format(0);
    if (!f.empty()) {
      target = f == "jwk" ? "subtleExportJwk" : "subtleExportBytes";
    }
  } else if (method == "sign" || method == "verify" || method == "encrypt" || method == "decrypt" ||
             method == "deriveBits") {
    literalAlgorithm(checker_.diags_, algorithm(0), method, "", bad);
  } else if (method == "deriveKey") {
    literalAlgorithm(checker_.diags_, algorithm(0), "deriveBits", "", bad);
    std::string name = literalAlgorithm(checker_.diags_, algorithm(2), "importKey", "", bad);
    if (!name.empty()) {
      llvm::StringRef family = familyOf(name);
      if (family != "Hmac" && family != "Aes" && family != "Kdf") {
        bad = true;
        checker_.diags_.error(arg(2)->loc(),
                              "deriveKey makes secret keys: AES, HMAC, HKDF or PBKDF2, "
                              "not '" +
                                  name + "'");
      } else {
        if (family != "Kdf") {
          literalAlgorithm(checker_.diags_, algorithm(2), "get key length", "", bad);
        }
        usages(4, name, 0, true);
      }
    }
  } else if (method == "wrapKey") {
    format(0);
    literalAlgorithm(checker_.diags_, algorithm(3), "wrapKey", "encrypt", bad);
  } else if (method == "unwrapKey") {
    std::string f = format(0);
    literalAlgorithm(checker_.diags_, algorithm(3), "unwrapKey", "decrypt", bad);
    std::string name = literalAlgorithm(checker_.diags_, algorithm(4), "importKey", "", bad);
    if (!f.empty() && !name.empty()) {
      std::vector<llvm::StringLiteral> formats = formatsOf(familyOf(name));
      if (!llvm::is_contained(formats, f)) {
        bad = true;
        checker_.diags_.error(arg(0)->loc(), "'" + name + "' keys are not imported as '" + f +
                                                 "'; it takes " + quotedList(formats, "or"));
      }
    }
    int isPublic = f == "spki" || f == "raw" ? 1 : f == "pkcs8" ? 0 : -1;
    if (familyOf(name) == "Hmac" || familyOf(name) == "Aes" || familyOf(name) == "Kdf") {
      isPublic = 0;
    }
    usages(6, name, isPublic, isPublic == 0);
    if (!name.empty()) {
      target = "subtleUnwrap" + familyOf(name).str();
    }
  } else {
    return nullptr;
  }

  if (bad) {
    call->type = Type::error();
    std::vector<FunctionDecl *> methods =
        ClassDeclarer::methodsNamed(checker_.libraryClass("SubtleCrypto"), method, false);
    FuncSymbol *sym = methods.size() == 1 ? methods[0]->sym : nullptr;
    if (sym) {
      checker_.returnInferrer().resolveParams(sym);
    }
    for (size_t i = 0; i < args.size(); ++i) {
      if (!checkedArgs[i]) {
        args[i] = sym && i < sym->params.size()
                      ? checker_.argumentChecker().checkArg(args[i], sym->params[i])
                      : checker_.exprChecker().checkExpr(args[i]);
      }
    }
    return call;
  }
  Expr *checked;
  if (!target.empty() && checker_.libFunctions_.lookup(target) && isa<NameExpr>(member->object)) {
    checked = checker_.eventTargetChecker().callLibraryFunction(call, target, {}, checkedArgs,
                                                                member->loc());
  } else {
    checked = checker_.methodCallChecker().checkMethodCall(call, member, expected);
  }
  if (!keyDataFits()) {
    checked->type = Type::error();
  }

  return checked;
}

}
