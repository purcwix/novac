#include "../src/cli/cli.h"
#include <iostream>
#include <vector>
#include <string>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#if defined(_WIN32)
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#endif
using namespace novac;
using namespace novac::cli;
namespace fs = std::filesystem;

// ── arg parsing helper for `cli` global ──────────────────────────────────────

static void parseExtraArgs(const std::vector<std::string> &extra, CliArgs &cli)
{
    for (size_t i = 0; i < extra.size(); i++)
    {
        const std::string &arg = extra[i];
        if (arg.rfind("--", 0) == 0)
        {
            std::string key = arg.substr(2);
            auto eq = key.find('=');
            if (eq != std::string::npos)
            {
                cli.options[key.substr(0, eq)] = key.substr(eq + 1);
            }
            else if (i + 1 < extra.size() && extra[i + 1].rfind("-", 0) != 0)
            {
                cli.options[key] = extra[++i];
            }
            else
            {
                cli.flags[key] = true;
            }
        }
        else if (arg.rfind("-", 0) == 0 && arg.size() > 1)
        {
            cli.flags[arg.substr(1)] = true;
        }
        else if (arg.rfind("@", 0) == 0)
        {
            cli.addrs[arg.substr(1)] = true;
        }
        else if (arg.rfind("+", 0) == 0)
        {
            cli.additions[arg.substr(1)] = true;
        }
        else
        {
            cli.positional.push_back(arg);
        }
    }
}

int main(int argc, char *argv[])
{

#if defined(_WIN32)
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
    std::srand((unsigned)std::time(nullptr));
    std::srand((unsigned)std::time(nullptr));

    std::vector<std::string> args(argv + 1, argv + argc);

    CliArgs cli;
    for (int i = 0; i < argc; i++)
        cli.raw.push_back(argv[i]);

    if (args.empty())
    {
        return cmdRepl();
    }

    bool dumpTok = false;
    bool dumpAst_ = false;

    std::vector<std::string> filtered;
    std::string evalStr;
    bool hasEval = false;

    for (size_t i = 0; i < args.size(); i++)
    {
        const std::string &a = args[i];
        if (a == "--help" || a == "-h")
        {
            printHelp();
            return 0;
        }
        if (a == "--version" || a == "-v")
        {
            printVersion();
            return 0;
        }
        if (a == "--tokens")
        {
            dumpTok = true;
            continue;
        }
        if (a == "--ast")
        {
            dumpAst_ = true;
            continue;
        }
        if (a == "--repl")
        {
            continue;
        }
        if ((a == "-e" || a == "--eval") && i + 1 < args.size())
        {
            evalStr = args[++i];
            hasEval = true;
            continue;
        }
        filtered.push_back(a);
    }

    if (hasEval)
    {
        if (dumpTok)
        {
            dumpTokensJSON(evalStr, "<eval>", false);
            return 0;
        }
        if (dumpAst_)
        {
            dumpAstJSON(evalStr, "<eval>", false);
            return 0;
        }
        return cmdEval(evalStr);
    }

    if (filtered.empty())
        return cmdRepl();

    const std::string &sub = filtered[0];
    std::vector<std::string> rest(filtered.begin() + 1, filtered.end());

    // ── subcommands ──────────────────────────────────────────────────────────
    if (sub == "run")
    {
        if (rest.empty())
        {
            std::cerr << red("Usage: novac run <file> [args...]") << "\n";
            return 1;
        }
        std::string file = rest[0];
        std::vector<std::string> scriptArgs(rest.begin() + 1, rest.end());
        parseExtraArgs(scriptArgs, cli);
        if (dumpTok)
        {
            dumpTokensJSON(readFileOrExit(file), file, false);
            return 0;
        }
        if (dumpAst_)
        {
            dumpAstJSON(readFileOrExit(file), file, false);
            return 0;
        }
        return cmdRun(file, cli.positional, cli);
    }

    if (sub == "repl")
        return cmdRepl();

    if (sub == "eval")
    {
        if (rest.empty())
        {
            std::cerr << red("Usage: novac eval <code>") << "\n";
            return 1;
        }
        return cmdEval(rest[0]);
    }

    if (sub == "tokens")
    {
        if (rest.empty())
        {
            std::cerr << red("Usage: novac tokens <file|glob> ...") << "\n";
            return 1;
        }
        return cmdTokens(rest);
    }

    if (sub == "ast")
    {
        if (rest.empty())
        {
            std::cerr << red("Usage: novac ast <file|glob> ...") << "\n";
            return 1;
        }
        return cmdAst(rest);
    }

    if (sub == "test")
    {
        if (rest.empty())
        {
            std::cerr << red("Usage: novac test <file|glob> ...") << "\n";
            return 1;
        }
        return cmdTest(rest);
    }

    if (sub == "new")
    {
        if (rest.empty())
        {
            std::cerr << red("Usage: novac new <project> [--cd]") << "\n";
            return 1;
        }
        bool printCd = false;
        std::string project;
        for (auto &a : rest)
        {
            if (a == "--cd")
                printCd = true;
            else if (project.empty())
                project = a;
        }
        return cmdNew(project, printCd);
    }

    if (sub == "new-kit")
    {
        if (rest.empty())
        {
            std::cerr << red("Usage: novac new-kit <dirname>") << "\n";
            return 1;
        }
        return cmdNewKit(rest[0]);
    }

    if (sub == "config")
    {
        if (rest.empty())
        {
            std::cerr << red("Usage: novac config <get|set|init> [key] [value...]") << "\n";
            return 1;
        }
        std::string action = rest[0];
        std::string key = rest.size() > 1 ? rest[1] : "";
        std::vector<std::string> value;
        if (rest.size() > 2)
            value.assign(rest.begin() + 2, rest.end());
        return cmdConfig(action, key, value);
    }

    if (sub == "init")
    {
        if (rest.empty())
        {
            std::cerr << red("Usage: novac init PATH") << "\n";
            return 1;
        }
        if (rest[0] == "PATH" || rest[0] == "path")
            return cmdInitPath();
        std::cerr << red("Unknown init option: " + rest[0] + ". Use 'PATH'.") << "\n";
        return 1;
    }

    // ── package management hint ───────────────────────────────────────────────
    if (sub == "module" || sub == "install" || sub == "remove")
    {
        std::cerr << yellow("Package management has moved to nv+") << "\n";
        std::cerr << gray("  nv+ install <pkg>\n  nv+ remove <pkg>\n  nv+ list\n  nv+ --help\n");
        return 1;
    }

    // ── default: treat as file to run ────────────────────────────────────────
    std::string file = sub;
    std::vector<std::string> scriptArgs(filtered.begin() + 1, filtered.end());
    parseExtraArgs(scriptArgs, cli);

    if (!fs::exists(file))
    {
        std::cerr << red("Error: File not found: " + file) << "\n";
        return 1;
    }

    if (dumpTok)
    {
        dumpTokensJSON(readFileOrExit(file), file, false);
        return 0;
    }
    if (dumpAst_)
    {
        dumpAstJSON(readFileOrExit(file), file, false);
        return 0;
    }

    return cmdDefault(file, cli.positional, cli);
}