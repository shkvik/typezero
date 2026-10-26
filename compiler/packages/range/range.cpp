#include "compiler/packages/range/range.h"
#include "compiler/packages/partial/partial.h"
#include "compiler/packages/version/version.h"
#include "llvm/ADT/StringExtras.h"

namespace tz::pkg {

namespace {

Version floorOf(uint64_t major, uint64_t minor, uint64_t patch) {
  Version v;
  v.major = major;
  v.minor = minor;
  v.patch = patch;
  v.pre = {"0"};

  return v;
}

Version after(const Partial &p) {
  if (p.count == 1) {
    return floorOf(p.parts[0] + 1, 0, 0);
  }
  if (p.count == 2) {
    return floorOf(p.parts[0], p.parts[1] + 1, 0);
  }

  return floorOf(p.parts[0], p.parts[1], p.parts[2] + 1);
}

}

bool Range::parse(llvm::StringRef text, Range &out, std::string &error) {
  out = Range();
  out.text_ = text.trim().str();
  llvm::SmallVector<llvm::StringRef, 2> alternatives;
  llvm::StringRef(out.text_).split(alternatives, "||");
  for (llvm::StringRef alternative : alternatives) {
    Set set;
    if (!parseSet(alternative.trim(), set, error)) {
      return false;
    }
    out.sets_.push_back(std::move(set));
  }

  return true;
}

bool Range::parseSet(llvm::StringRef text, Set &out, std::string &error) {
  using Op = Comparator::Op;
  auto add = [&](Op op, Version v, bool synthetic = false) {
    out.push_back({op, std::move(v), synthetic});
  };
  llvm::SmallVector<llvm::StringRef, 4> pieces;
  text.split(pieces, ' ', -1, false);
  std::vector<std::string> words;
  for (size_t i = 0; i < pieces.size(); ++i) {
    std::string word = pieces[i].trim().str();
    if (word.empty()) {
      continue;
    }
    bool bareOperator = word == "<" || word == "<=" || word == ">" || word == ">=" || word == "=" ||
                        word == "^" || word == "~";
    if (bareOperator && i + 1 < pieces.size()) {
      word += pieces[++i].trim().str();
    }
    words.push_back(word);
  }
  auto bad = [&](llvm::StringRef word) {
    error = "'" + word.str() + "' is not a version or a range of versions";

    return false;
  };

  if (words.empty()) {
    return true;
  }
  if (words.size() == 3 && words[1] == "-") {
    Partial low, high;
    if (!parsePartial(words[0], low)) {
      return bad(words[0]);
    }
    if (!parsePartial(words[2], high)) {
      return bad(words[2]);
    }
    add(Op::GreaterEq, low.version());
    if (high.count == 3) {
      add(Op::LessEq, high.version());
    } else if (high.count > 0) {
      add(Op::Less, after(high), true);
    }
    return true;
  }

  for (llvm::StringRef word : words) {
    Op op = Op::Equal;
    char kind = 0;
    if (word.consume_front(">=")) {
      op = Op::GreaterEq;
    } else if (word.consume_front("<=")) {
      op = Op::LessEq;
    } else if (word.consume_front(">")) {
      op = Op::Greater;
    } else if (word.consume_front("<")) {
      op = Op::Less;
    } else if (word.consume_front("^")) {
      kind = '^';
    } else if (word.consume_front("~>") || word.consume_front("~")) {
      kind = '~';
    }
    Partial p;
    if (!parsePartial(word, p)) {
      return bad(word);
    }
    Version v = p.version();
    uint64_t major = p.parts[0], minor = p.parts[1];

    if (kind == '^') {
      if (p.count == 0) {
        continue;
      }
      add(Op::GreaterEq, v);
      if (p.count == 1 || major > 0) {
        add(Op::Less, floorOf(major + 1, 0, 0), true);
      } else if (p.count == 2 || minor > 0) {
        add(Op::Less, floorOf(0, minor + 1, 0), true);
      } else {
        add(Op::Less, floorOf(0, 0, p.parts[2] + 1), true);
      }
      continue;
    }
    if (kind == '~') {
      if (p.count == 0) {
        continue;
      }
      add(Op::GreaterEq, v);
      add(Op::Less, p.count == 1 ? floorOf(major + 1, 0, 0) : floorOf(major, minor + 1, 0), true);
      continue;
    }
    switch (op) {
    case Op::Equal:
      if (p.count == 3) {
        add(Op::Equal, v);
      } else if (p.count > 0) {
        add(Op::GreaterEq, v);
        add(Op::Less, after(p), true);
      }
      break;
    case Op::Greater:
      if (p.count == 3) {
        add(Op::Greater, v);
      } else if (p.count == 0) {
        add(Op::Less, floorOf(0, 0, 0), true);
      } else {
        Version next = after(p);
        next.pre.clear();
        add(Op::GreaterEq, next);
      }
      break;
    case Op::GreaterEq:
      if (p.count > 0) {
        add(Op::GreaterEq, v);
      }
      break;
    case Op::Less:
      if (p.count == 3) {
        add(Op::Less, v);
      } else {
        add(Op::Less, floorOf(p.parts[0], p.parts[1], 0), true);
      }
      break;
    case Op::LessEq:
      if (p.count == 3) {
        add(Op::LessEq, v);
      } else if (p.count > 0) {
        add(Op::Less, after(p), true);
      }
      break;
    }
  }

  return true;
}

bool Range::test(const Set &set, const Version &version) {
  for (const Comparator &c : set) {
    int order = version.compare(c.version);
    bool ok = false;
    switch (c.op) {
    case Comparator::Less:
      ok = order < 0;
      break;
    case Comparator::LessEq:
      ok = order <= 0;
      break;
    case Comparator::Greater:
      ok = order > 0;
      break;
    case Comparator::GreaterEq:
      ok = order >= 0;
      break;
    case Comparator::Equal:
      ok = order == 0;
      break;
    }
    if (!ok) {
      return false;
    }
  }
  if (!version.isPrerelease()) {
    return true;
  }
  for (const Comparator &c : set) {
    if (!c.synthetic && c.version.isPrerelease() && c.version.major == version.major &&
        c.version.minor == version.minor && c.version.patch == version.patch) {
      return true;
    }
  }

  return false;
}

bool Range::satisfies(const Version &version) const {
  for (const Set &set : sets_) {
    if (test(set, version)) {
      return true;
    }
  }

  return false;
}

}
