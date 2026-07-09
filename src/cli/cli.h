#pragma once
#include "../parse/parser.h"
#include "../vm/vm.h"
#include <string>
#include <vector>
#include <unordered_map>

namespace novac::cli {

// ── shared CLI context (passed to Nova as `Cli` global) ──────────────────────
struct CliArgs {
    std::vector<std::string>                     positional;
    std::unordered_map<std::string, std::string> options;    // --key value / --key=value
    std::unordered_map<std::string, bool>        flags;      // --key (no value) / -k
    std::unordered_map<std::string, bool>        addrs;      // @key
    std::unordered_map<std::string, bool>        additions;  // +key
    std::vector<std::string>                     raw;        // full argv
};

// ── command entry points ──────────────────────────────────────────────────────
int cmdDefault (const std::string& file, const std::vector<std::string>& args, CliArgs cli);
int cmdRun     (const std::string& file, const std::vector<std::string>& args, CliArgs cli);
int cmdRepl    ();
int cmdEval    (const std::string& code);
int cmdTokens  (const std::vector<std::string>& files);
int cmdAst     (const std::vector<std::string>& files);
int cmdTest    (const std::vector<std::string>& files);
int cmdNew     (const std::string& project, bool printCd);
int cmdNewKit  (const std::string& dirname);
int cmdConfig  (const std::string& action, const std::string& key, const std::vector<std::string>& value);
int cmdInitPath();
void printHelp   ();
void printVersion();

// ── helpers ───────────────────────────────────────────────────────────────────
std::string              readFileOrExit  (const std::string& path);
std::vector<std::string> expandFileArgs  (const std::vector<std::string>& args);
bool                     isGlobPattern   (const std::string& s);
std::vector<std::string> expandGlob      (const std::string& pattern);

// ANSI color helpers (no-op if NO_COLOR set)
std::string clr   (const std::string& code, const std::string& text);
std::string red   (const std::string& s);
std::string green (const std::string& s);
std::string yellow(const std::string& s);
std::string blue  (const std::string& s);
std::string gray  (const std::string& s);
std::string bold  (const std::string& s);

// JSON-ish dumps
void dumpTokensJSON(const std::string& src, const std::string& fname, bool multi);
void dumpAstJSON   (const std::string& src, const std::string& fname, bool multi);

// Run source with CLI context injected into globalScope
int runSource(const std::string& src, const std::string& fname, const CliArgs& cli);

} // namespace novac::cli