#pragma once
// JELLL source scanning: call expansion, comment/string-aware block extraction.
#include <string>
#include <vector>

namespace jelll {

struct Block {
  std::string lang;   // "c", "rs", "zig", "js", "ts", "tsx", "css", "html"
  std::string body;   // text between the outermost { }
  std::string file;   // source file the block was found in
  int line = 0;       // 1-based line of "lang.X"
};

struct Program {
  std::vector<Block> blocks;
  std::vector<std::string> deps;  // all files read (root + call targets)
};

// Parse root .jelll file with recursive call expansion. Throws BuildError on
// malformed input ("Circular call detected", "Failed to read call target", ...).
Program parse_program(const std::string& root_path);

// Mask comments and string literals to spaces (newlines preserved) for a given
// language tag ("c", "rs", "zig", "js", "ts", "tsx", "css", "html").
std::string mask_source(const std::string& text, const std::string& lang);

}  // namespace jelll
