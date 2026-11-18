#include "setup.hpp"

#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "engine.hpp"
#include "util.hpp"

namespace jelll {

namespace fs = std::filesystem;

namespace {

bool dry_run() {
  const char* v = std::getenv("JELLL_SETUP_DRY_RUN");
  return v && *v && std::strcmp(v, "0") != 0;
}

// S-04 (supply chain): pinned versions and hashes baked into the binary.
// These are the versions the test suite is green against; the remote index
// files are NOT trusted for version or hash selection.
constexpr const char* kEsbuildVer = "0.28.2";
constexpr const char* kEmsdkVer = "6.0.13";
constexpr const char* kRustToolchain = "1.99.1";
constexpr const char* kZigVer = "0.17.1";
const std::map<std::string, std::string> kZigSha = {
    {"x86_64-linux",
     "3581863e3cbcfd0fce0efc682ce7e9e93da96668a977791d356a26d2a7e9b17a"},
    {"aarch64-linux",
     "a1546d5646eb184437918d6635ca6b762a0e889eac09cef720ee7a1ed3fe7795"}};

// R-09: every downloaded artifact is checked against a digest/commit that is
// FIXED IN THIS BINARY - never against a hash fetched from the same server.
// (Update these pins together with kEmsdkVer / kEsbuildVer / kRustToolchain.)
const std::map<std::string, std::string> kRustupInitSha = {
    {"x86_64", "b1c8cb14e30889e7e2999e6b69e62d9e9b9ffc0f4df8180564d306d9680e6c55"},
    {"aarch64", "9f608c142e0e38a78957d0661ba1dfaf0b6fb37184cfaa4b9a0aa57af6184282"}};
const std::map<std::string, std::string> kEsbuildBinSha = {
    {"x86_64", "e1698a3d5c6c0798fee4fd3b5cc816651f460c63d390a7a26ea4beb0b1884100"},
    {"aarch64", "90bd269553d258e80b19e3ccab4f98f0831f87442a2f056510ce24fd1b425fc2"}};
constexpr const char* kEmsdkCommit = "35659964bc2762fffec0d257997fb61475f765b7";

// Every download goes through the same hardened curl policy.
std::vector<std::string> curl_fetch(const std::string& url, const std::string& out) {
  return {"--proto", "=https", "--tlsv1.2", "--max-redirs", "0", "-fsSL", url, "-o", out};
}

// tar entries must stay inside the destination (no absolute paths / ..).
bool tar_listing_is_safe(const std::string& archive) {
  std::string log = archive + ".listing.tmp";
  auto r = run_command({"tar", "-tJf", archive}, log);
  auto content = read_file(log);
  std::error_code ec;
  fs::remove(log, ec);
  if (!r.started || r.exit_code != 0 || !content) return false;
  std::string entry;
  auto check = [&](std::string e) {
    if (e.empty()) return true;
    if (e[0] == '/') return false;
    if (e.find("..") != std::string::npos) return false;
    return true;
  };
  for (char c : *content) {
    if (c == '\n') {
      if (!check(entry)) return false;
      entry.clear();
    } else {
      entry.push_back(c);
    }
  }
  return check(entry);
}

int run_step(const std::vector<std::string>& argv,
             const std::map<std::string, std::string>& env = {}) {
  std::string line = "+";
  for (const auto& a : argv) line += " " + a;
  std::cout << line << std::endl;
  if (dry_run()) return 0;
  auto r = run_command(argv, "", env, /*inherit_stdio=*/true);
  if (!r.started) {
    std::cerr << "jelll: error: failed to start " << argv[0] << ": " << r.error << std::endl;
    return -1;
  }
  return r.exit_code;
}

std::string arch_tag() {
#if defined(__x86_64__)
  return "x86_64";
#elif defined(__aarch64__)
  return "aarch64";
#else
  return "unsupported";
#endif
}

bool install_esbuild(const std::string& td) {
  auto npm = find_in_path("npm");
  if (!npm) {
    std::cerr << "jelll: error: npm not found in PATH" << std::endl;
    std::cerr << "hint: Node.js をインストールしてください" << std::endl;
    return false;
  }
  std::cout << "[esbuild] installing esbuild@" << kEsbuildVer << " via npm..." << std::endl;
  // R-09: skip package lifecycle scripts and verify the binary we will
  // actually execute against a digest pinned in this program.
  if (run_step({*npm, "i", "--ignore-scripts", "--prefix", td + "/npm",
                std::string("esbuild@") + kEsbuildVer}) != 0) {
    return false;
  }
  if (!dry_run()) {
    std::string pkg = arch_tag() == "aarch64" ? "linux-arm64" : "linux-x64";
    std::string bin = td + "/npm/node_modules/@esbuild/" + pkg + "/bin/esbuild";
    auto it = kEsbuildBinSha.find(arch_tag() == "aarch64" ? "aarch64" : "x86_64");
    try {
      if (it == kEsbuildBinSha.end() || sha256_hex(bin) != it->second) {
        std::cerr << "jelll: error: esbuild binary sha256 mismatch: " << bin << std::endl;
        return false;
      }
    } catch (const BuildError& e) {
      std::cerr << "jelll: error: " << e.what() << std::endl;
      return false;
    }
  }
  return true;
}

bool install_emscripten(const std::string& td) {
  std::string dir = td + "/emsdk";
  if (!fs::exists(dir + "/emsdk")) {
    std::cout << "[emsdk] cloning " << kEmsdkVer << "..." << std::endl;
    auto git = find_in_path("git");
    if (!git) {
      std::cerr << "jelll: error: git not found in PATH" << std::endl;
      return false;
    }
    if (run_step({*git, "clone", "--depth", "1", "--branch", kEmsdkVer,
                  "https://github.com/emscripten-core/emsdk.git", dir}) != 0) {
      return false;
    }
  }
  if (!dry_run()) {
    // R-09: a tag name can be re-pointed; the commit cannot.
    std::string log = td + "/emsdk-commit.tmp";
    std::string git_bin = find_in_path("git").value_or("git");
    auto r = run_command({git_bin, "-C", dir, "rev-parse", "HEAD"}, log);
    auto head = read_file(log);
    std::error_code ec2;
    fs::remove(log, ec2);
    std::string head_s = head.value_or("");
    while (!head_s.empty() && (head_s.back() == '\n' || head_s.back() == '\r' ||
                               head_s.back() == ' ')) {
      head_s.pop_back();
    }
    if (!r.started || r.exit_code != 0 || head_s != kEmsdkCommit) {
      std::cerr << "jelll: error: emsdk commit mismatch (expected " << kEmsdkCommit
                << ", got " << head_s << ")" << std::endl;
      return false;
    }
  }
  if (run_step({dir + "/emsdk", "install", kEmsdkVer}) != 0) return false;
  return run_step({dir + "/emsdk", "activate", kEmsdkVer}) == 0;
}

bool install_rust(const std::string& td) {
  auto curl = find_in_path("curl");
  if (!curl) {
    std::cerr << "jelll: error: curl not found in PATH" << std::endl;
    return false;
  }
  fs::create_directories(td + "/rust");
  std::string init = td + "/rust/rustup-init";
  std::string url = "https://static.rust-lang.org/rustup/dist/x86_64-unknown-linux-gnu/rustup-init";
  if (arch_tag() == "aarch64") {
    url = "https://static.rust-lang.org/rustup/dist/aarch64-unknown-linux-gnu/rustup-init";
  }
  std::cout << "[rust] downloading rustup-init..." << std::endl;
  {
    auto args = curl_fetch(url, init);
    args.insert(args.begin(), *curl);
    if (run_step(args) != 0) return false;
  }
  if (!dry_run()) {
    // R-09: the digest comes from THIS binary, not from the same server that
    // served the artifact (a compromised mirror could swap both).
    auto it = kRustupInitSha.find(arch_tag() == "aarch64" ? "aarch64" : "x86_64");
    std::string actual;
    try {
      actual = sha256_hex(init);
    } catch (const BuildError& e) {
      std::cerr << "jelll: error: " << e.what() << std::endl;
      return false;
    }
    if (it == kRustupInitSha.end() || actual != it->second) {
      std::cerr << "jelll: error: rustup-init sha256 mismatch" << std::endl;
      return false;
    }
    if (::chmod(init.c_str(), 0755) != 0) {
      std::cerr << "jelll: error: chmod failed: " << init << std::endl;
      return false;
    }
  }
  std::map<std::string, std::string> env;
  env["RUSTUP_HOME"] = td + "/rustup";
  env["CARGO_HOME"] = td + "/cargo";
  // S-04: reproducible toolchain pin instead of floating "stable".
  return run_step({init, "-y", "--profile", "minimal", "--default-toolchain", kRustToolchain,
                   "--target", "wasm32-unknown-unknown", "--no-modify-path"},
                  env) == 0;
}

bool install_zig(const std::string& td) {
  auto curl = find_in_path("curl");
  auto tar = find_in_path("tar");
  if (!curl || !tar) {
    std::cerr << "jelll: error: curl / tar が必要です" << std::endl;
    return false;
  }
  std::string arch = arch_tag() + "-linux";
  auto sha_it = kZigSha.find(arch);
  if (arch_tag() == "unsupported" || sha_it == kZigSha.end()) {
    std::cerr << "jelll: error: unsupported architecture for Zig" << std::endl;
    return false;
  }
  fs::create_directories(td + "/zig");
  // S-04: pinned URL + pinned hash from the binary. The remote index.json is
  // never consulted, so a compromised mirror cannot swap the artifact.
  std::string tarball = std::string("https://ziglang.org/download/") + kZigVer + "/zig-" +
                        arch + "-" + kZigVer + ".tar.xz";
  std::string archive = td + "/zig/zig.tar.xz";
  std::cout << "[zig] downloading " << tarball << std::endl;
  {
    auto args = curl_fetch(tarball, archive);
    args.insert(args.begin(), *curl);
    if (run_step(args) != 0) return false;
  }
  if (!dry_run()) {
    try {
      if (sha256_hex(archive) != sha_it->second) {
        std::cerr << "jelll: error: zig archive sha256 mismatch" << std::endl;
        return false;
      }
    } catch (const BuildError& e) {
      std::cerr << "jelll: error: " << e.what() << std::endl;
      return false;
    }
    if (!tar_listing_is_safe(archive)) {
      std::cerr << "jelll: error: zig archive contains unsafe paths" << std::endl;
      return false;
    }
  }
  return run_step({*tar, "-xJf", archive, "--no-same-owner", "--strip-components=1",
                   "-C", td + "/zig"}) == 0;
}

std::string sh_squote(const std::string& s) {
  std::string o = "'";
  for (char c : s) {
    if (c == '\'') o += "'\\''";
    else o += c;
  }
  return o + "'";
}

bool register_path(const std::string& td) {
  const char* home = std::getenv("HOME");
  if (!home) return true;
  // S-02: never write shell code built from unquoted environment input, and
  // never register a toolchain directory outside $HOME.
  std::error_code ec;
  fs::path td_p = fs::weakly_canonical(td, ec);
  std::string td_s = ec ? td : td_p.string();
  std::string home_s(home);
  if (td_s.rfind(home_s + "/", 0) != 0 && td_s != home_s) {
    std::cerr << "jelll: error: refusing to register PATH outside $HOME: " << td_s << std::endl;
    return false;
  }
  const std::string block =
      "\n# jelll toolchains (added by jelll --setup)\n"
      "export PATH=" +
      sh_squote(td_s + "/cargo/bin") + ":" + sh_squote(td_s + "/emsdk/upstream/emscripten") +
      ":" + sh_squote(td_s + "/zig") + ":" + sh_squote(td_s + "/npm/node_modules/.bin") +
      ":\"$PATH\"\n";
  for (const char* rc : {".profile", ".bashrc"}) {
    std::string path = home_s + "/" + rc;
    std::string existing = read_file(path).value_or("");
    if (existing.find("# jelll toolchains") != std::string::npos) continue;
    if (dry_run()) {
      std::cout << "[path] would append to " << path << std::endl;
      continue;
    }
    std::ofstream out(path, std::ios::app);
    if (!out) {
      std::cerr << "jelll: error: cannot append to " << path << std::endl;
      return false;
    }
    out << block;
    if (!out) {
      std::cerr << "jelll: error: write failed: " << path << std::endl;
      return false;
    }
    std::cout << "[path] updated " << path << std::endl;
  }
  return true;
}

// ---- terminal multiselect -------------------------------------------------

struct RawTerminal {
  termios saved{};
  bool active = false;
  void enable() {
    if (!::isatty(STDIN_FILENO)) return;
    if (tcgetattr(STDIN_FILENO, &saved) != 0) return;
    termios raw = saved;
    raw.c_lflag &= ~static_cast<tcflag_t>(ECHO | ICANON);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    // TCSANOW (not TCSAFLUSH): keep input that arrived before raw mode.
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0) active = true;
  }
  void disable() {
    if (active) tcsetattr(STDIN_FILENO, TCSANOW, &saved);
    active = false;
  }
  ~RawTerminal() { disable(); }
};

// Renders the menu; returns selected flags (empty = cancelled / no TTY).
std::vector<bool> multiselect(const std::vector<std::string>& items) {
  std::vector<bool> sel(items.size(), false);
  RawTerminal term;
  term.enable();
  if (!term.active) {
    term.disable();
    std::cerr << "jelll: error: 対話型セットアップにはTTYが必要です" << std::endl;
    std::cerr << "hint: ターミナルから実行してください" << std::endl;
    return {};
  }
  int cursor = 0;
  bool first = true;
  while (true) {
    if (!first) std::printf("\x1b[%zuA\x1b[J", items.size() + 2);
    std::printf("\x1b[1m? どのコンパイラをインストールしますか？\x1b[0m\n");
    for (size_t i = 0; i < items.size(); ++i) {
      std::printf("%s %s %s\x1b[0m\n", (int)i == cursor ? "\x1b[36m>" : " ",
                  sel[i] ? "[\x1b[32m*\x1b[0m]" : "[ ]", items[i].c_str());
    }
    std::printf("(Space: 選択/解除, Enter: 決定)\n");
    std::fflush(stdout);
    first = false;
    char c = 0;
    if (::read(STDIN_FILENO, &c, 1) != 1) break;
    if (c == '\r' || c == '\n') break;
    if (c == 'q') {
      term.disable();
      std::printf("\n");
      return {};
    }
    if (c == 0x1b) {
      char seq[2] = {0, 0};
      if (::read(STDIN_FILENO, &seq[0], 1) == 1 && ::read(STDIN_FILENO, &seq[1], 1) == 1) {
        if (seq[0] == '[' && seq[1] == 'A') cursor = (cursor + int(items.size()) - 1) % int(items.size());
        if (seq[0] == '[' && seq[1] == 'B') cursor = (cursor + 1) % int(items.size());
      }
      continue;
    }
    if (c == ' ') sel[cursor] = !sel[cursor];
  }
  term.disable();
  std::printf("\n");
  return sel;
}

}  // namespace

int run_setup() {
  const std::string td = toolchain_dir();
  std::error_code ec;
  fs::create_directories(td, ec);
  std::cout << "JELLL V5 セットアップ (Linux)" << std::endl;
  std::cout << "インストール先: " << td << std::endl;

  if (!install_esbuild(td)) return 1;

  std::vector<std::string> items = {"C/C++ (Emscripten)", "Rust (wasm32)", "Zig"};
  std::vector<bool> sel = multiselect(items);
  if (sel.empty()) return 1;

  bool ok = true;
  if (sel.size() > 0 && sel[0]) ok = install_emscripten(td) && ok;
  if (sel.size() > 1 && sel[1]) ok = install_rust(td) && ok;
  if (sel.size() > 2 && sel[2]) ok = install_zig(td) && ok;
  ok = register_path(td) && ok;
  std::cout << (ok ? "セットアップが完了しました。" : "セットアップ中にエラーが発生しました。") << std::endl;
  return ok ? 0 : 1;
}

}  // namespace jelll
