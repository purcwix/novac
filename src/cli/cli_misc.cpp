#include "cli.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <cstdlib>
#include <regex>

#if defined(_WIN32)
#  include <windows.h>
#endif

namespace fs = std::filesystem;

namespace novac::cli {

// ════════════════════════════════════════════════════════════════════════════
//  Tiny JSON value model — just enough for nova.config.json
//  Reuses Nova's own parser/executor since JSON is a subset of Nova object
//  literal syntax (quoted keys, true/false/null, numbers, strings, arrays).
// ════════════════════════════════════════════════════════════════════════════

static Val jsonParse(const std::string& src) {
    if (src.empty()) return nova_obj();
    std::string wrapped = "(" + src + ")";
    try {
        Parser p(wrapped, "<json>");
        auto ast = p.parse();
        if (ast->body.empty()) return nova_obj();
        Executor exe(wrapped, "<json>");
        const Node* exprNode = ast->body[0].get();
        if (exprNode->kind == Node::Kind::Exec && exprNode->value)
            exprNode = exprNode->value.get();
        return exe.evaluate(exprNode, exe.globalScope);
    } catch (...) {
        return nova_obj();
    }
}

static std::string jsonStringify(Val v, int indent = 2) {
    Executor exe("", "<json>");
    return exe.stringify(v, indent);
}

static Val jsonGet(Val root, const std::string& dottedKey) {
    if (dottedKey.empty()) return root;
    Val cur = root;
    std::stringstream ss(dottedKey);
    std::string seg;
    while (std::getline(ss, seg, '.')) {
        if (!cur || !cur.isObject()) return nullptr;
        cur = cur->obj->get(seg);
        if (!cur) return nullptr;
    }
    return cur;
}

static void jsonSet(Val root, const std::string& dottedKey, Val value) {
    if (!root || !root.isObject()) return;
    std::vector<std::string> segs;
    std::stringstream ss(dottedKey);
    std::string seg;
    while (std::getline(ss, seg, '.')) segs.push_back(seg);
    if (segs.empty()) return;

    Val cur = root;
    for (size_t i = 0; i + 1 < segs.size(); i++) {
        Val next = cur->obj->get(segs[i]);
        if (!next || !next.isObject()) {
            next = nova_obj();
            cur->obj->set(segs[i], next);
        }
        cur = next;
    }
    cur->obj->set(segs.back(), value);
}

static fs::path configFilePath() {
    return fs::path("nova.config.json");
}

static Val loadConfig() {
    auto path = configFilePath();
    if (!fs::exists(path)) return nova_obj();
    std::ifstream f(path);
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return jsonParse(content);
}

static void saveConfig(Val config) {
    std::ofstream f(configFilePath());
    f << jsonStringify(config, 2) << "\n";
}

// ════════════════════════════════════════════════════════════════════════════
//  novac new <project>
// ════════════════════════════════════════════════════════════════════════════

int cmdNew(const std::string& project, bool printCd) {
    fs::path projectPath = fs::absolute(project);
    if (fs::exists(projectPath)) {
        std::cerr << red("Directory already exists: " + projectPath.string()) << "\n";
        return 1;
    }

    fs::create_directories(projectPath / "src");
    fs::create_directories(projectPath / "bin");
    fs::create_directories(projectPath / "nova_modules");

    {
        std::ofstream f(projectPath / "src" / "main.nova");
        f << "// Welcome to Nova!\n\nprint(\"Hello, Nova!\");\n";
    }
    {
        std::ofstream f(projectPath / "bin" / (project + ".nv"));
        f << "";
    }
    {
        std::ofstream f(projectPath / "README.md");
        f << "# " << project << "\n\nThis is a Nova project.\n";
    }
    {
        std::ofstream f(projectPath / ".gitignore");
        f << "nova_modules/\n";
    }

    auto config = nova_obj();
    config->obj->set("name",        nova_str(project));
    config->obj->set("version",     nova_str("1.0.0"));
    config->obj->set("description", nova_str(""));
    config->obj->set("author",      nova_str(""));
    config->obj->set("license",     nova_str("MIT"));
    config->obj->set("main",        nova_str("src/main.nova"));
    config->obj->set("srcDir",      nova_str("src"));

    auto scripts = nova_obj();
    scripts->obj->set("run", nova_str("novac src/main.nova"));
    config->obj->set("scripts", scripts);
    config->obj->set("dependencies",    nova_obj());
    config->obj->set("devDependencies", nova_obj());

    {
        std::ofstream f(projectPath / "nova.config.json");
        f << jsonStringify(config, 2) << "\n";
    }

    std::cout << green("✓ Project created: " + projectPath.string()) << "\n";
    std::cout << gray("  src/main.nova    — entry point") << "\n";
    std::cout << gray("  nova.config.json — project config") << "\n";

    if (printCd) {
        std::cout << blue("\nRun: cd " + project) << "\n";
    } else {
        std::cout << blue("\nGet started:\n  cd " + project + "\n  novac src/main.nova") << "\n";
    }
    return 0;
}

// ════════════════════════════════════════════════════════════════════════════
//  novac new-kit <dirname>
// ════════════════════════════════════════════════════════════════════════════

int cmdNewKit(const std::string& dirname) {
    fs::path kitPath = fs::absolute(dirname);
    if (fs::exists(kitPath)) {
        std::cerr << red("Directory already exists: " + kitPath.string()) << "\n";
        return 1;
    }
    fs::create_directories(kitPath);

    {
        std::ofstream f(kitPath / "kitdef.h");
        f << "#pragma once\n"
          << "#include \"../../src/runtime/value.h\"\n"
          << "#include \"../../src/vm/vm.h\"\n\n"
          << "// " << dirname << " — Novac kit\n"
          << "// Register all kit functions in registerKit().\n"
          << "// After registration this is accessible from Novac via:\n"
          << "//   import_kit " << dirname << "\n"
          << "namespace novac::kit_" << dirname << " {\n\n"
          << "inline void registerKit(Executor& exe) {\n"
          << "    auto kit = nova_obj();\n\n"
          << "    kit->obj->set(\"hello\", NovaValue::makeNative([](ValVec args, auto) -> Val {\n"
          << "        return nova_str(\"Hello from " << dirname << "!\");\n"
          << "    }, \"hello\"));\n\n"
          << "    // TODO: implement your kit API here\n\n"
          << "    exe.globalScope->setOwn(\"__kit_" << dirname << "__\", kit);\n"
          << "}\n\n"
          << "} // namespace novac::kit_" << dirname << "\n";
    }
    {
        std::ofstream f(kitPath / "index.nova");
        f << "// " << dirname << " kit — Nova wrapper\n"
          << "// Import this from Nova: import_kit " << dirname << "\n"
          << "// Then call: " << dirname << ".hello()\n";
    }
    {
        auto manifest = nova_obj();
        manifest->obj->set("name",        nova_str(dirname));
        manifest->obj->set("version",     nova_str("1.0.0"));
        manifest->obj->set("description", nova_str(""));
        manifest->obj->set("main",        nova_str("kitdef.h"));
        manifest->obj->set("author",      nova_str(""));
        manifest->obj->set("license",     nova_str("MIT"));
        std::ofstream f(kitPath / "nova.kit.json");
        f << jsonStringify(manifest, 2) << "\n";
    }
    {
        std::ofstream f(kitPath / "README.md");
        f << "# " << dirname << "\n\nA Novac kit (native C++).\n\n"
          << "## Usage\n\n```nova\nimport_kit " << dirname << "\n" << dirname << ".hello()\n```\n\n"
          << "## Implementation\n\nEdit `kitdef.h` to implement your API as native C++ functions "
          << "registered via `NovaValue::makeNative(...)`. Call `registerKit(exe)` from your "
          << "CLI/VM setup (or add a `CMakeLists.txt` for auto-discovery).\n";
    }

    std::cout << green("Kit scaffolded at " + kitPath.string()) << "\n";
    std::cout << blue("  Edit kitdef.h to implement your API.") << "\n";
    std::cout << blue("  Update index.nova and README.md as needed.") << "\n";
    return 0;
}

// ════════════════════════════════════════════════════════════════════════════
//  novac config get|set|init [key] [value...]
// ════════════════════════════════════════════════════════════════════════════

int cmdConfig(const std::string& action, const std::string& key, const std::vector<std::string>& value) {
    try {
        if (action == "get") {
            Val config = loadConfig();
            if (key.empty()) {
                std::cout << jsonStringify(config, 2) << "\n";
            } else {
                Val v = jsonGet(config, key);
                if (!v) {
                    std::cout << yellow("Config key \"" + key + "\" not found") << "\n";
                } else if (v.isObject() || v.isArray()) {
                    std::cout << jsonStringify(v, 2) << "\n";
                } else {
                    std::cout << v.asString() << "\n";
                }
            }
            return 0;
        }
        if (action == "set") {
            if (key.empty() || value.empty()) {
                std::cerr << red("Usage: novac config set <key> <value>") << "\n";
                return 1;
            }
            std::string valueStr;
            for (size_t i = 0; i < value.size(); i++) {
                if (i) valueStr += " ";
                valueStr += value[i];
            }
            Val config = loadConfig();

            Val parsed;
            if (valueStr == "true")  parsed = nova_bool(true);
            else if (valueStr == "false") parsed = nova_bool(false);
            else if (valueStr == "null")  parsed = nova_null();
            else {
                try {
                    size_t pos;
                    double d = std::stod(valueStr, &pos);
                    if (pos == valueStr.size()) parsed = nova_num(d);
                } catch (...) {}
                if (!parsed) parsed = nova_str(valueStr);
            }

            jsonSet(config, key, parsed);
            saveConfig(config);
            std::cout << green("Config updated: " + key + " = " + valueStr) << "\n";
            return 0;
        }
        if (action == "init") {
            std::cout << "Initializing Nova configuration...\n\n";
            if (fs::exists(configFilePath())) {
                std::cerr << yellow("nova.config.json already exists.") << "\n";
                return 0;
            }
            auto config = nova_obj();
            config->obj->set("name",        nova_str(fs::current_path().filename().string()));
            config->obj->set("version",     nova_str("1.0.0"));
            config->obj->set("description", nova_str(""));
            config->obj->set("author",      nova_str(""));
            config->obj->set("license",     nova_str("MIT"));
            config->obj->set("main",        nova_str("src/main.nova"));
            config->obj->set("srcDir",      nova_str("src"));
            auto scripts = nova_obj();
            scripts->obj->set("run", nova_str("novac src/main.nova"));
            config->obj->set("scripts",         scripts);
            config->obj->set("dependencies",    nova_obj());
            config->obj->set("devDependencies", nova_obj());
            saveConfig(config);
            std::cout << green("Created nova.config.json") << "\n";
            return 0;
        }

        std::cerr << red("Unknown action: " + action + ". Use \"get\", \"set\", or \"init\".") << "\n";
        return 1;
    } catch (std::exception& e) {
        std::cerr << red(std::string("Config error: ") + e.what()) << "\n";
        return 1;
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  novac init PATH
// ════════════════════════════════════════════════════════════════════════════

int cmdInitPath() {
    fs::path binPath = fs::current_path();
#if defined(_WIN32)
    {
        char buf[MAX_PATH];
        if (GetModuleFileNameA(NULL, buf, MAX_PATH))
            binPath = fs::path(buf).parent_path();
    }
#else
    {
        char buf[4096];
        ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf)-1);
        if (len > 0) { buf[len] = 0; binPath = fs::path(buf).parent_path(); }
    }
#endif

    std::cout << blue("Adding " + binPath.string() + " to PATH...") << "\n";

#if defined(_WIN32)
    std::string escaped = binPath.string();
    std::string psPath;
    for (char c : escaped) {
        if (c == '\\') psPath += "\\\\";
        else if (c == '\'') psPath += "''";
        else psPath += c;
    }

    std::ostringstream ps;
    ps << "$regPath = 'HKCU:\\Environment'; "
       << "$current = (Get-ItemProperty -Path $regPath -Name PATH -ErrorAction SilentlyContinue).PATH; "
       << "if (-not $current) { $current = '' }; "
       << "$add = '" << psPath << "'; "
       << "$parts = $current -split ';' | Where-Object { $_ -ne '' }; "
       << "if ($parts -contains $add) { "
       << "  Write-Host 'Already in PATH -- nothing changed.' "
       << "} else { "
       << "  $newPath = ($parts + $add) -join ';'; "
       << "  [Environment]::SetEnvironmentVariable('PATH', $newPath, 'User'); "
       << "  Write-Host 'PATH updated successfully.' "
       << "}";

    std::string cmd = "powershell -NoProfile -Command \"" + ps.str() + "\"";
    int rc = std::system(cmd.c_str());
    if (rc != 0) {
        std::cerr << red("Failed to update PATH via PowerShell.") << "\n";
        return 1;
    }
    std::cout << green("Done. Open a NEW terminal for the PATH change to take effect.") << "\n";
    std::cout << yellow("To apply in the current session run:") << "\n";
    std::cout << "  $env:PATH += ';" << binPath.string() << "'\n";
#else
    std::string home;
    if (const char* h = std::getenv("HOME")) home = h;
    fs::path shellRc;
#if defined(__APPLE__)
    shellRc = fs::path(home) / ".zshrc";
#else
    shellRc = fs::path(home) / ".bashrc";
#endif
    std::string exportCmd = "export PATH=\"$PATH:" + binPath.string() + "\"";
    std::ofstream f(shellRc, std::ios::app);
    if (!f.is_open()) {
        std::cerr << red("Failed to write to " + shellRc.string()) << "\n";
        return 1;
    }
    f << exportCmd << "\n";
    std::cout << green("Added to " + shellRc.string() + ". Run 'source "
              + shellRc.string() + "' or restart your shell.") << "\n";
#endif
    return 0;
}

// ════════════════════════════════════════════════════════════════════════════
//  Help / Version
// ════════════════════════════════════════════════════════════════════════════

void printVersion() {
    std::cout << "novac 0.1.0 (C++20)\n";
}

void printHelp() {
    std::cout << R"(
novac — Nova language interpreter and toolchain

Usage:
  novac [file] [args...]            Run a Nova file
  novac                             Start REPL
  novac run <file> [args...]        Run a Nova file (explicit)
  novac repl                        Start interactive REPL
  novac eval <code>                 Evaluate Nova code inline
  novac tokens <file|glob> ...      Print token stream (JSON)
  novac ast <file|glob> ...         Print AST (JSON)
  novac test <file|glob> ...        Check syntax of file(s)
  novac new <project> [--cd]        Create a new Nova project
  novac new-kit <dirname>           Scaffold a new native C++ kit
  novac config get [key]            Print config (or one key)
  novac config set <key> <value>    Set a config key
  novac config init                 Create nova.config.json
  novac init PATH                   Add novac to your PATH

Package management (nv+):
  nv+ install <pkg>                 Install a package
  nv+ remove  <pkg>                 Remove a package
  nv+ list                          List installed packages
  nv+ update                        Update all packages
  nv+ login                         Save GitHub token for publishing
  nv+ publish                       Publish package to nova-registry
  nv+ --help                        Full nv+ help

Options:
  -e, --eval <code>   Evaluate code string
  --tokens            Print token stream then exit (with file/eval)
  --ast               Print AST then exit (with file/eval)
  --repl              Force REPL mode
  -h, --help          Show this help
  -v, --version       Show version
)";
}

} // namespace novac::cli