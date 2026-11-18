// JELLL V5 - JavaScript/TypeScript + Emscripten Bundled Compiler (Linux rewrite).
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <algorithm>
#include <map>
#include <optional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "engine.hpp"
#include "server.hpp"
#include "setup.hpp"
#include "util.hpp"

namespace fs = std::filesystem;

namespace {

constexpr const char* kVersion = "jelll 5.0 (Linux)";

void print_usage() {
  std::cout <<
      R"(JELLL V5 - JavaScript/TypeScript + Emscripten Bundled Compiler

使い方:
  jelll <file.jelll>          指定ファイルをビルド
  jelll dev <file.jelll>      ライブリロード付き開発サーバーを起動
  jelll --watch <file>        jelll dev のエイリアス
  jelll --threads <file>      共有メモリー+スレッド版をビルド (配信にCOOP/COEP要)
  jelll --setup               対話型ツールチェーンインストーラー
  jelll setup                 jelll --setup のエイリアス
  jelll --version             バージョン表示
  jelll --help                このヘルプを表示
)";
}

void print_error(const jelll::BuildError& e) {
  std::cerr << "jelll: error: " << e.what() << std::endl;
  if (!e.file.empty()) {
    std::cerr << "  at " << e.file;
    if (e.line > 0) std::cerr << ":" << e.line;
    std::cerr << std::endl;
  }
  if (!e.hint.empty()) std::cerr << "hint: " << e.hint << std::endl;
}

std::string dist_dir_for(const std::string& source) {
  fs::path src(source);
  fs::path dir = src.has_parent_path() ? src.parent_path() : fs::path(".");
  return (dir / "dist").string();
}

int run_build(const std::string& source, bool threads) {
  try {
    auto out = jelll::build_project(source, false, threads);
    std::cout << "build ok: " << dist_dir_for(source) << " (native.wasm, bundle.js, index.html)"
              << std::endl;
    return 0;
  } catch (const jelll::BuildError& e) {
    print_error(e);
    return 1;
  } catch (const std::exception& e) {
    std::cerr << "jelll: internal error: " << e.what() << std::endl;
    return 70;
  } catch (...) {
    std::cerr << "jelll: internal error (unknown)" << std::endl;
    return 70;
  }
}

int run_dev(const std::string& source, bool threads) {
  namespace j = jelll;
  std::shared_ptr<j::BuildOutput> last;
  try {
    auto out = j::build_project(source, true, threads);
    last = std::make_shared<j::BuildOutput>(std::move(out));
    std::cout << "initial build ok" << std::endl;
  } catch (const j::BuildError& e) {
    print_error(e);
    return 1;
  } catch (const std::exception& e) {
    std::cerr << "jelll: internal error: " << e.what() << std::endl;
    return 70;
  } catch (...) {
    std::cerr << "jelll: internal error (unknown)" << std::endl;
    return 70;
  }

  j::ServerStatus status;
  status.set_ok(last->id);
  std::string dist = dist_dir_for(source);
  if (!j::start_dev_server(dist, 8000, status, threads)) {
    std::cerr << "jelll: error: ポート8000をバインドできません" << std::endl;
    std::cerr << "hint: 既存のサーバーを停止してください" << std::endl;
    return 1;
  }
  std::cout << "dev server: http://127.0.0.1:8000 (Ctrl+C で終了)" << std::endl;

  // 500ms polling watcher over all source dependencies. Missing files are
  // observed as "absent" (not skipped), so a deleted dependency does NOT
  // retrigger a rebuild every tick - only a presence/mtime change does (S-08).
  std::map<std::string, std::optional<fs::file_time_type>> stamps;
  auto refresh = [&]() {
    stamps.clear();
    for (const auto& d : last->deps) {
      std::error_code ec;
      auto t = fs::last_write_time(d, ec);
      stamps[d] = ec ? std::nullopt : std::optional<fs::file_time_type>(t);
    }
  };
  refresh();
  int poll_ms = 500;
  while (true) {
    std::this_thread::sleep_for(std::chrono::milliseconds(poll_ms));
    bool changed = false;
    for (const auto& [path, t0] : stamps) {
      std::error_code ec;
      auto t1 = fs::last_write_time(path, ec);
      std::optional<fs::file_time_type> now =
          ec ? std::nullopt : std::optional<fs::file_time_type>(t1);
      if (now != t0) {
        changed = true;
        break;
      }
    }
    if (!changed) continue;
    // Debounce rapid writes (e.g. editors writing twice).
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    std::cout << "[jelll] change detected, rebuilding..." << std::endl;
    try {
      auto out = j::build_project(source, true, threads);
      last = std::make_shared<j::BuildOutput>(std::move(out));
      status.set_ok(last->id);
      poll_ms = 500;
      std::cout << "[jelll] rebuild ok" << std::endl;
    } catch (const j::BuildError& e) {
      status.set_error(e.what());
      print_error(e);
      // S-08: back off while the project stays broken (no 500ms CPU roast).
      poll_ms = std::min(poll_ms * 2, 5000);
      std::cout << "[jelll] ビルド失敗。前回の成果物を配信し続けます。" << std::endl;
    } catch (const std::exception& e) {
      status.set_error(std::string("internal error: ") + e.what());
      poll_ms = std::min(poll_ms * 2, 5000);
    } catch (...) {
      status.set_error("internal error (unknown)");
      poll_ms = std::min(poll_ms * 2, 5000);
    }
    refresh();
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::signal(SIGPIPE, SIG_IGN);
  std::vector<std::string> args;
  bool threads = false;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--threads") {
      threads = true;
      continue;
    }
    args.push_back(a);
  }
  if (args.empty()) {
    print_usage();
    return 2;
  }
  std::string arg1 = args[0];
  if (arg1 == "--help" || arg1 == "-h") {
    print_usage();
    return 0;
  }
  if (arg1 == "--version" || arg1 == "-v") {
    std::cout << kVersion << std::endl;
    return 0;
  }
  if (arg1 == "--setup" || arg1 == "setup") {
    return jelll::run_setup();
  }
  if (arg1 == "dev" || arg1 == "--watch") {
    if (args.size() < 2) {
      std::cerr << "jelll: error: ビルド対象ファイルが指定されていません" << std::endl;
      print_usage();
      return 2;
    }
    try {
      return run_dev(args[1], threads);
    } catch (const std::exception& e) {
      std::cerr << "jelll: internal error: " << e.what() << std::endl;
      return 70;
    } catch (...) {
      std::cerr << "jelll: internal error (unknown)" << std::endl;
      return 70;
    }
  }
  if (!arg1.empty() && arg1[0] == '-') {
    std::cerr << "jelll: error: 不明なオプションです: " << arg1 << std::endl;
    print_usage();
    return 2;
  }
  try {
    return run_build(arg1, threads);
  } catch (const std::exception& e) {
    std::cerr << "jelll: internal error: " << e.what() << std::endl;
    return 70;
  } catch (...) {
    std::cerr << "jelll: internal error (unknown)" << std::endl;
    return 70;
  }
}
