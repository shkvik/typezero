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
#include "compiler/load/token_cursor/token_cursor.h"
#include "compiler/load/type_parser/type_parser.h"
#include "compiler/load/var_parser/var_parser.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/SaveAndRestore.h"

namespace tz {

using llvm::dyn_cast;

CallParser::CallParser(Parser &parser)
    : parser(parser) {}

Expr *CallParser::parseCallOrMember() {
  Expr *expr = this->parser.primaries().parsePrimary();
  bool chain = false;
  auto finish = [&]() -> Expr * {
    return chain ? this->parser.ctx.make<OptionalChainExpr>(expr->loc(), expr) : expr;
  };
  for (;;) {
    if (auto *name = dyn_cast<NameExpr>(expr); name && !chain && this->parser.tok.is(Tok::Less) &&
                                               name->typeArgs.empty() &&
                                               this->parser.types().tryTypeArgs(name->typeArgs)) {
      continue;
    }
    if (auto *member = dyn_cast<MemberExpr>(expr);
        member && this->parser.tok.is(Tok::Less) && member->typeArgs.empty() &&
        this->parser.types().tryTypeArgs(member->typeArgs, true)) {
      continue;
    }
    bool optional = false;
    if (this->parser.tok.is(Tok::QuestionDot)) {
      optional = chain = true;
      this->parser.cursor().advance();
      if (this->parser.tok.is(Tok::Identifier) || TokenKinds::isKeyword(this->parser.tok.kind)) {
        auto *member =
            this->parser.ctx.make<MemberExpr>(this->parser.tok.loc(), expr, this->parser.tok.text);
        member->optional = true;
        expr = member;
        this->parser.cursor().advance();
        continue;
      }
      if (!this->parser.tok.is(Tok::LParen) && !this->parser.tok.is(Tok::LBracket)) {
        this->parser.cursor().error(this->parser.tok.loc(),
                                    "expected a property name, '[' or '(' after '?.'");
        return finish();
      }
    }
    if (this->parser.tok.is(Tok::LParen)) {
      llvm::SMLoc loc = expr->loc();
      this->parser.cursor().advance();
      llvm::SaveAndRestore<bool> allowIn(this->parser.noIn, false);
      std::vector<Expr *> args;
      std::vector<bool> spread;
      while (!this->parser.tok.is(Tok::RParen) && !this->parser.tok.is(Tok::Eof)) {
        bool spreads = this->parser.cursor().accept(Tok::Ellipsis);
        if (spreads || !spread.empty()) {
          spread.resize(args.size(), false);
          spread.push_back(spreads);
        }
        args.push_back(this->parser.exprs().parseExpr());
        if (!this->parser.cursor().accept(Tok::Comma)) {
          break;
        }
      }
      this->parser.cursor().expect(Tok::RParen, "to close the argument list");
      auto *call = this->parser.ctx.make<CallExpr>(loc, expr, std::move(args));
      call->optional = optional;
      call->spread = std::move(spread);
      expr = call;
    } else if (this->parser.tok.is(Tok::Dot)) {
      this->parser.cursor().advance();
      llvm::SMLoc loc = this->parser.tok.loc();
      if (!this->parser.tok.is(Tok::Identifier) && !TokenKinds::isKeyword(this->parser.tok.kind)) {
        this->parser.cursor().error(this->parser.tok.loc(), "expected a property name after '.'");
        return finish();
      }
      expr = this->parser.ctx.make<MemberExpr>(loc, expr, this->parser.tok.text);
      this->parser.cursor().advance();
    } else if (this->parser.tok.is(Tok::LBracket)) {
      llvm::SMLoc loc = this->parser.tok.loc();
      this->parser.cursor().advance();
      llvm::SaveAndRestore<bool> allowIn(this->parser.noIn, false);
      Expr *index = this->parser.exprs().parseSequence();
      this->parser.cursor().expect(Tok::RBracket, "to close the index");
      auto *access = this->parser.ctx.make<IndexExpr>(loc, expr, index);
      access->optional = optional;
      expr = access;
    } else if (this->parser.tok.is(Tok::TemplateString) || this->parser.tok.is(Tok::TemplateHead)) {
      if (chain) {
        this->parser.cursor().report(
            this->parser.tok.loc(),
            "a tagged template cannot be part of an optional chain; check the tag "
            "before the template");
      }
      auto *member = dyn_cast<MemberExpr>(expr);
      auto *object = member ? dyn_cast<NameExpr>(member->object) : nullptr;
      bool raw = object && object->name == "String" && member->name == "raw";
      llvm::SMLoc loc = expr->loc();
      TemplateExpr *tmpl = this->parser.templates().parseTemplate(raw);
      auto *texts = this->parser.ctx.make<ArrayLitExpr>(tmpl->loc());
      for (std::u16string &text : tmpl->quasis) {
        texts->elements.push_back(
            {this->parser.ctx.make<StringLitExpr>(tmpl->loc(), std::move(text)), false});
      }
      std::vector<Expr *> args{texts};
      args.insert(args.end(), tmpl->exprs.begin(), tmpl->exprs.end());
      auto *call = this->parser.ctx.make<CallExpr>(loc, expr, std::move(args));
      call->tagged = true;
      expr = call;
    } else if (this->parser.tok.is(Tok::Bang) && !this->parser.tok.newlineBefore) {
      expr = this->parser.ctx.make<UnaryExpr>(this->parser.tok.loc(), UnaryOp::NonNull, expr);
      this->parser.cursor().advance();
    } else {
      return finish();
    }
  }
}

Expr *CallParser::parseNew() {
  llvm::SMLoc loc = this->parser.tok.loc();
  this->parser.cursor().advance();
  Expr *callee = nullptr;
  if (this->parser.tok.is(Tok::LParen)) {
    llvm::SMLoc at = this->parser.tok.loc();
    this->parser.cursor().advance();
    llvm::SaveAndRestore<bool> allowIn(this->parser.noIn, false);
    callee = this->parser.exprs().parseSequence();
    this->parser.cursor().expect(Tok::RParen, "to close the class");
    std::vector<Expr *> args;
    if (this->parser.cursor().accept(Tok::LParen)) {
      while (!this->parser.tok.is(Tok::RParen) && !this->parser.tok.is(Tok::Eof)) {
        args.push_back(this->parser.exprs().parseExpr());
        if (!this->parser.cursor().accept(Tok::Comma)) {
          break;
        }
      }
      this->parser.cursor().expect(Tok::RParen, "to close the argument list");
    }
    auto *create = this->parser.ctx.make<NewExpr>(at, "", std::vector<TypeRef>(), std::move(args));
    create->callee = callee;
    return create;
  }
  if (!this->parser.tok.is(Tok::Identifier)) {
    this->parser.cursor().error(this->parser.tok.loc(), "expected a class name after 'new'");
    return this->parser.ctx.make<IntLitExpr>(loc, 0);
  }
  llvm::StringRef name = this->parser.tok.text;
  llvm::SMLoc nameLoc = this->parser.tok.loc();
  this->parser.cursor().advance();
  llvm::StringRef member;
  llvm::SMLoc memberLoc;
  if (this->parser.cursor().accept(Tok::Dot)) {
    if (this->parser.tok.is(Tok::Identifier)) {
      member = this->parser.tok.text;
      memberLoc = this->parser.tok.loc();
      this->parser.cursor().advance();
    } else {
      this->parser.cursor().error(this->parser.tok.loc(), "expected a name after '.'");
    }
  }
  if (this->parser.tok.is(Tok::Dot) || this->parser.tok.is(Tok::LBracket)) {
    callee = this->parser.ctx.make<NameExpr>(nameLoc, name);
    if (!member.empty()) {
      callee = this->parser.ctx.make<MemberExpr>(memberLoc, callee, member);
    }
    while (this->parser.tok.is(Tok::Dot) || this->parser.tok.is(Tok::LBracket)) {
      if (this->parser.cursor().accept(Tok::Dot)) {
        if (!this->parser.tok.is(Tok::Identifier) &&
            !TokenKinds::isKeyword(this->parser.tok.kind)) {
          this->parser.cursor().error(this->parser.tok.loc(), "expected a property name after '.'");
          break;
        }
        callee = this->parser.ctx.make<MemberExpr>(this->parser.tok.loc(), callee,
                                                   this->parser.tok.text);
        this->parser.cursor().advance();
        continue;
      }
      llvm::SMLoc at = this->parser.tok.loc();
      this->parser.cursor().advance();
      llvm::SaveAndRestore<bool> allowIn(this->parser.noIn, false);
      Expr *index = this->parser.exprs().parseSequence();
      this->parser.cursor().expect(Tok::RBracket, "to close the index");
      callee = this->parser.ctx.make<IndexExpr>(at, callee, index);
    }
    name = member = {};
  }
  std::vector<TypeRef> typeArgs;
  if (this->parser.tok.is(Tok::Less)) {
    typeArgs = this->parser.types().parseTypeArgs();
  }
  std::vector<Expr *> args;
  if (this->parser.cursor().accept(Tok::LParen)) {
    while (!this->parser.tok.is(Tok::RParen) && !this->parser.tok.is(Tok::Eof)) {
      args.push_back(this->parser.exprs().parseExpr());
      if (!this->parser.cursor().accept(Tok::Comma)) {
        break;
      }
    }
    this->parser.cursor().expect(Tok::RParen, "to close the argument list");
  }
  auto *create =
      this->parser.ctx.make<NewExpr>(nameLoc, name, std::move(typeArgs), std::move(args));
  create->member = member;
  create->callee = callee;

  return create;
}

}
