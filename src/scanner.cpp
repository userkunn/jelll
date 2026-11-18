#include "scanner.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <filesystem>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "util.hpp"

namespace jelll {

namespace {

enum class LangKind { Union, C, Sync, Rust, Zig, Js, Css, Html };

LangKind kind_for(const std::string& lang) {
  if (lang == "c") return LangKind::C;
  if (lang == "sync") return LangKind::Sync;
  if (lang == "rs") return LangKind::Rust;
  if (lang == "zig") return LangKind::Zig;
  if (lang == "js" || lang == "ts" || lang == "tsx") return LangKind::Js;
  if (lang == "css") return LangKind::Css;
  if (lang == "html") return LangKind::Html;
  return LangKind::Union;
}

bool is_ident_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

bool word_at(const std::string& s, size_t i, const std::string& word) {
  if (i + word.size() > s.size()) return false;
  if (s.compare(i, word.size(), word) != 0) return false;
  if (i > 0 && is_ident_char(s[i - 1])) return false;
  if (i + word.size() < s.size() && is_ident_char(s[i + word.size()])) return false;
  return true;
}

// Mask comments and string literals to spaces (newlines preserved) so that
// structure scanning never trips over `lang.x {}` or `call "..."` inside them.
class Masker {
 public:
  Masker(const std::string& src, LangKind kind) : s_(src), out_(src), kind_(kind) {
    // M-03: precompute "is at line start" once (O(n)) instead of rescanning
    // from the beginning for every candidate.
    compute_bol(src, bol_owned_);
    bol_ = &bol_owned_;
  }
  // R-03: region mode - no output copy and a SHARED line-start bitmap, so a
  // file with N blocks costs O(text) total instead of O(N * text).
  Masker(const std::string& src, LangKind kind, const std::vector<char>* shared_bol)
      : s_(src), kind_(kind), bol_(shared_bol) {}
  static void compute_bol(const std::string& src, std::vector<char>& out) {
    out.assign(src.size(), 0);
    bool bol = true;
    for (size_t k = 0; k < src.size(); ++k) {
      out[k] = bol ? 1 : 0;
      if (src[k] == '\n') bol = true;
      else if (!std::isspace(static_cast<unsigned char>(src[k]))) bol = false;
    }
  }
  std::string take() {
    size_t i = 0;
    code(i, false);
    return std::move(out_);
  }
  // R-03: scan the braced region starting just after '{' with this lexer and
  // stop at the matching '}'. Returns the absolute position of that '}' or
  // npos. Nothing outside the region is visited.
  size_t region_close(size_t start) {
    stopped_at_close_ = false;
    size_t i = start;
    code(i, true);
    return (stopped_at_close_ && i > 0) ? i - 1 : std::string::npos;
  }

 private:
  void blank(size_t pos) {
    if (pos < out_.size() && out_[pos] != '\n') out_[pos] = ' ';
  }

  // Process a code region. If stop_on_interp_close, a `}` at brace depth 0
  // ends the region (used for template `${ ... }`).
  void code(size_t& i, bool stop_on_interp_close) {
    // M-01: mutual recursion with template_() is depth-limited so adversarial
    // nesting cannot blow the native stack (SIGSEGV).
    DepthGuard dg(*this);
    int depth = 0;
    const size_t n = s_.size();
    while (i < n) {
      char c = s_[i];
      if (c == '}' && stop_on_interp_close) {
        if (depth == 0) {
          blank(i);
          ++i;
          stopped_at_close_ = true;
          return;
        }
        --depth;
        ++i;
        continue;
      }
      if (c == '{' && stop_on_interp_close) {
        ++depth;
        ++i;
        continue;
      }
      if (c == '/' && i + 1 < n && s_[i + 1] == '/' && kind_ != LangKind::Css && kind_ != LangKind::Html) {
        while (i < n && s_[i] != '\n') blank(i++);
        continue;
      }
      if (c == '/' && i + 1 < n && s_[i + 1] == '*') {
        blank(i);
        blank(i + 1);
        i += 2;
        while (i < n) {
          if (s_[i] == '*' && i + 1 < n && s_[i + 1] == '/') {
            blank(i);
            blank(i + 1);
            i += 2;
            break;
          }
          blank(i++);
        }
        continue;
      }
      if (kind_ == LangKind::Html && c == '<' && s_.compare(i, 4, "<!--") == 0) {
        for (size_t k = 0; k < 4; ++k) blank(i + k);
        i += 4;
        while (i < n) {
          if (s_[i] == '-' && s_.compare(i, 3, "-->") == 0) {
            for (size_t k = 0; k < 3; ++k) blank(i + k);
            i += 3;
            break;
          }
          blank(i++);
        }
        continue;
      }
      // C++ raw string literal R"delim( ... )delim"
      if ((kind_ == LangKind::C || kind_ == LangKind::Sync) && c == 'R' && i + 1 < n && s_[i + 1] == '"' &&
          (i == 0 || !is_ident_char(s_[i - 1]))) {
        size_t j = i + 2;
        std::string delim;
        while (j < n && s_[j] != '(' && delim.size() < 16) delim.push_back(s_[j++]);
        if (j < n && s_[j] == '(') {
          std::string close = ")" + delim + "\"";
          for (size_t k = i; k <= j; ++k) blank(k);
          i = j + 1;
          size_t end = s_.find(close, i);
          size_t stop = (end == std::string::npos) ? n : end + close.size();
          for (size_t k = i; k < stop && k < n; ++k) blank(k);
          i = stop;
          continue;
        }
      }
      // Rust raw strings: r"..." / r#"..."# / br##"..."##
      if ((kind_ == LangKind::Rust || kind_ == LangKind::Union) &&
          (c == 'r' || (c == 'b' && i + 1 < n && s_[i + 1] == 'r')) &&
          (i == 0 || !is_ident_char(s_[i - 1]))) {
        size_t j = (c == 'b') ? i + 2 : i + 1;
        size_t hashes = 0;
        while (j < n && s_[j] == '#') {
          ++hashes;
          ++j;
        }
        if (j < n && s_[j] == '"') {
          for (size_t k = i; k <= j; ++k) blank(k);
          i = j + 1;
          while (i < n) {
            if (s_[i] == '"') {
              size_t k = i + 1;
              size_t h = 0;
              while (k < n && h < hashes && s_[k] == '#') {
                ++h;
                ++k;
              }
              if (h == hashes) {
                for (size_t p = i; p < k; ++p) blank(p);
                i = k;
                break;
              }
            }
            blank(i++);
          }
          continue;
        }
      }
      // Zig multiline string lines: first non-space chars on a line are two backslashes.
      if (kind_ == LangKind::Zig && c == '\\' && i + 1 < n && s_[i + 1] == '\\') {
        bool line_start = bol_ && i < bol_->size() && (*bol_)[i] != 0;
        if (line_start) {
          while (i < n && s_[i] != '\n') blank(i++);
          continue;
        }
      }
      if (c == '"' || c == '\'') {
        // `'` is a string/char quote in every supported language EXCEPT that
        // Rust (and legacy mixed input) also has lifetimes ('a) and
        // apostrophes. Decide per language.
        if (c == '\'') {
          if (kind_ == LangKind::Rust) {
            // Rust: only single-char literals are quotes; anything else is a
            // lifetime (or an apostrophe) and stays code.
            bool is_char = false;
            if (i + 1 < n && s_[i + 1] == '\\') is_char = true;
            else if (i + 1 < n && s_[i + 1] == '\'') is_char = true;
            else if (i + 2 < n && s_[i + 2] == '\'' && s_[i + 1] != '\n') is_char = true;
            if (!is_char) {
              ++i;  // Rust lifetime or stray apostrophe: leave as code.
              continue;
            }
          } else if (kind_ == LangKind::Union) {
            // Legacy mixed input: a `'` is a quote only when a closing `'`
            // exists on the same line (Rust lifetimes such as &'a have none).
            size_t k = i + 1;
            bool closed = false;
            while (k < n && s_[k] != '\n') {
              if (s_[k] == '\\') {
                k += 2;
                continue;
              }
              if (s_[k] == '\'') {
                closed = true;
                break;
              }
              ++k;
            }
            if (!closed) {
              ++i;  // lifetime or stray apostrophe
              continue;
            }
          }
          // JS/TS/C/C++/CSS/HTML: `'` always opens a string or char literal.
        }
        char q = c;
        blank(i++);
        while (i < n) {
          if (s_[i] == '\\') {
            blank(i);
            if (i + 1 < n) blank(i + 1);
            i += 2;
            continue;
          }
          if (s_[i] == '\n') break;  // unterminated literal: resume at the line end
          blank(i);
          if (s_[i] == q) {
            ++i;
            break;
          }
          ++i;
        }
        continue;
      }
      // Template literals (JS/TS/TSX and union contexts).
      if (c == '`' && (kind_ == LangKind::Js || kind_ == LangKind::Union)) {
        template_(i);
        continue;
      }
      // JS/TS regex literal heuristic: `/` after an operator/punctuation.
      if (c == '/' && (kind_ == LangKind::Js || kind_ == LangKind::Union) && regex_allowed(i)) {
        blank(i++);
        bool in_class = false;
        while (i < n) {
          if (s_[i] == '\\') {
            blank(i);
            if (i + 1 < n) blank(i + 1);
            i += 2;
            continue;
          }
          if (s_[i] == '\n') break;
          if (s_[i] == '[') in_class = true;
          else if (s_[i] == ']') in_class = false;
          else if (s_[i] == '/' && !in_class) {
            blank(i++);
            break;
          }
          blank(i++);
        }
        while (i < n && std::isalpha(static_cast<unsigned char>(s_[i]))) blank(i++);
        continue;
      }
      ++i;
    }
  }

  struct DepthGuard {
    Masker& m;
    explicit DepthGuard(Masker& mm) : m(mm) {
      if (++m.nest_ > kMaxNest) {
        throw BuildError("Nesting too deep in template literal",
                         "テンプレートリテラル/補間のネストは " + std::to_string(kMaxNest) +
                             " 段までです", m.file_, 0);
      }
    }
    ~DepthGuard() { --m.nest_; }
  };

  bool regex_allowed(size_t i) {
    // Look at previous non-whitespace, non-comment char. After an identifier
    // or ')' / ']' a `/` is division; but after an expression keyword it is a
    // regex literal (`return /}/;` must not leak its brace into the scan).
    size_t k = i;
    while (k > 0) {
      --k;
      char p = s_[k];
      if (std::isspace(static_cast<unsigned char>(p))) continue;
      if (p == ')' || p == ']') return false;
      if (is_ident_char(p)) {
        size_t e = k + 1;
        while (k > 0 && is_ident_char(s_[k - 1])) --k;
        std::string word = s_.substr(k, e - k);
        static const std::vector<std::string> kExprKeywords = {
            "return", "typeof", "instanceof", "in",    "of",   "new",  "delete",
            "void",   "case",   "do",        "else",  "yield", "await", "throw"};
        for (const auto& kw : kExprKeywords) {
          if (word == kw) return true;
        }
        return false;  // identifier or number: division
      }
      return true;  // ( , = : [ ! & | ? { ; } + * etc.
    }
    return true;
  }

  void template_(size_t& i) {
    DepthGuard dg(*this);
    const size_t n = s_.size();
    blank(i++);
    while (i < n) {
      if (s_[i] == '\\') {
        blank(i);
        if (i + 1 < n) blank(i + 1);
        i += 2;
        continue;
      }
      if (s_[i] == '`') {
        blank(i++);
        return;
      }
      if (s_[i] == '$' && i + 1 < n && s_[i + 1] == '{') {
        blank(i);
        blank(i + 1);
        i += 2;
        code(i, true);
        continue;
      }
      blank(i++);
    }
  }

  const std::string& s_;
  std::string out_;
  LangKind kind_;
  std::vector<char> bol_owned_;
  const std::vector<char>* bol_ = nullptr;
  bool stopped_at_close_ = false;
  int nest_ = 0;
  static constexpr int kMaxNest = 64;
  std::string file_;  // diagnostics only (filled by mask_source callers)
};

std::string mask(const std::string& s, LangKind kind) { return Masker(s, kind).take(); }

// R-06 + huge-file defense: containment is verified against the fd we are
// ACTUALLY about to read (/proc/self/fd/N resolves the kernel's truth), and
// the size is capped before/while reading - check and read share one open
// file description, so a symlink swap between them cannot redirect us.
std::optional<std::string> read_file_checked(const std::string& path, const std::string& root,
                                             bool contain, bool* outside, size_t max_bytes) {
  namespace fs = std::filesystem;
  *outside = false;
  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return std::nullopt;
  std::string actual;
  if (contain && !root.empty()) {
    // R-06: FAIL-CLOSED. The identity of the opened fd must be proven via
    // procfs; if that cannot be verified there is NO fallback to the path
    // string - the request is rejected outright.
    std::string link = "/proc/self/fd/" + std::to_string(fd);
    char buf[4096];
    ssize_t m = ::readlink(link.c_str(), buf, sizeof buf - 1);
    if (m <= 0) {
      ::close(fd);
      throw BuildError("Cannot verify call target",
                       "call 対象ファイルの実体を検証できません (/proc が利用不能)。"
                       "セキュリティ上の理由で読み込みを拒否します: " + path);
    }
    actual.assign(buf, static_cast<size_t>(m));
    if (auto sp = actual.rfind(" (deleted)"); sp != std::string::npos) actual.resize(sp);
  }
  if (contain && !root.empty()) {
    std::error_code ec1, ec2;
    fs::path a = fs::weakly_canonical(fs::path(actual), ec1);
    fs::path r = fs::weakly_canonical(fs::path(root), ec2);
    std::string as = ec1 ? actual : a.string();
    std::string rs = ec2 ? root : r.string();
    std::string prefix = (rs.empty() || rs.back() == '/') ? rs : rs + "/";
    if (as != rs && as.rfind(prefix, 0) != 0) {
      *outside = true;
      ::close(fd);
      return std::nullopt;
    }
  }
  struct stat st {};
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    ::close(fd);
    return std::nullopt;
  }
  if (static_cast<size_t>(st.st_size) > max_bytes) {
    ::close(fd);
    throw BuildError("Input file too large",
                     "入力ファイルが上限 (" + std::to_string(max_bytes / (1024 * 1024)) +
                         " MiB) を超えています: " + path);
  }
  std::string content;
  content.reserve(static_cast<size_t>(st.st_size));
  char buf[65536];
  size_t total = 0;
  while (true) {
    ssize_t n = ::read(fd, buf, sizeof buf);
    if (n < 0) {
      if (errno == EINTR) continue;
      ::close(fd);
      return std::nullopt;
    }
    if (n == 0) break;
    total += static_cast<size_t>(n);
    if (total > max_bytes) {
      ::close(fd);
      throw BuildError("Input file too large",
                       "入力ファイルが上限 (" + std::to_string(max_bytes / (1024 * 1024)) +
                           " MiB) を超えています: " + path);
    }
    content.append(buf, static_cast<size_t>(n));
  }
  ::close(fd);
  return content;
}

const std::set<std::string> kLangs = {"c", "sync", "rs", "zig", "js", "ts", "tsx", "css", "html"};

struct FileParser {
  FileParser(Program& p, std::vector<std::string>& s) : program(p), call_stack(s) {}
  Program& program;
  std::vector<std::string>& call_stack;
  std::string root_dir;             // S-01: call targets must stay inside here
  std::set<std::string> expanded;   // M-02: include-once (DAG blowup guard)
  size_t total_bytes = 0;
  size_t total_blocks = 0;
  long calls = 0;
  // Hard input limits: "linear time in the input" is the only real DoS fix.
  static constexpr size_t kMaxBytes = 64u * 1024 * 1024;
  static constexpr size_t kMaxBlocks = 20000;
  static constexpr long kMaxCalls = 100000;
  static constexpr int kMaxCallDepth = 64;

  void parse(const std::string& path, const std::string& original_for_diag) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path canon = fs::weakly_canonical(fs::path(path), ec);
    std::string canon_str = ec ? path : canon.string();
    for (const auto& entry : call_stack) {
      if (entry == canon_str) {
        std::string chain;
        for (const auto& c : call_stack) chain += c + " -> ";
        chain += canon_str;
        throw BuildError("Circular call detected",
                         "call の循環参照を確認してください: " + chain, original_for_diag, 0);
      }
    }
    if (expanded.count(canon_str)) return;  // M-02: already included once
    if (++calls > kMaxCalls) {
      throw BuildError("Too many call expansions",
                       "call の展開回数が上限 (" + std::to_string(kMaxCalls) +
                           ") を超えました。DAG の多重参照を確認してください",
                       original_for_diag, 0);
    }
    if (static_cast<int>(call_stack.size()) >= kMaxCallDepth) {
      throw BuildError("call nesting too deep",
                       "call のネストは " + std::to_string(kMaxCallDepth) + " 段までです",
                       original_for_diag, 0);
    }
    bool outside = false;
    auto content = read_file_checked(path, root_dir, /*contain=*/!call_stack.empty(),
                                     &outside, kMaxBytes);
    if (!content) {
      if (outside) {
        throw BuildError("Call target outside project root",
                         "call はプロジェクトディレクトリ内のファイルのみ参照できます: " + path,
                         original_for_diag, 0);
      }
      throw BuildError("Failed to read call target",
                       "call \"...\" のファイルパスを確認してください: " + path, original_for_diag, 0);
    }
    total_bytes += content->size();
    if (total_bytes > kMaxBytes) {
      throw BuildError("Expanded input too large",
                       "call 展開後の総入力が上限 (" + std::to_string(kMaxBytes / (1024 * 1024)) +
                           " MiB) を超えました",
                       original_for_diag, 0);
    }
    program.deps.push_back(canon_str);  // deduped by `expanded`
    expanded.insert(canon_str);
    call_stack.push_back(canon_str);
    // M-04: unwind `call_stack` on exceptions too.
    struct StackGuard {
      std::vector<std::string>& v;
      ~StackGuard() { v.pop_back(); }
    } guard{call_stack};
    scan(*content, path);
  }

  void scan(const std::string& text, const std::string& file) {
    // R-03: one shared line-start bitmap per file (Zig \\ line handling).
    std::vector<char> shared_bol;
    Masker::compute_bol(text, shared_bol);
    std::string outer = mask(text, LangKind::Union);
    const size_t n = text.size();
    size_t i = 0;
    int line = 1;
    auto advance = [&](size_t count) {
      for (size_t k = 0; k < count && i < n; ++k) {
        if (text[i] == '\n') ++line;
        ++i;
      }
    };
    while (i < n) {
      if (outer[i] == text[i] && word_at(outer, i, "call")) {
        // call "relative/path" directive at top level.
        int call_line = line;
        size_t j = i + 4;
        advance(4);
        while (j < n && std::isspace(static_cast<unsigned char>(text[j]))) advance(1), ++j;
        if (j >= n || text[j] != '"') {
          throw BuildError("Failed to read call target",
                           "call ディレクティブの書式は call \"relative/path\" です", file, call_line);
        }
        ++j;
        advance(1);
        std::string target;
        while (j < n && text[j] != '"' && text[j] != '\n') {
          target.push_back(text[j]);
          advance(1);
          ++j;
        }
        if (j >= n || text[j] != '"') {
          throw BuildError("Failed to read call target",
                           "call ディレクティブの引用符が閉じていません", file, call_line);
        }
        advance(1);
        // Consume the rest of the line (must be blank or a comment).
        while (i < n && text[i] != '\n') {
          if (outer[i] != ' ' && outer[i] != '\t' && outer[i] != '\r') {
            // Any real code/comment content after the path is ignored, matching
            // "call はその場に展開" semantics.
            break;
          }
          advance(1);
        }
        if (target.empty()) {
          throw BuildError("Failed to read call target", "call のパスが空です", file, call_line);
        }
        std::filesystem::path rel(target);
        std::filesystem::path base = std::filesystem::path(file).parent_path();
        std::filesystem::path resolved = rel.is_absolute() ? rel : (base / rel);
        // S-01 (LFI): force every call target inside the project root, after
        // symlink resolution. No absolute paths, no `..` escapes.
        {
          std::error_code ec1, ec2;
          std::filesystem::path cand = std::filesystem::weakly_canonical(resolved, ec1);
          std::filesystem::path root = std::filesystem::weakly_canonical(root_dir, ec2);
          std::string cs = ec1 ? resolved.string() : cand.string();
          std::string rs = ec2 ? root_dir : root.string();
          if (cs != rs && cs.rfind(rs.empty() || rs.back() == '/' ? rs : rs + "/", 0) != 0) {
            throw BuildError("Call target outside project root",
                             "call はプロジェクトディレクトリ内のファイルのみ参照できます: " + target,
                             file, call_line);
          }
        }
        parse(resolved.string(), file);
        continue;
      }
      if (outer[i] == text[i] && word_at(outer, i, "lang")) {
        size_t j = i + 4;
        if (j < n && text[j] == '.') {
          size_t k = j + 1;
          std::string tag;
          while (k < n && is_ident_char(text[k])) tag.push_back(text[k++]);
          size_t ws = k;
          while (ws < n && std::isspace(static_cast<unsigned char>(text[ws]))) ++ws;
          if (!tag.empty() && ws < n && text[ws] == '{') {
            if (!kLangs.count(tag)) {
              throw BuildError("Unsupported language block 'lang." + tag + "'",
                               "対応言語: c, rs, zig, js, ts, tsx, css, html", file, line);
            }
            int block_line = line;
            // Advance to the opening brace.
            advance(static_cast<size_t>(ws - i) + 1);
            size_t body_start = i;
            // R-03: brace-match with the block language's own lexer, visiting
            // ONLY this region (no copy of the remaining text, no re-mask of
            // the tail). N blocks now cost O(text) total, not O(N * text).
            Masker region(text, kind_for(tag), &shared_bol);
            size_t close = region.region_close(body_start);
            if (close == std::string::npos) {
              throw BuildError("Unterminated lang." + tag + " block",
                               "対応する '}' が見つかりません", file, block_line);
            }
            size_t body_end = close - body_start;
            if (++total_blocks > kMaxBlocks) {
              throw BuildError("Too many language blocks",
                               "ブロック数が上限 (" + std::to_string(kMaxBlocks) + ") を超えました",
                               file, block_line);
            }
            Block b;
            b.lang = tag;
            b.body = text.substr(body_start, body_end);  // body_end = close - body_start
            b.file = file;
            b.line = block_line;
            program.blocks.push_back(std::move(b));
            advance(static_cast<size_t>(body_end) + 1);
            continue;
          }
        }
      }
      advance(1);
    }
  }
};

}  // namespace

std::string mask_source(const std::string& text, const std::string& lang) {
  return mask(text, kind_for(lang));
}

Program parse_program(const std::string& root_path) {
  namespace fs = std::filesystem;
  std::error_code ec;
  if (!fs::exists(root_path, ec)) {
    throw BuildError("Failed to read call target", "ファイルが存在しません: " + root_path);
  }
  Program program;
  std::vector<std::string> stack;
  FileParser fp(program, stack);
  {
    fs::path parent = fs::path(root_path).parent_path();
    fs::path root = fs::weakly_canonical(parent.empty() ? fs::path(".") : parent, ec);
    fp.root_dir = ec ? (parent.empty() ? std::string(".") : parent.string()) : root.string();
  }
  fp.parse(root_path, root_path);
  return program;
}

}  // namespace jelll
