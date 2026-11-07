#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/stream/set_encoding_checker/set_encoding_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class SetEncodingChecker {
public:
  explicit SetEncodingChecker(CheckState &owner);

  Expr *checkSetEncoding(CallExpr *call, MemberExpr *member, ClassDecl *cls);
  const Type *encodedStreamType(ClassDecl *cls, llvm::SMLoc loc);

private:
  using ResultStdinIsPlaced = Result<const Module *, SetEncodingCheckerStdinPlaceError>;
  using ResultDecodedChunks = Result<const Type *, SetEncodingCheckerDecodedChunksError>;

  ResultStdinIsPlaced stdinPlace() const;
  static ResultDecodedChunks decodedChunks(const Type *readable, const Type *receiver);

  CheckState &state;
};

}
