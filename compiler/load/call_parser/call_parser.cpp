#include "compiler/load/call_parser/call_parser.h"
#include "compiler/load/class_parser/class_parser.h"
#include "compiler/load/decl_parser/decl_parser.h"
#include "compiler/load/expr_parser/expr_parser.h"
#include "compiler/load/function_parser/function_parser.h"
#include "compiler/load/module_parser/module_parser.h"
#include "compiler/load/pattern_parser/pattern_parser.h"
#include "compiler/load/primary_parser/primary_parser.h"
#include "compiler/load/stmt_parser/stmt_parser.h"
#include "compiler/load/template_parser/template_parser.h"
#include "compiler/load/to_utf8/to_utf8.h"
#include "compiler/load/token_cursor/token_cursor.h"
#include "compiler/load/type_parser/type_parser.h"
#include "compiler/load/var_parser/var_parser.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/SaveAndRestore.h"

namespace tz {

using llvm::dyn_cast;

CallParser::CallParser(Parser &parser) : parser_(parser) {}

Expr *CallParser::parseCallOrMember() {
  Expr *expr = parser_.primaries().parsePrimary();
  bool chain = false;
  auto finish = [&]() -> Expr * {
    return chain ? parser_.ctx_.make<OptionalChainExpr>(expr->loc(), expr) : expr;
  };
  for (;;) {
    if (auto *name = dyn_cast<NameExpr>(expr); name && !chain && parser_.tok_.is(Tok::Less) &&
                                               name->typeArgs.empty() &&
                                               parser_.types().tryTypeArgs(name->typeArgs)) {
      continue;
    }
    if (auto *member = dyn_cast<MemberExpr>(expr);
        member && parser_.tok_.is(Tok::Less) && member->typeArgs.empty() &&
        parser_.types().tryTypeArgs(member->typeArgs, true)) {
      continue;
    }
    bool optional = false;
    if (parser_.tok_.is(Tok::QuestionDot)) {
      optional = chain = true;
      parser_.cursor().advance();
      if (parser_.tok_.is(Tok::Identifier) || isKeyword(parser_.tok_.kind)) {
        auto *member = parser_.ctx_.make<MemberExpr>(parser_.tok_.loc(), expr, parser_.tok_.text);
        member->optional = true;
        expr = member;
        parser_.cursor().advance();
        continue;
      }
      if (!parser_.tok_.is(Tok::LParen) && !parser_.tok_.is(Tok::LBracket)) {
        parser_.cursor().error(parser_.tok_.loc(),
                               "expected a property name, '[' or '(' after '?.'");
        return finish();
      }
    }
    if (parser_.tok_.is(Tok::LParen)) {
      llvm::SMLoc loc = expr->loc();
      parser_.cursor().advance();
      llvm::SaveAndRestore<bool> allowIn(parser_.noIn_, false);
      std::vector<Expr *> args;
      std::vector<bool> spread;
      while (!parser_.tok_.is(Tok::RParen) && !parser_.tok_.is(Tok::Eof)) {
        bool spreads = parser_.cursor().accept(Tok::Ellipsis);
        if (spreads || !spread.empty()) {
          spread.resize(args.size(), false);
          spread.push_back(spreads);
        }
        args.push_back(parser_.exprs().parseExpr());
        if (!parser_.cursor().accept(Tok::Comma)) {
          break;
        }
      }
      parser_.cursor().expect(Tok::RParen, "to close the argument list");
      auto *call = parser_.ctx_.make<CallExpr>(loc, expr, std::move(args));
      call->optional = optional;
      call->spread = std::move(spread);
      expr = call;
    } else if (parser_.tok_.is(Tok::Dot)) {
      parser_.cursor().advance();
      llvm::SMLoc loc = parser_.tok_.loc();
      if (!parser_.tok_.is(Tok::Identifier) && !isKeyword(parser_.tok_.kind)) {
        parser_.cursor().error(parser_.tok_.loc(), "expected a property name after '.'");
        return finish();
      }
      expr = parser_.ctx_.make<MemberExpr>(loc, expr, parser_.tok_.text);
      parser_.cursor().advance();
    } else if (parser_.tok_.is(Tok::LBracket)) {
      llvm::SMLoc loc = parser_.tok_.loc();
      parser_.cursor().advance();
      llvm::SaveAndRestore<bool> allowIn(parser_.noIn_, false);
      Expr *index = parser_.exprs().parseSequence();
      parser_.cursor().expect(Tok::RBracket, "to close the index");
      auto *access = parser_.ctx_.make<IndexExpr>(loc, expr, index);
      access->optional = optional;
      expr = access;
    } else if (parser_.tok_.is(Tok::TemplateString) || parser_.tok_.is(Tok::TemplateHead)) {
      if (chain) {
        parser_.cursor().report(
            parser_.tok_.loc(),
            "a tagged template cannot be part of an optional chain; check the tag "
            "before the template");
      }
      auto *member = dyn_cast<MemberExpr>(expr);
      auto *object = member ? dyn_cast<NameExpr>(member->object) : nullptr;
      bool raw = object && object->name == "String" && member->name == "raw";
      llvm::SMLoc loc = expr->loc();
      TemplateExpr *tmpl = parser_.templates().parseTemplate(raw);
      auto *texts = parser_.ctx_.make<ArrayLitExpr>(tmpl->loc());
      for (std::u16string &text : tmpl->quasis) {
        texts->elements.push_back(
            {parser_.ctx_.make<StringLitExpr>(tmpl->loc(), std::move(text)), false});
      }
      std::vector<Expr *> args{texts};
      args.insert(args.end(), tmpl->exprs.begin(), tmpl->exprs.end());
      auto *call = parser_.ctx_.make<CallExpr>(loc, expr, std::move(args));
      call->tagged = true;
      expr = call;
    } else if (parser_.tok_.is(Tok::Bang) && !parser_.tok_.newlineBefore) {
      expr = parser_.ctx_.make<UnaryExpr>(parser_.tok_.loc(), UnaryOp::NonNull, expr);
      parser_.cursor().advance();
    } else {
      return finish();
    }
  }
}

Expr *CallParser::parseNew() {
  llvm::SMLoc loc = parser_.tok_.loc();
  parser_.cursor().advance();
  Expr *callee = nullptr;
  if (parser_.tok_.is(Tok::LParen)) {
    llvm::SMLoc at = parser_.tok_.loc();
    parser_.cursor().advance();
    llvm::SaveAndRestore<bool> allowIn(parser_.noIn_, false);
    callee = parser_.exprs().parseSequence();
    parser_.cursor().expect(Tok::RParen, "to close the class");
    std::vector<Expr *> args;
    if (parser_.cursor().accept(Tok::LParen)) {
      while (!parser_.tok_.is(Tok::RParen) && !parser_.tok_.is(Tok::Eof)) {
        args.push_back(parser_.exprs().parseExpr());
        if (!parser_.cursor().accept(Tok::Comma)) {
          break;
        }
      }
      parser_.cursor().expect(Tok::RParen, "to close the argument list");
    }
    auto *create = parser_.ctx_.make<NewExpr>(at, "", std::vector<TypeRef>(), std::move(args));
    create->callee = callee;
    return create;
  }
  if (!parser_.tok_.is(Tok::Identifier)) {
    parser_.cursor().error(parser_.tok_.loc(), "expected a class name after 'new'");
    return parser_.ctx_.make<IntLitExpr>(loc, 0);
  }
  llvm::StringRef name = parser_.tok_.text;
  llvm::SMLoc nameLoc = parser_.tok_.loc();
  parser_.cursor().advance();
  llvm::StringRef member;
  llvm::SMLoc memberLoc;
  if (parser_.cursor().accept(Tok::Dot)) {
    if (parser_.tok_.is(Tok::Identifier)) {
      member = parser_.tok_.text;
      memberLoc = parser_.tok_.loc();
      parser_.cursor().advance();
    } else {
      parser_.cursor().error(parser_.tok_.loc(), "expected a name after '.'");
    }
  }
  if (parser_.tok_.is(Tok::Dot) || parser_.tok_.is(Tok::LBracket)) {
    callee = parser_.ctx_.make<NameExpr>(nameLoc, name);
    if (!member.empty()) {
      callee = parser_.ctx_.make<MemberExpr>(memberLoc, callee, member);
    }
    while (parser_.tok_.is(Tok::Dot) || parser_.tok_.is(Tok::LBracket)) {
      if (parser_.cursor().accept(Tok::Dot)) {
        if (!parser_.tok_.is(Tok::Identifier) && !isKeyword(parser_.tok_.kind)) {
          parser_.cursor().error(parser_.tok_.loc(), "expected a property name after '.'");
          break;
        }
        callee = parser_.ctx_.make<MemberExpr>(parser_.tok_.loc(), callee, parser_.tok_.text);
        parser_.cursor().advance();
        continue;
      }
      llvm::SMLoc at = parser_.tok_.loc();
      parser_.cursor().advance();
      llvm::SaveAndRestore<bool> allowIn(parser_.noIn_, false);
      Expr *index = parser_.exprs().parseSequence();
      parser_.cursor().expect(Tok::RBracket, "to close the index");
      callee = parser_.ctx_.make<IndexExpr>(at, callee, index);
    }
    name = member = {};
  }
  std::vector<TypeRef> typeArgs;
  if (parser_.tok_.is(Tok::Less)) {
    typeArgs = parser_.types().parseTypeArgs();
  }
  std::vector<Expr *> args;
  if (parser_.cursor().accept(Tok::LParen)) {
    while (!parser_.tok_.is(Tok::RParen) && !parser_.tok_.is(Tok::Eof)) {
      args.push_back(parser_.exprs().parseExpr());
      if (!parser_.cursor().accept(Tok::Comma)) {
        break;
      }
    }
    parser_.cursor().expect(Tok::RParen, "to close the argument list");
  }
  auto *create = parser_.ctx_.make<NewExpr>(nameLoc, name, std::move(typeArgs), std::move(args));
  create->member = member;
  create->callee = callee;

  return create;
}

}
