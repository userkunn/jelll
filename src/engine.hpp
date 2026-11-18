#pragma once
// Build pipeline and toolchain discovery.
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace jelll {

struct Tools {
  std::string emcc;
  std::string esbuild;
  std::string rustc;
  std::string zig;
  std::map<std::string, std::string> rust_env;  // RUSTUP_HOME/CARGO_HOME when rustc is toolchain-managed
};

// Directory that holds setup-installed toolchains.
std::string toolchain_dir();

// Discover one tool. Throws BuildError ("<tool> not found in PATH") if missing.
std::string require_tool(const std::string& name);
Tools discover_tools(bool need_rust, bool need_zig);

struct BuildOutput {
  std::vector<std::string> deps;  // watched source files (canonical paths)
  uint64_t id = 0;
};

// Full build: preprocess -> parse -> sync -> compile -> link -> bundle -> html.
// Throws BuildError on any failure. Project layout is derived from source_path.
// threads=true builds shared memory + pthreads (needs COOP/COEP hosting).
BuildOutput build_project(const std::string& source_path, bool dev_mode, bool threads = false);

}  // namespace jelll
