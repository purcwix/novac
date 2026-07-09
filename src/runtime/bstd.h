#pragma once
#include "../runtime/value.h"
#include "../runtime/scope.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <unordered_map>
#include <functional>
#include <memory>

namespace novac {

class Executor;

// ── registerBstd — called by Executor::_loadBstd ─────────────────────────────
// Registers all bstd bindings into the global scope
void registerBstd(Executor& exe);

// ════════════════════════════════════════════════════════════════════════════
//  History — persistent named history backed by files
// ════════════════════════════════════════════════════════════════════════════
namespace History {
    std::vector<std::string> load(const std::string& name);
    void save(const std::string& name, const std::vector<std::string>& arr);
    void push(const std::string& name, const std::string& entry);
    void clear(const std::string& name);
    std::vector<std::string> keys();
    bool exists(const std::string& name);

    Val makeNovaObj();
}

// ════════════════════════════════════════════════════════════════════════════
//  Storage — simple file-backed key/value store
// ════════════════════════════════════════════════════════════════════════════
namespace Storage {
    void        setItem(const std::string& key, const std::string& value);
    std::string getItem(const std::string& key);
    void        removeItem(const std::string& key);
    void        clearAll();

    Val makeNovaObj();
}

// ════════════════════════════════════════════════════════════════════════════
//  Crypto — minimal hash utilities
// ════════════════════════════════════════════════════════════════════════════
namespace Crypto {
    std::string hash(const std::string& data, const std::string& algo = "sha256");
    std::string hmac(const std::string& key, const std::string& data, const std::string& algo = "sha256");

    Val makeNovaObj();
}

// ════════════════════════════════════════════════════════════════════════════
//  Base64
// ════════════════════════════════════════════════════════════════════════════
namespace Base64 {
    std::string encode(const std::string& input);
    std::string decode(const std::string& input);

    Val makeNovaObj();
}

// ════════════════════════════════════════════════════════════════════════════
//  PathUtils
// ════════════════════════════════════════════════════════════════════════════
namespace PathUtils {
    std::string join(const std::vector<std::string>& segments);
    std::string resolve(const std::vector<std::string>& segments);
    std::string basename(const std::string& p, const std::string& ext = "");
    std::string dirname(const std::string& p);
    std::string extname(const std::string& p);

    Val makeNovaObj();
}

// ════════════════════════════════════════════════════════════════════════════
//  OsUtils
// ════════════════════════════════════════════════════════════════════════════
namespace OsUtils {
    std::string platform();
    std::string homedir();
    std::string tmpdir();
    uint64_t    totalmem();
    uint64_t    freemem();

    Val makeNovaObj();
}

// ════════════════════════════════════════════════════════════════════════════
//  UUID
// ════════════════════════════════════════════════════════════════════════════
namespace UUID {
    std::string v4();
    std::string shortId();
    bool        is(const std::string& s);

    Val makeNovaObj();
}

// ════════════════════════════════════════════════════════════════════════════
//  Deep — deep clone, equal, merge on Val
// ════════════════════════════════════════════════════════════════════════════
namespace Deep {
    Val  clone(Val v);
    bool equal(Val a, Val b);
    Val  merge(Val target, Val source);

    Val makeNovaObj();
}

// ════════════════════════════════════════════════════════════════════════════
//  Timers
// ════════════════════════════════════════════════════════════════════════════
namespace Timers {
    void sleepMs(int ms);
    Val makeNovaObj(Executor& exe);
}

// ════════════════════════════════════════════════════════════════════════════
//  Env — .env file reader
// ════════════════════════════════════════════════════════════════════════════
namespace Env {
    std::unordered_map<std::string,std::string> load(const std::string& filepath = "");
    std::string get(const std::string& key);
    void        set(const std::string& key, const std::string& value);

    Val makeNovaObj();
}

// ════════════════════════════════════════════════════════════════════════════
//  SyntaxHighlight — pluggable ANSI highlighter registry
// ════════════════════════════════════════════════════════════════════════════
namespace SyntaxHighlight {
    using Highlighter = std::function<std::string(const std::string&)>;

    void        registerHighlighter(const std::string& name, Highlighter fn);
    Highlighter get(const std::string& name);
    std::string highlight(const std::string& name, const std::string& line);
    std::vector<std::string> list();

    Val makeNovaObj();
}

// ════════════════════════════════════════════════════════════════════════════
//  InputMgr — synchronous terminal input with history, highlight, list
// ════════════════════════════════════════════════════════════════════════════
namespace InputMgr {
    struct PromptOpts {
        bool        password    = false;
        std::string mask        = "•";
        std::string historyKey  = "default";
        std::string highlight;      // theme name or empty
        std::string placeholder;
    };

    std::string prompt(const std::string& message, const PromptOpts& opts = {});

    struct ListItem { std::string value; int index; };
    struct ListOpts {
        bool        multi          = false;
        std::string color          = "\x1b[34m";
        std::string highlightColor = "\x1b[32m";
        std::string pointer        = "❯ ";
        std::string label;
    };
    std::vector<ListItem> list(const std::vector<std::string>& items, const ListOpts& opts = {});

    Val makeNovaObj(Executor& exe);
}

// ════════════════════════════════════════════════════════════════════════════
//  Stack / Queue
// ════════════════════════════════════════════════════════════════════════════
Val makeNovaStack();
Val makeNovaQueue();

} // namespace novac
