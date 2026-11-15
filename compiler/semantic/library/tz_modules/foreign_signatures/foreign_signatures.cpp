#include "compiler/semantic/library/tz_modules/foreign_signatures/foreign_signatures.h"
#include "compiler/program/library/ffi/foreign_out/foreign_out.h"

namespace tz {

ForeignSignatures::ForeignSignatures(SemanticState &owner)
    : state(owner) {}

void ForeignSignatures::walkForeign(FunctionDecl *functionDecl) {
  FuncSymbol *function = functionDecl->sym;
  functionDecl->checked = true;
  auto signature = this->checkSignatureFits(functionDecl);
  if (signature.isErr()) {
    this->state.diagnostics.error(functionDecl->loc(), signature.error().message);
    return;
  }
  for (size_t index = 0; index < functionDecl->params.size(); ++index) {
    auto param = this->checkParamCrossesC(functionDecl->params[index], function->params[index]);
    if (param.isErr()) {
      this->state.diagnostics.error(functionDecl->params[index].loc, param.error().message);
    }
  }
  auto value = this->checkValueCrossesC(functionDecl);
  if (value.isErr()) {
    this->state.diagnostics.error(functionDecl->loc(), value.error().message);
  }
}

ForeignSignatures::ResultSignatureFits
ForeignSignatures::checkSignatureFits(const FunctionDecl *functionDecl) const {
  if (!functionDecl->typeParams.empty() || functionDecl->sym->hasRest) {
    auto message = ErrorTextForeign::plain();

    return err(ForeignSignaturesPlainSignatureError{
        .code = ForeignSignaturesPlainSignatureErrorCode::TYPE_PARAMS_OR_REST,
        .message = message,
    });
  }

  return ok(functionDecl);
}

ForeignSignatures::ResultParamCrossesC
ForeignSignatures::checkParamCrossesC(const Param &param, const Type *type) const {
  if (param.defaultValue() != nullptr || param.optional()) {
    auto message = ErrorTextForeign::optional();

    return err(ForeignSignaturesForeignParamError{
        .code = ForeignSignaturesForeignParamErrorCode::OPTIONAL_PARAM,
        .message = message,
    });
  }
  if (!type->isError() && !this->isForeignArgument(type)) {
    auto message = ErrorTextForeign::takes(type->name());

    return err(ForeignSignaturesForeignParamError{
        .code = ForeignSignaturesForeignParamErrorCode::PARAM_TYPE,
        .message = message,
    });
  }

  return ok(type);
}

ForeignSignatures::ResultValueCrossesC
ForeignSignatures::checkValueCrossesC(const FunctionDecl *functionDecl) const {
  if (!functionDecl->returnType.has_value()) {
    auto message = ErrorTextForeign::noReturn();

    return err(ForeignSignaturesForeignReturnError{
        .code = ForeignSignaturesForeignReturnErrorCode::NO_RETURN_TYPE,
        .message = message,
    });
  }
  const auto *returnType = functionDecl->sym->getKnownReturnType();
  bool isPassing = returnType == nullptr || returnType->isError() || returnType->isVoid() ||
                   this->isForeignResult(returnType);
  if (!isPassing) {
    auto message = ErrorTextForeign::gives(returnType->name());

    return err(ForeignSignaturesForeignReturnError{
        .code = ForeignSignaturesForeignReturnErrorCode::RETURN_TYPE,
        .message = message,
    });
  }

  return ok(returnType);
}

bool ForeignSignatures::isForeignArgument(const Type *type) const {
  return this->isForeignResult(type) || this->isForeignArray(type) || this->isForeignBytes(type) ||
         this->isForeignOut(type);
}

bool ForeignSignatures::isForeignOut(const Type *type) const {
  ForeignOut out;
  if (!out.isOut(type)) {
    return false;
  }
  const Type *element = out.getElement(type);

  return element != nullptr &&
         (this->isForeignNumber(element) || element->isBool() || element->isHandle());
}

bool ForeignSignatures::isForeignResult(const Type *type) const {
  return this->isForeignNumber(type) || type->isBool() || this->isForeignPointer(type);
}

bool ForeignSignatures::isForeignNumber(const Type *type) const {
  return type->isInteger() || type->isFloat();
}

bool ForeignSignatures::isForeignPointer(const Type *type) const {
  const Type *pointed = type->nonOptional();

  return pointed->isHandle() || pointed->isCString();
}

bool ForeignSignatures::isForeignArray(const Type *type) const {
  if (!type->isArray()) {
    return false;
  }
  const Type *element = type->inner();

  return this->isForeignNumber(element) || element->isHandle();
}

bool ForeignSignatures::isForeignBytes(const Type *type) const {
  return type->isBuffer() || type->isTypedArray();
}

}
