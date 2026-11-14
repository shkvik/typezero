#include "compiler/semantic/code/classes/overload_choice/variant_filter/variant_filter.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
#include "compiler/semantic/generics/generic_check_helpers/generic_check_helpers.h"
#include "llvm/ADT/STLExtras.h"

using llvm::isa;

namespace tz {

VariantFilter::VariantFilter(SemanticState &owner, ChoiceRules &sharedChoiceRules)
    : state(owner),
      choiceRules(sharedChoiceRules) {}

void VariantFilter::chooseVariant(const ChooseVariant &task) {
  llvm::ArrayRef<FunctionDecl *> variants = task.variants;
  std::vector<Expr *> &args = *task.args;
  GenericVariants generics = task.generics;
  auto &overloads = this->state.code.overloads;
  if (variants.size() == 1) {
    return overloads.pushChoice(this->uncheckedChoice(variants[0]));
  }
  auto what = this->variantsName(variants[0]);
  auto plain = this->checkNoSpread(what, task.spread);
  if (plain.isErr()) {
    this->walkLoosely(args);
    return overloads.pushChoice(this->choiceRules.failedChoice(plain.error()));
  }
  auto fit = this->checkFittingVariants(what, variants, args.size());
  if (fit.isErr()) {
    this->walkLoosely(args);
    return overloads.pushChoice(this->choiceRules.failedChoice(fit.error()));
  }
  if (fit.value().size() == 1) {
    return overloads.pushChoice(this->uncheckedChoice(fit.value()[0]));
  }
  auto draft = ChoiceDraft{
      .fit = fit.value(),
      .args = &args,
      .what = what,
      .generics = generics,
      .ranking = VariantRanking(),
      .index = 0,
  };
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(RankVariants{.draft = draft});
  for (size_t index = args.size(); index > 0; --index) {
    const Type *expected = this->expectedArgument(draft.fit, args[index - 1], index - 1);
    bool isUsable = expected != nullptr && !expected->isError();
    const Type *wanted = isUsable ? expected : nullptr;
    tasks.pushTask(CheckExprSlot{.slot = &args[index - 1], .expected = wanted});
  }
}

VariantChoice VariantFilter::uncheckedChoice(FunctionDecl *variant) const {
  return VariantChoice{
      .function = variant->sym,
      .isChecked = false,
      .typeArgs = {},
      .isReported = false,
      .message = "",
  };
}

void VariantFilter::walkLoosely(std::vector<Expr *> &args) {
  for (size_t index = args.size(); index > 0; --index) {
    if (args[index - 1]->type == nullptr) {
      this->state.walk.tasks.pushTask(CheckExprSlot{.slot = &args[index - 1], .expected = nullptr});
    }
  }
}

VariantFilter::ResultChoiceAllowed
VariantFilter::checkNoSpread(llvm::StringRef what, const std::vector<bool> &spread) const {
  if (llvm::is_contained(spread, true)) {
    auto message = ErrorTextVariant::spread(what);

    return err(CodeClassesRegistryChooseVariantError{
        .code = CodeClassesRegistryChooseVariantErrorCode::SPREAD_ARGUMENT,
        .message = message,
    });
  }

  return ok();
}

VariantFilter::ResultVariantsFound
VariantFilter::checkFittingVariants(llvm::StringRef what, llvm::ArrayRef<FunctionDecl *> variants,
                                    size_t count) const {
  auto fit = std::vector<FunctionDecl *>();
  for (FunctionDecl *variant : variants) {
    if (this->isTakingCount(variant->sym, count)) {
      fit.push_back(variant);
    }
  }
  if (fit.empty()) {
    auto list = ErrorTextVariant::list(variants);
    auto message = ErrorTextVariant::count(what, count, list);

    return err(CodeClassesRegistryChooseVariantError{
        .code = CodeClassesRegistryChooseVariantErrorCode::NO_VARIANT_FOR_COUNT,
        .message = message,
    });
  }

  return ok(fit);
}

bool VariantFilter::hasFloatLiteral(const Expr *expr) const {
  for (const Expr *node : ExprParts::operatorTree(expr)) {
    if (isa<FloatLitExpr>(node)) {
      return true;
    }
  }

  return false;
}

std::string VariantFilter::variantsName(const FunctionDecl *first) const {
  if (first->isConstructor()) {
    return ErrorTextVariant::ctorOf(first->cls()->name);
  }

  return ErrorTextVariant::quoted(first->name);
}

bool VariantFilter::isTakingCount(const FuncSymbol *function, size_t count) const {
  bool isEnough = count >= function->minArgs;
  bool isNotTooMany = function->hasRest || count <= function->params.size();

  return isEnough && isNotTooMany;
}

const Type *VariantFilter::expectedArgument(llvm::ArrayRef<FunctionDecl *> fit, const Expr *arg,
                                            size_t index) const {
  const Type *common = this->commonParameter(fit, index);
  if (common != nullptr) {
    return common;
  }
  const Type *shape = this->soleObjectShape(fit, arg, index);
  if (shape != nullptr) {
    return shape;
  }

  return this->decimalForLiteral(fit, arg, index);
}

const Type *VariantFilter::commonParameter(llvm::ArrayRef<FunctionDecl *> fit, size_t index) const {
  const Type *common = this->choiceRules.parameterAt(fit[0]->sym, index);
  for (FunctionDecl *variant : fit) {
    bool isSame = this->choiceRules.parameterAt(variant->sym, index) == common;
    bool isOpen =
        common != nullptr && GenericCheckHelpers::mentionsAnyOf(common, variant->sym->typeParams);
    if (!isSame || isOpen) {
      return nullptr;
    }
  }

  return common;
}

const Type *VariantFilter::soleObjectShape(llvm::ArrayRef<FunctionDecl *> fit, const Expr *arg,
                                           size_t index) const {
  if (!isa<ObjectLitExpr>(arg)) {
    return nullptr;
  }
  auto shapes = this->objectShapes(fit, index);
  bool isSole = !shapes.empty() && llvm::all_equal(shapes);

  return isSole ? shapes[0] : nullptr;
}

std::vector<const Type *> VariantFilter::objectShapes(llvm::ArrayRef<FunctionDecl *> fit,
                                                      size_t index) const {
  auto shapes = std::vector<const Type *>();
  for (FunctionDecl *variant : fit) {
    const Type *param = this->choiceRules.parameterAt(variant->sym, index);
    if (this->isPlainObjectParam(param)) {
      shapes.push_back(param);
    }
  }

  return shapes;
}

bool VariantFilter::isPlainObjectParam(const Type *param) const {
  if (param == nullptr) {
    return false;
  }
  const Type *bare = param->nonOptional();

  return bare->isObject() && !bare->isClass() && !bare->isAnyObject() && !bare->isTuple();
}

const Type *VariantFilter::decimalForLiteral(llvm::ArrayRef<FunctionDecl *> fit, const Expr *arg,
                                             size_t index) const {
  if (!this->state.code.conversions.isNumberLiteralTree(arg) || !this->hasFloatLiteral(arg)) {
    return nullptr;
  }
  bool hasDecimal = false;
  bool hasDouble = false;
  for (FunctionDecl *variant : fit) {
    const Type *param = this->choiceRules.parameterAt(variant->sym, index)->nonOptional();
    hasDecimal = hasDecimal || param->isDecimal();
    hasDouble = hasDouble || param->isFloat();
  }
  if (hasDecimal && !hasDouble) {
    return Type::decimalTy();
  }

  return nullptr;
}

}
