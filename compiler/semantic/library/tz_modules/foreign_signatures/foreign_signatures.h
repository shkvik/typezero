#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/library/tz_modules/foreign_signatures/foreign_signatures_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class ForeignSignatures {
public:
  explicit ForeignSignatures(SemanticState &owner);

  void walkForeign(FunctionDecl *functionDecl);

private:
  using ResultSignatureFits = Result<const FunctionDecl *, ForeignSignaturesPlainSignatureError>;
  using ResultParamCrossesC = Result<const Type *, ForeignSignaturesForeignParamError>;
  using ResultValueCrossesC = Result<const Type *, ForeignSignaturesForeignReturnError>;

  ResultSignatureFits checkSignatureFits(const FunctionDecl *functionDecl) const;
  ResultParamCrossesC checkParamCrossesC(const Param &param, const Type *type) const;
  ResultValueCrossesC checkValueCrossesC(const FunctionDecl *functionDecl) const;

  bool isForeignArgument(const Type *type) const;
  bool isForeignResult(const Type *type) const;
  bool isForeignNumber(const Type *type) const;
  bool isForeignPointer(const Type *type) const;
  bool isForeignArray(const Type *type) const;
  bool isForeignBytes(const Type *type) const;
  bool isForeignOut(const Type *type) const;

  SemanticState &state;
};

}
