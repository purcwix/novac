#include "cli.h"
#include "../lex/lexer.h"
#include "../error/novac_error.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <regex>
#include <algorithm>
#include <cstdlib>
#include <ctime>

namespace fs = std::filesystem;

namespace novac::cli
{

    // ════════════════════════════════════════════════════════════════════════════
    //  Color helpers
    // ════════════════════════════════════════════════════════════════════════════

    static bool colorEnabled()
    {
        static int cached = -1;
        if (cached == -1)
            cached = std::getenv("NO_COLOR") ? 0 : 1;
        return cached == 1;
    }

    std::string clr(const std::string &code, const std::string &text)
    {
        if (!colorEnabled())
            return text;
        return "\x1b[" + code + "m" + text + "\x1b[0m";
    }
    std::string red(const std::string &s) { return clr("31", s); }
    std::string green(const std::string &s) { return clr("32", s); }
    std::string yellow(const std::string &s) { return clr("33", s); }
    std::string blue(const std::string &s) { return clr("34", s); }
    std::string gray(const std::string &s) { return clr("90", s); }
    std::string bold(const std::string &s) { return clr("1", s); }

    // ════════════════════════════════════════════════════════════════════════════
    //  File / glob helpers
    // ════════════════════════════════════════════════════════════════════════════

    std::string readFileOrExit(const std::string &path)
    {
        std::ifstream f(path, std::ios::binary);
        if (!f.is_open())
        {
            std::cerr << red("Error: cannot open file: " + path) << "\n";
            std::exit(1);
        }
        return std::string(std::istreambuf_iterator<char>(f), {});
    }

    bool isGlobPattern(const std::string &s)
    {
        return s.find_first_of("*?{}[]") != std::string::npos;
    }

    // Convert a simple shell glob (*, ?) to a regex
    static std::regex globToRegex(const std::string &pattern)
    {
        std::string out;
        for (char c : pattern)
        {
            switch (c)
            {
            case '.':
            case '+':
            case '^':
            case '$':
            case '(':
            case ')':
            case '|':
            case '[':
            case ']':
            case '\\':
                out += '\\';
                out += c;
                break;
            case '*':
                out += ".*";
                break;
            case '?':
                out += ".";
                break;
            default:
                out += c;
            }
        }
        return std::regex("^" + out + "$");
    }

    // Expand a single glob pattern by walking the directory tree
    std::vector<std::string> expandGlob(const std::string &pattern)
    {
        if (!isGlobPattern(pattern))
        {
            // Pass through; caller checks existence
            return {pattern};
        }

        // Split into directory prefix (no glob chars) and the glob tail
        fs::path p(pattern);
        std::string patStr = p.generic_string();

        // Find the first path component containing a glob char
        std::vector<std::string> parts;
        {
            std::stringstream ss(patStr);
            std::string seg;
            while (std::getline(ss, seg, '/'))
                parts.push_back(seg);
        }

        fs::path base = patStr.front() == '/' ? fs::path("/") : fs::path(".");
        size_t globIdx = 0;
        for (; globIdx < parts.size(); globIdx++)
        {
            if (isGlobPattern(parts[globIdx]))
                break;
            if (!parts[globIdx].empty())
                base /= parts[globIdx];
        }

        // Check for recursive **
        bool recursive = false;
        for (auto &seg : parts)
            if (seg == "**")
                recursive = true;

        std::vector<std::string> results;
        if (!fs::exists(base))
            return {};

        std::string tailPattern;
        for (size_t i = globIdx; i < parts.size(); i++)
        {
            if (i > globIdx)
                tailPattern += "/";
            tailPattern += parts[i];
        }
        // Build regex from tail (treat ** as .*)
        std::string rx;
        for (size_t i = 0; i < tailPattern.size(); i++)
        {
            if (tailPattern[i] == '*' && i + 1 < tailPattern.size() && tailPattern[i + 1] == '*')
            {
                rx += ".*";
                i++;
                if (i + 1 < tailPattern.size() && tailPattern[i + 1] == '/')
                    i++;
            }
            else if (tailPattern[i] == '*')
                rx += "[^/]*";
            else if (tailPattern[i] == '?')
                rx += "[^/]";
            else if (std::string(".+^$()|[]\\").find(tailPattern[i]) != std::string::npos)
            {
                rx += '\\';
                rx += tailPattern[i];
            }
            else
                rx += tailPattern[i];
        }
        std::regex re("^" + rx + "$");

        auto matchAndAdd = [&](const fs::path &filePath)
        {
            std::string rel = fs::relative(filePath, base).generic_string();
            if (std::regex_match(rel, re))
                results.push_back(filePath.string());
        };

        try
        {
            if (recursive || tailPattern.find('/') != std::string::npos)
            {
                for (auto &entry : fs::recursive_directory_iterator(base))
                    if (entry.is_regular_file())
                        matchAndAdd(entry.path());
            }
            else
            {
                for (auto &entry : fs::directory_iterator(base))
                    if (entry.is_regular_file())
                        matchAndAdd(entry.path());
            }
        }
        catch (...)
        {
        }

        std::sort(results.begin(), results.end());
        return results;
    }

    std::vector<std::string> expandFileArgs(const std::vector<std::string> &args)
    {
        std::vector<std::string> out;
        std::vector<std::string> seen;
        for (auto &a : args)
        {
            for (auto &f : expandGlob(a))
            {
                std::string abs;
                try
                {
                    abs = fs::absolute(f).string();
                }
                catch (...)
                {
                    abs = f;
                }
                if (std::find(seen.begin(), seen.end(), abs) == seen.end())
                {
                    seen.push_back(abs);
                    out.push_back(f); // keep relative for display, but dedupe by absolute
                }
            }
        }
        return out;
    }

    // ════════════════════════════════════════════════════════════════════════════
    //  runSource — shared execution path
    // ════════════════════════════════════════════════════════════════════════════
    static Val cliArgsToNova(const CliArgs &cli)
    {
        auto obj = nova_obj();

        // Cli.positional
        ValVec pos;
        for (auto &a : cli.positional)
            pos.push_back(nova_str(a));
        obj->obj->set("positional", nova_arr(std::move(pos)));

        // Cli.options  — --key value / --key=value
        auto opts = nova_obj();
        for (auto &[k, v] : cli.options)
            opts->obj->set(k, nova_str(v));
        obj->obj->set("options", opts);

        // Cli.flags  — --key (no value) / -k
        auto flags = nova_obj();
        for (auto &[k, v] : cli.flags)
            flags->obj->set(k, nova_bool(v));
        obj->obj->set("flags", flags);

        // Cli.addrs  — @key
        auto addrs = nova_obj();
        for (auto &[k, v] : cli.addrs)
            addrs->obj->set(k, nova_bool(v));
        obj->obj->set("addrs", addrs);

        // Cli.additions  — +key
        auto additions = nova_obj();
        for (auto &[k, v] : cli.additions)
            additions->obj->set(k, nova_bool(v));
        obj->obj->set("additions", additions);

        // Cli.argsRaw
        ValVec raw;
        for (auto &a : cli.raw)
            raw.push_back(nova_str(a));
        obj->obj->set("argsRaw", nova_arr(std::move(raw)));

        // Cli.has(key) — checks flags, options, addrs, additions
        obj->obj->set("has", NovaValue::makeNative([obj](ValVec a, auto) -> Val
                                                   {
        if (a.empty()) return nova_bool(false);
        std::string key = a[0].asString();
        for (auto field : {"flags", "options", "addrs", "additions"}) {
            Val f = obj->obj->get(field);
            if (f && f.isObject() && f->obj->has(key)) return nova_bool(true);
        }
        return nova_bool(false); }, "has"));

        // Cli.get(key, fallback?) — get option value or fallback
        obj->obj->set("get", NovaValue::makeNative([obj](ValVec a, auto) -> Val
                                                   {
        if (a.empty()) return nova_null();
        std::string key = a[0].asString();
        Val fallback = a.size() > 1 ? a[1] : nova_null();
        Val o = obj->obj->get("options");
        if (o && o.isObject()) {
            Val v = o->obj->get(key);
            if (v) return v;
        }
        return fallback; }, "get"));

        return obj;
    }
    int runSource(const std::string &src, const std::string &fname, const CliArgs &cli)
    {
        try
        {
            Parser p(src, fname);
            auto ast = p.parse();

            Executor exe(src, fname);

            exe.globalScope->setOwn("Cli", cliArgsToNova(cli));

            // __file__ / __dirname__
            fs::path fp(fname);
            exe.globalScope->setOwn("__file__", nova_str(fname));
            exe.globalScope->setOwn("__dirname__", nova_str(fp.has_parent_path() ? fp.parent_path().string() : "."));

            // process.argv
            Val proc = exe.globalScope->get("process");
            if (proc && proc.isObject())
            {
                ValVec argv;
                argv.push_back(nova_str("novac"));
                argv.push_back(nova_str(fname));
                for (auto &a : cli.positional)
                    argv.push_back(nova_str(a));
                proc->obj->set("argv", nova_arr(argv));
            }

            exe.run(ast.get());
            return 0;
        }
        catch (const NovacError &e)
        {
            std::cerr << red(e.what()) << "\n";
            return 1;
        }
        catch (const ThrowSignal &e)
        {
            std::cerr << red("[uncaught throw] ");
            if (e.value)
                std::cerr << e.value.asString();
            std::cerr << "\n";
            return 1;
        }
        catch (const std::exception &e)
        {
            std::cerr << red(std::string("Error: ") + e.what()) << "\n";
            return 1;
        }
    }

    // ════════════════════════════════════════════════════════════════════════════
    //  novac <file> [args...]  /  novac run <file> [args...]
    // ════════════════════════════════════════════════════════════════════════════

    int cmdDefault(const std::string &file, const std::vector<std::string> &args, CliArgs cli)
    {
        return cmdRun(file, args, std::move(cli));
    }

    int cmdRun(const std::string &file, const std::vector<std::string> &args, CliArgs cli)
    {
        fs::path filePath = fs::absolute(file);
        if (!fs::exists(filePath))
        {
            std::cerr << red("Error: File not found: " + file) << "\n";
            return 1;
        }
        std::string src = readFileOrExit(filePath.string());

        cli.positional = args;
        return runSource(src, file, cli);
    }

    // ════════════════════════════════════════════════════════════════════════════
    //  novac eval <code>
    // ════════════════════════════════════════════════════════════════════════════

    int cmdEval(const std::string &code)
    {
        CliArgs cli;
        return runSource(code, "<eval>", cli);
    }

} // namespace novac::cli
