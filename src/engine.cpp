#include "engine.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <sstream>

#include "codegen.hpp"
#include "scanner.hpp"
#include "util.hpp"

namespace jelll {

namespace fs = std::filesystem;

std::string toolchain_dir() {
  if (const char* env = std::getenv("JELLL_TOOLCHAIN_DIR")) {
    if (*env) return env;
  }
  // No env var needed: honor whichever default location actually exists.
  // tests/install_toolchains.sh installs to ~/.cache/jelll-tools, `jelll
  // --setup` installs to ~/.local/share/jelll/tools.
  const char* home = std::getenv("HOME");
  std::string h = home ? home : "";
  std::error_code ec;
  for (const std::string& cand :
       {h + "/.local/share/jelll/tools", h + "/.cache/jelll-tools"}) {
    if (!cand.empty() && std::filesystem::is_directory(cand, ec)) return cand;
  }
  return h + "/.local/share/jelll/tools";
}

namespace {

std::optional<std::string> first_existing(const std::vector<std::string>& candidates) {
  for (const auto& c : candidates) {
    if (!c.empty() && ::access(c.c_str(), X_OK) == 0 && safe_exec_owner(c)) return c;
  }
  return std::nullopt;
}

// S-03: a toolchain directory must be owned by root or the user and must not
// be group/other-writable (otherwise JELLL_TOOLCHAIN_DIR becomes an RCE knob).
void check_toolchain_dir(const std::string& td) {
  struct stat st {};
  if (::stat(td.c_str(), &st) != 0) return;  // absent: tools will be "not found"
  if (!S_ISDIR(st.st_mode)) return;
  uid_t me = ::getuid();
  if ((st.st_uid != 0 && st.st_uid != me) || (st.st_mode & (S_IWGRP | S_IWOTH))) {
    throw BuildError("Unsafe toolchain directory",
                     "所有者/権限が不正なツールチェーンディレクトリは使用できません: " + td);
  }
}

std::string tool_not_found(const std::string& name) {
  if (name == "emcc") return "emcc not found in PATH";
  if (name == "esbuild") return "esbuild not found in PATH";
  if (name == "rustc") return "rustc not found in PATH";
  if (name == "zig") return "zig not found in PATH";
  return name + " not found in PATH";
}

std::string tool_hint(const std::string& name) {
  if (name == "emcc") return "`jelll --setup` を実行するか Emscripten を手動インストールしてください";
  if (name == "esbuild") return "npm i -g esbuild を実行してください";
  if (name == "rustc") return "`jelll --setup` を実行するか Rust (wasm32) をインストールしてください";
  return "`jelll --setup` を実行するか Zig をインストールしてください";
}

}  // namespace

std::string require_tool(const std::string& name) {
  const std::string td = toolchain_dir();
  check_toolchain_dir(td);
  std::vector<std::string> candidates;
  if (auto p = find_in_path(name)) return *p;
  const char* home = std::getenv("HOME");
  std::string home_dir = home ? home : "";
  if (name == "emcc") {
    candidates = {td + "/emsdk/upstream/emscripten/emcc", td + "/emscripten/emcc",
                  home_dir + "/.local/share/jelll/tools/emsdk/upstream/emscripten/emcc",
                  home_dir + "/.cache/jelll-tools/emsdk/upstream/emscripten/emcc",
                  home_dir + "/emsdk/upstream/emscripten/emcc"};
  } else if (name == "esbuild") {
    candidates = {td + "/bin/esbuild", td + "/npm/node_modules/.bin/esbuild",
                  home_dir + "/.local/share/jelll/tools/bin/esbuild",
                  home_dir + "/.cache/jelll-tools/bin/esbuild",
                  home_dir + "/.local/share/jelll/tools/npm/node_modules/.bin/esbuild",
                  home_dir + "/.cache/jelll-tools/npm/node_modules/.bin/esbuild",
                  "./node_modules/.bin/esbuild", "node_modules/.bin/esbuild"};
  } else if (name == "rustc") {
    candidates = {td + "/cargo/bin/rustc", home_dir + "/.local/share/jelll/tools/cargo/bin/rustc",
                     home_dir + "/.cache/jelll-tools/cargo/bin/rustc"};
  } else if (name == "zig") {
    candidates = {td + "/zig/zig", home_dir + "/.local/share/jelll/tools/zig/zig",
                     home_dir + "/.cache/jelll-tools/zig/zig"};
  }
  if (auto found = first_existing(candidates)) return *found;
  throw BuildError(tool_not_found(name), tool_hint(name));
}

Tools discover_tools(bool need_rust, bool need_zig) {
  Tools t;
  t.emcc = require_tool("emcc");
  t.esbuild = require_tool("esbuild");
  if (need_rust) {
    t.rustc = require_tool("rustc");
    // If rustc comes from a rustup-managed toolchain dir, provide its env.
    const std::string td = toolchain_dir();
    if (t.rustc.find("/cargo/bin/") != std::string::npos) {
      std::string root = t.rustc.substr(0, t.rustc.size() - std::string("cargo/bin/rustc").size());
      t.rust_env["RUSTUP_HOME"] = root + "rustup";
      t.rust_env["CARGO_HOME"] = root + "cargo";
    }
  }
  if (need_zig) t.zig = require_tool("zig");
  return t;
}

namespace {

std::string log_header(const std::vector<std::string>& argv) {
  std::string s = "+";
  for (const auto& a : argv) {
    s += " ";
    s += a;
  }
  return s + "\n";
}

void reset_log(const std::string& path) { write_file(path, ""); }

void append_log(const std::string& path, const std::string& text) {
  if (auto prev = read_file(path)) write_file(path, *prev + text);
  else write_file(path, text);
}

int run_logged(const std::vector<std::string>& argv, const std::string& log,
               const std::map<std::string, std::string>& env = {}) {
  append_log(log, log_header(argv));
  auto r = run_command(argv, log, env);
  if (!r.started) {
    append_log(log, "spawn failed: " + r.error + "\n");
    return -1;
  }
  return r.exit_code;
}

std::string compile_failure(const std::string& tool) {
  if (tool == "emcc") return "emcc build failed";
  if (tool == "esbuild") return "esbuild bundle failed";
  if (tool == "rustc") return "rustc build failed";
  return "zig build failed";
}

// ---- C-06: explicit export list -------------------------------------------
// `--export-all` leaked libc/Emscripten internals (_malloc, _memcpy, ...) to
// JS. Instead we collect the DEFINED C-linkage symbols from the compiled
// objects with llvm-nm (the compiler is the source of truth) and pass exactly
// those - plus the generated handle API - to -sEXPORTED_FUNCTIONS.

std::string find_llvm_nm(const std::string& emcc_path) {
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::path cand = fs::path(emcc_path).parent_path().parent_path() / "bin" / "llvm-nm";
  if (fs::exists(cand, ec)) return cand.string();
  if (auto p = find_in_path("llvm-nm")) return *p;
  return "";
}

bool symbol_excluded(const std::string& name, const std::set<std::string>& exact,
                     const std::vector<std::string>& prefixes) {
  if (exact.count(name)) return true;
  for (const auto& p : prefixes) {
    if (name.rfind(p, 0) == 0) return true;
  }
  return false;
}

std::set<std::string> collect_user_exports(const std::vector<std::string>& objects,
                                           const std::vector<SyncStruct>& structs,
                                           const std::string& nm,
                                           const std::string& bdir) {
  // Symbols that must never become JS-visible even if defined in the objects.
  static const std::set<std::string> kRuntime = {
      "main", "_initialize", "malloc", "free", "calloc", "realloc", "memcpy", "memmove",
      "memset", "memcmp", "memchr", "strlen", "strcmp", "strncmp", "strcpy", "strncpy",
      "strcat", "strncat", "strchr", "strrchr", "strstr", "strdup", "sprintf", "snprintf",
      "printf", "fprintf", "vsnprintf", "vprintf", "puts", "putchar", "atoi", "atof",
      "strtol", "strtoul", "abs", "labs", "qsort", "bsearch", "rand", "srand", "exit",
      "abort", "atexit", "getenv", "time", "isdigit", "isspace", "isalpha", "isalnum",
      "toupper", "tolower", "round", "floor", "ceil", "trunc"};
  std::vector<std::string> prefixes = {"__jelll", "__wasm", "__cxx", "_Z", "__gnu",
                                       "__rust", "__rdl", "__rustc", "_Unwind", "emscripten_",
                                       "_emscripten_"};
  // Raw generated API and user method bodies take raw pointers: not exported.
  std::set<std::string> exact = kRuntime;
  for (const auto& s : structs) {
    exact.insert(s.name + "_alloc");
    exact.insert(s.name + "_free");
    exact.insert(s.name + "_live_count");
    exact.insert(s.name + "_slot_count");
    prefixes.push_back(s.name + "_get_");
    prefixes.push_back(s.name + "_set_");
    for (const auto& m : s.methods) exact.insert(m.c_func);
  }
  std::set<std::string> out;
  for (const auto& obj : objects) {
    std::string log = bdir + "/nm.log";
    reset_log(log);  // never accumulate symbols from previous nm invocations
    int rc = run_logged({nm, "--defined-only", obj}, log);
    // R-08: if nm itself fails the export list would silently lose user
    // functions - that must break the build instead of shipping a broken WASM.
    if (rc != 0) {
      throw BuildError("llvm-nm failed",
                       "エクスポート列挙に失敗しました: " + nm + " " + obj + " (" +
                           std::to_string(rc) + ")。" + log + " を参照");
    }
    auto content = read_file(log);
    if (!content) {
      throw BuildError("llvm-nm failed",
                       "エクスポート列挙の出力を読めません: " + log);
    }
    std::istringstream is(*content);
    std::string line;
    while (std::getline(is, line)) {
      // "<addr> <type> <name>" (global types: T/W only)
      size_t p1 = line.find(' ');
      if (p1 == std::string::npos) continue;
      size_t p2 = line.find_first_not_of(' ', p1);
      if (p2 == std::string::npos) continue;
      size_t p3 = line.find(' ', p2);
      std::string typ = line.substr(p2, p3 == std::string::npos ? std::string::npos : p3 - p2);
      if (typ.size() != 1 || (typ[0] != 'T' && typ[0] != 'W')) continue;
      size_t p4 = line.find_first_not_of(' ', p2 + 1);
      if (p4 == std::string::npos) continue;
      std::string name = line.substr(p4);
      while (!name.empty() && (name.back() == '\r' || name.back() == ' ')) name.pop_back();
      if (name.empty()) continue;
      if (symbol_excluded(name, exact, prefixes)) continue;
      out.insert(name);
    }
  }
  return out;
}

// S-06: one build at a time per project directory.
struct BuildLock {
  int fd = -1;
  explicit BuildLock(const std::string& bdir) {
    fd = ::open((bdir + "/.jelll-lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if (fd < 0 || ::flock(fd, LOCK_EX | LOCK_NB) != 0) {
      if (fd >= 0) ::close(fd);
      throw BuildError("Another jelll build is running",
                       "同じプロジェクトへの同時ビルドはできません: " + bdir);
    }
  }
  ~BuildLock() {
    if (fd >= 0) {
      ::flock(fd, LOCK_UN);
      ::close(fd);
    }
  }
  BuildLock(const BuildLock&) = delete;
  BuildLock& operator=(const BuildLock&) = delete;
};

}  // namespace

BuildOutput build_project(const std::string& source_path, bool dev_mode, bool threads) {
  // 1. Extension check.
  std::string ext;
  size_t dot = source_path.find_last_of('.');
  if (dot != std::string::npos) ext = source_path.substr(dot);
  if (ext != ".jelll" && ext != ".jsbc" && ext != ".tsbc" && ext != ".tsxbc") {
    throw BuildError("Unsupported file extension",
                     ".jelll / .jsbc / .tsbc / .tsxbc を使用してください: " + source_path);
  }

  // 2. Preprocess (call expansion) + parse blocks.
  Program program = parse_program(source_path);

  // 3. Language inventory.
  std::vector<const Block*> c_blocks, rs_blocks, zig_blocks, script_blocks, sync_blocks;
  bool has_css = false, has_html = false;
  bool has_tsx = false, has_ts = false;
  for (const auto& b : program.blocks) {
    if (b.lang == "c") c_blocks.push_back(&b);
    if (b.lang == "sync") sync_blocks.push_back(&b);
    else if (b.lang == "rs") rs_blocks.push_back(&b);
    else if (b.lang == "zig") zig_blocks.push_back(&b);
    else if (b.lang == "js" || b.lang == "ts" || b.lang == "tsx") {
      script_blocks.push_back(&b);
      if (b.lang == "tsx") has_tsx = true;
      if (b.lang == "ts") has_ts = true;
    } else if (b.lang == "css") has_css = true;
    else if (b.lang == "html") has_html = true;
  }
  (void)has_css;
  (void)has_html;

  // 4. Required sections.
  if (c_blocks.empty() && rs_blocks.empty() && zig_blocks.empty() && sync_blocks.empty()) {
    throw BuildError("No lang.c section found",
                     "ネイティブブロック (lang.c / lang.rs / lang.zig) が1つもありません");
  }
  if (script_blocks.empty()) {
    throw BuildError("No script section found",
                     "スクリプトブロック (lang.js / lang.ts / lang.tsx) を追加してください");
  }
  if (threads && (!rs_blocks.empty() || !zig_blocks.empty())) {
    throw BuildError("Unsupported language in --threads build",
                     "Rust/Zig ブロックは共有メモリー (--threads) ビルドの WebAssembly feature "
                     "整合性を満たせないため対象外です。lang.c + lang.sync のみ使用してください");
  }

  // 5. Mode: TSX > TS > JS.
  ScriptMode::E mode = has_tsx ? ScriptMode::Tsx : (has_ts ? ScriptMode::Ts : ScriptMode::Js);

  // 6. lang.sync (IDL) parsing.
  std::vector<Block> owned_sync;
  owned_sync.reserve(sync_blocks.size());
  for (const auto* b : sync_blocks) owned_sync.push_back(*b);
  std::vector<SyncStruct> structs = parse_sync(owned_sync);

  // 7. Code generation.
  Generated gen = generate(program, structs, mode, dev_mode, threads);

  fs::path src(source_path);
  fs::path project_dir = src.has_parent_path() ? src.parent_path() : fs::path(".");
  fs::path build_dir = project_dir / ".bc_build";
  fs::path dist_dir = project_dir / "dist";
  std::error_code ec;
  fs::create_directories(build_dir, ec);
  fs::create_directories(dist_dir, ec);

  std::string bdir = build_dir.string();
  std::string ddir = dist_dir.string();

  BuildLock build_lock(bdir);  // S-06: no concurrent builds in one project

  auto write_or_die = [&](const std::string& path, const std::string& content) {
    if (!write_file(path, content)) {
      throw BuildError("Failed to write output", "書き込めません: " + path);
    }
  };
  // S-07: a silent write failure used to compile STALE sources.
  write_or_die(bdir + "/core.cpp", gen.core_cpp);
  if (!gen.core_rs.empty()) write_or_die(bdir + "/core.rs", gen.core_rs);
  if (!gen.core_zig.empty()) write_or_die(bdir + "/core.zig", gen.core_zig);
  write_or_die(bdir + "/" + gen.entry_name, gen.entry);

  // Tool discovery (emcc+esbuild always; rustc/zig only when used).
  Tools tools = discover_tools(!rs_blocks.empty(), !zig_blocks.empty());

  std::string emcc_log = bdir + "/emcc.stderr.log";
  std::string esbuild_log = bdir + "/esbuild.stderr.log";
  std::string rustc_log = bdir + "/rustc.stderr.log";
  std::string zig_log = bdir + "/zig.stderr.log";
  reset_log(emcc_log);
  reset_log(esbuild_log);

  // 8. Native compilation (each language to its own object file).
  std::vector<std::string> objects;
  std::vector<std::string> archives;
  if (!c_blocks.empty() || !structs.empty()) {
    // C-06: user code is compiled as C++ under -O2; disable strict aliasing so
    // classic type-punning (float<->u32 bit casts) behaves like C, and surface
    // warnings in emcc.stderr.log instead of staying silent.
    std::vector<std::string> argv = {tools.emcc, "-c", bdir + "/core.cpp", "-O2",
                                     "-fno-strict-aliasing", "-Wall", "-Wextra",
                                     "-Wno-unused-parameter"};
    if (threads) { argv.push_back("-pthread"); argv.push_back("-DJELLL_THREADS=1"); }
    argv.insert(argv.end(), {"-o", bdir + "/core.o"});
    int rc = run_logged(argv, emcc_log);
    if (rc != 0) {
      throw BuildError(compile_failure("emcc"), "Cコードを確認してください。" + emcc_log + " を参照");
    }
    objects.push_back(bdir + "/core.o");
  }
  if (!rs_blocks.empty()) {
    reset_log(rustc_log);
    // Emit both the crate object (so #[no_mangle] exports are retained) and a
    // staticlib archive (pulls in std on demand at link time).
    int rc = run_logged({tools.rustc, "--edition=2021", "--crate-name", "jelll_core",
                         "--crate-type=staticlib", "--emit=obj=" + bdir + "/core_rs.o,link=" + bdir + "/libcore_rs.a",
                         "-C", "opt-level=3", "-C", "panic=abort",
                         "--target", "wasm32-unknown-unknown", bdir + "/core.rs"},
                        rustc_log, tools.rust_env);
    if (rc != 0) {
      throw BuildError(compile_failure("rustc"), "Rustコードを確認してください。" + rustc_log + " を参照");
    }
    objects.push_back(bdir + "/core_rs.o");
    archives.push_back(bdir + "/libcore_rs.a");
  }
  if (!zig_blocks.empty()) {
    reset_log(zig_log);
    int rc = run_logged({tools.zig, "build-obj", "-target", "wasm32-freestanding", "-O", "ReleaseSafe",
                         "-fPIC", "-femit-bin=" + bdir + "/core_zig.o", bdir + "/core.zig"},
                        zig_log);
    if (rc != 0) {
      throw BuildError(compile_failure("zig"), "Zigコードを確認してください。" + zig_log + " を参照");
    }
    objects.push_back(bdir + "/core_zig.o");
  }

  // 9. WASM link into one native.wasm (+ Emscripten ES module glue).
  // Memory is growable: 32MB initial, up to 2GB. No fixed-size cap.
  {
    // C-06: explicit export list = generated handle API + user C-linkage
    // functions discovered via llvm-nm from the compiled objects.
    std::vector<std::string> exports;
    for (const auto& s : structs) {
      exports.push_back(s.name + "_alloc_h");
      exports.push_back(s.name + "_free_h");
      exports.push_back(s.name + "_live_count");
      exports.push_back(s.name + "_slot_count");
      for (const auto& f : s.fields) {
        exports.push_back(s.name + "_get_" + f.name + "_h");
        exports.push_back(s.name + "_set_" + f.name + "_h");
      }
      for (const auto& m : s.methods) exports.push_back(m.c_func + "_h");
    }
    std::string nm = find_llvm_nm(tools.emcc);
    if (nm.empty()) {
      throw BuildError("llvm-nm not found",
                       "エクスポート列挙に llvm-nm が必要です。emsdk の upstream/bin を確認してください");
    }
    for (const auto& name : collect_user_exports(objects, structs, nm, bdir)) {
      exports.push_back(name);
    }
    std::string exported = "[";
    for (size_t k = 0; k < exports.size(); ++k) {
      if (k) exported += ",";
      exported += "_" + exports[k];
    }
    exported += "]";

    std::vector<std::string> argv = {tools.emcc};
    argv.insert(argv.end(), objects.begin(), objects.end());
    argv.insert(argv.end(), archives.begin(), archives.end());
    if (threads) {
      argv.push_back("-pthread");
      argv.push_back("-sPTHREAD_POOL_SIZE=4");
    }
    argv.insert(argv.end(),
                // Generated code uses C++ runtime (placement new, std::mutex in
                // --threads builds), so make emcc link libc++ like em++ does.
                {"-sDEFAULT_TO_CXX=1",
                 "-O2", "--no-entry", "-fno-strict-aliasing",
                 "-sALLOW_MEMORY_GROWTH=1", "-sINITIAL_MEMORY=32MB", "-sMAXIMUM_MEMORY=2GB",
                 "-sMODULARIZE=1", "-sEXPORT_ES6=1", "-sEXPORT_NAME=createJelllModule",
                 "-sENVIRONMENT=web", "-sSTACK_SIZE=4MB",
                 "-sEXPORTED_FUNCTIONS=" + exported});
    if (dev_mode) {
      // C-06: memory-safety debug aids in dev builds only.
      argv.push_back("-sASSERTIONS=2");
      argv.push_back("-sSTACK_OVERFLOW_CHECK=2");
    }
    argv.insert(argv.end(), {"-o", bdir + "/native.mjs"});
    int rc = run_logged(argv, emcc_log);
    if (rc != 0) {
      throw BuildError(compile_failure("emcc"), "リンクエラーです。" + emcc_log + " を参照");
    }
  }

  // 10. JS/TS bundle via esbuild.
  {
    int rc = run_logged({tools.esbuild, bdir + "/" + gen.entry_name, "--bundle", "--minify",
                         "--format=esm", "--target=es2022", "--external:./native.mjs",
                         "--outfile=" + bdir + "/bundle.js"},
                        esbuild_log);
    if (rc != 0) {
      throw BuildError(compile_failure("esbuild"), "スクリプトを確認してください。" + esbuild_log + " を参照");
    }
  }

  // 11. Publish dist/ atomically (S-07: copy_file + rename; no whole-file
  // buffering of native.wasm in RAM).
  auto copy_atomic = [&](const std::string& from, const std::string& to) {
    std::error_code ec2;
    std::string tmp = to + ".jelll-tmp";
    fs::copy_file(from, tmp, fs::copy_options::overwrite_existing, ec2);
    if (ec2) {
      throw BuildError("Failed to write output", "出力先に書き込めません: " + to);
    }
    fs::rename(tmp, to, ec2);
    if (ec2) {
      fs::remove(tmp, ec2);
      throw BuildError("Failed to write output", "出力先に書き込めません: " + to);
    }
  };
  copy_atomic(bdir + "/native.mjs", ddir + "/native.mjs");
  copy_atomic(bdir + "/native.wasm", ddir + "/native.wasm");
  copy_atomic(bdir + "/bundle.js", ddir + "/bundle.js");
  if (!write_file_atomic(ddir + "/index.html", gen.html)) {
    throw BuildError("Failed to write output", "出力先に書き込めません: " + ddir + "/index.html");
  }

  BuildOutput out;
  out.deps = program.deps;
  out.id = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
  return out;
}

}  // namespace jelll
