#pragma once
#include "../parse/ast.h"
#include "../parse/parser.h"
#include "../runtime/value.h"
#include "../runtime/scope.h"
#include "../runtime/type_registry.h"
#include "../runtime/fiber.h"
#include "../runtime/fetch.h"
#include "../runtime/socket.h"
#include "../error/novac_error.h"
#include <string>
#include <memory>
#include <unordered_map>
#include <vector>
#include <functional>
#include <optional>

namespace novac
{

    // ── control flow signals ─────────────────────────────────────────────────────
    struct ReturnSignal
    {
        Val value;
        bool hard = false;
    };
    struct BreakSignal
    {
        std::string label;
    };
    struct ContinueSignal
    {
        std::string label;
    };
    struct GobackSignal
    {
    };
    struct ThrowSignal
    {
        Val value;
    };

    // ── algebraic effect signal ─────────────────────────────────────────────────
    struct PerformSignal
    {
        std::string effectName;
        ValVec args;
    };

    // ── statement execution result ────────────────────────────────────────────────
    // All statement handlers and execute()/_runBody() return this instead of
    // throwing C++ exceptions for return/break/continue.
    enum class Signal : uint8_t
    {
        Normal,
        Return,
        Break,
        Goback,
        Continue
    };

    struct ExecResult
    {
        Signal sig = Signal::Normal;
        Val value;
        bool hardReturn = false;

        ExecResult() = default;
        static ExecResult Val_(Val v)
        {
            ExecResult r;
            r.value = std::move(v);
            return r;
        }
        static ExecResult Returned(Val v, bool hard)
        {
            ExecResult r;
            r.sig = Signal::Return;
            r.value = std::move(v);
            r.hardReturn = hard;
            return r;
        }
        static ExecResult Broke()
        {
            ExecResult r;
            r.sig = Signal::Break;
            return r;
        }
        static ExecResult Goback()
        {
            ExecResult r;
            r.sig = Signal::Goback;
            return r;
        }
        static ExecResult Continued()
        {
            ExecResult r;
            r.sig = Signal::Continue;
            return r;
        }

        bool isNormal() const { return sig == Signal::Normal; }
    };

    // ── event registry ────────────────────────────────────────────────────────────
    struct EventListener
    {
        std::string param;
        NodeList body;
        std::shared_ptr<Scope> scope;
    };

    // ════════════════════════════════════════════════════════════════════════════
    //  Executor
    // ════════════════════════════════════════════════════════════════════════════

    class Executor
    {
    public:
        std::unordered_map<std::string, Val> classRegistry; // name → class Val, for Super lookup
        explicit Executor(std::string source, std::string filename = "<repl>");

        // ── public API ────────────────────────────────────────────────────────────
        Val run(Node *ast, std::shared_ptr<Scope> scope = nullptr);

        // Evaluate an expression node
        Val evaluate(const Node *node, std::shared_ptr<Scope> scope);

        // Execute a statement node — returns ExecResult carrying return/break/continue signals
        ExecResult execute(const Node *node, std::shared_ptr<Scope> scope);

        Val callFunction(Val fn, ValVec args, std::shared_ptr<Scope> callerScope,
                         Val thisVal = nullptr);
        Val runFunctionNode(const Node *node, std::shared_ptr<Scope> closure,
                            ValVec args, bool strictArgs, Val thisVal = nullptr);

        // Stringify a value (for print / tostring)
        std::string stringify(Val v, int indent = 0);

        // ── public state ─────────────────────────────────────────────────────────
        std::shared_ptr<Scope> globalScope;
        TypeRegistry types;
        std::string source;
        std::string filename;

        // event bus: name → list of listeners
        std::unordered_map<std::string, std::vector<EventListener>> eventListeners;

        // option flags (with/option)
        std::unordered_map<std::string, Val> options;

        // namespace registry: ns name → scope
        std::unordered_map<std::string, std::shared_ptr<Scope>> namespaces;

        // module cache: path → exported object
        std::unordered_map<std::string, Val> moduleCache;

        // custom escape registry (exposed to lexer re-registration at runtime)
        std::unordered_map<std::string, std::string> runtimeEscapes;

        // REPL mode: accumulate definitions across calls
        bool replMode = false;

    private:
        // ── statement handlers ───────────────────────────────────────────────────
        ExecResult _execDeclare(const Node *n, std::shared_ptr<Scope> s);
        ExecResult _execFunction(const Node *n, std::shared_ptr<Scope> s);
        ExecResult _execClass(const Node *n, std::shared_ptr<Scope> s);
        ExecResult _execBranch(const Node *n, std::shared_ptr<Scope> s);
        ExecResult _execForOf(const Node *n, std::shared_ptr<Scope> s);
        ExecResult _execForIn(const Node *n, std::shared_ptr<Scope> s);
        ExecResult _execEach(const Node *n, std::shared_ptr<Scope> s);
        ExecResult _execSwitch(const Node *n, std::shared_ptr<Scope> s);
        ExecResult _execMatch(const Node *n, std::shared_ptr<Scope> s);
        ExecResult _execTry(const Node *n, std::shared_ptr<Scope> s);
        ExecResult _execReturn(const Node *n, std::shared_ptr<Scope> s);
        ExecResult _execThrow(const Node *n, std::shared_ptr<Scope> s);
        ExecResult _execEmit(const Node *n, std::shared_ptr<Scope> s);
        ExecResult _execOn(const Node *n, std::shared_ptr<Scope> s);
        ExecResult _execWith(const Node *n, std::shared_ptr<Scope> s);
        ExecResult _execImport(const Node *n, std::shared_ptr<Scope> s);
        ExecResult _execExport(const Node *n, std::shared_ptr<Scope> s);
        ExecResult _execNamespace(const Node *n, std::shared_ptr<Scope> s);
        ExecResult _execServer(const Node *n, std::shared_ptr<Scope> s);
        ExecResult _execTypeDecl(const Node *n, std::shared_ptr<Scope> s);
        ExecResult _execStructDecl(const Node *n, std::shared_ptr<Scope> s);
        ExecResult _execEnumDecl(const Node *n, std::shared_ptr<Scope> s);
        ExecResult _execInterfaceDecl(const Node *n, std::shared_ptr<Scope> s);
        ExecResult _execTraitDecl(const Node *n, std::shared_ptr<Scope> s);
        ExecResult _execImplDecl(const Node *n, std::shared_ptr<Scope> s);

        // ── expression helpers ───────────────────────────────────────────────────
        Val _evalBinary(const Node *n, std::shared_ptr<Scope> s);
        Val _evalUnary(const Node *n, std::shared_ptr<Scope> s);
        Val _evalCall(const Node *n, std::shared_ptr<Scope> s);
        Val _evalStructCall(const Node *n, std::shared_ptr<Scope> s);
        Val _evalProp(const Node *n, std::shared_ptr<Scope> s, bool isOptional);
        Val _evalSubscript(const Node *n, std::shared_ptr<Scope> s);
        Val _evalAssign(const Node *n, std::shared_ptr<Scope> s);
        Val _evalFString(const Node *n, std::shared_ptr<Scope> s);
        Val _evalObject(const Node *n, std::shared_ptr<Scope> s);
        Val _evalArray(const Node *n, std::shared_ptr<Scope> s);
        Val _evalArrow(const Node *n, std::shared_ptr<Scope> s);
        Val _evalNew(const Node *n, std::shared_ptr<Scope> s);
        Val _evalAwait(const Node *n, std::shared_ptr<Scope> s);
        Val _evalPerform(const Node *n, std::shared_ptr<Scope> s);
        Val _buildSuperProxy(Val superClassVal, Val thisVal);
        Val _evalDVar(const Node *n, std::shared_ptr<Scope> s);
        Val _evalHttpRequest(const Node *n, std::shared_ptr<Scope> s);
        Val _evalFetch(const Node *n, std::shared_ptr<Scope> s);

        // ── binary op dispatch ───────────────────────────────────────────────────
        Val _novaBinaryOp(const std::string &op, Val left, Val right, const Node *n);
        Val _novaUnaryOp(const std::string &op, Val operand);

        // ── property getter on a value ────────────────────────────────────────────
        Val _getProp(Val obj, const std::string &key, std::shared_ptr<Scope> s, bool isOptional);
        void _setProp(Val obj, const std::string &key, Val value);
        void _setDeep(Val obj, const Node *path, Val value, std::shared_ptr<Scope> s);

        // ── destructure assignment ───────────────────────────────────────────────
        void _destructureObj(const ObjPattern &pat, Val val, std::shared_ptr<Scope> s, bool isConst);
        void _destructureArr(const ArrPattern &pat, Val val, std::shared_ptr<Scope> s, bool isConst);

        // ── module loading ───────────────────────────────────────────────────────
        Val _loadModule(const std::string &path, std::shared_ptr<Scope> s, bool isBuiltin = false);
        Val _loadKitBinary(const std::string &kitName, std::shared_ptr<Scope> s);

        // ── class instance creation ───────────────────────────────────────────────
        Val _instantiateClass(Val classDef, ValVec args);

        // ── async runner ─────────────────────────────────────────────────────────
        Val _runAsync(const Node *fnNode, std::shared_ptr<Scope> s, ValVec args);

        // ── generator runner ─────────────────────────────────────────────────────
        Val _makeGenerator(const Node *fnNode, std::shared_ptr<Scope> s, ValVec args);

        // ── body runner with break/continue/return support ───────────────────────
        // Runs statements, short-circuiting on first non-Normal signal
        ExecResult _runBody(const NodeList &body, std::shared_ptr<Scope> s);

        // ── type helpers ─────────────────────────────────────────────────────────
        std::string _typeOf(Val v) const;
        bool _isTruthy(Val v) const
        {
            if (v.isBigInt())
                return !v->bigint->isZero();
            if (v.isBigFloat())
                return !v->bigfloat->mantissa.isZero();
            return v && v->asBool();
        }

        // nat / unat / raw_ptr support
        Val _toNative(Val v, const std::string &typeStr);
        Val _fromNative(Val v);
        Val _toRawPtr(Val v, const std::string &typeStr = "");
        std::string _inferNativeType(Val v) const;
        void _registerNative(Val obj);

        // ── match pattern check ──────────────────────────────────────────────────
        bool _matchPattern(Val subject, const Node *pattern, std::shared_ptr<Scope> s);

        // ── stringify helper (recursive) ─────────────────────────────────────────
        std::string _stringify(Val v, int indent, int depth);

        // ── sync http fetch ───────────────────────────────────────────────────────
        Val _syncFetch(const std::string &url, Val options);

        // perform handlers
        std::unordered_map<std::string, Val> _performHandlers;

        // ── built-in scope setup ─────────────────────────────────────────────────
        void _setupGlobalScope();
        void _registerMath(Val obj);
        void _registerJSON(Val obj);
        void _registerCCType(Val obj);
        void _registerAlgorithm(Val obj);
        void _registerBit(Val obj);
        void _registerComplex(Val obj);
        void _registerValarray(Val obj);
        void _registerCharconv(Val obj);
        void _registerString(Val obj);
        void _registerArray(Val obj);
        void _registerObject(Val obj);
        void _registerProcess(Val obj);
        void _registerIO(Val obj);
        void _registerStdioControl(Val obj);
        void _registerMemory(Val obj);
        void _registerCFile(Val obj);
        void _registerFFI(Val obj);
        void _registerSystemControl(Val obj);
        void _registerErrnoControl(Val obj);
        void _registerSignalControl(Val obj);
        void _registerFS(Val obj);
        void _registerTimers(Val obj);
        void _registerRandom(Val obj);
        void _registerChrono(Val obj);
        void _registerSocket(Val obj);
        void _registerFiber(Val obj);
        void _registerExperimental(Val obj);
        void _loadBstd(std::shared_ptr<Scope> s);

        // ── error helper ─────────────────────────────────────────────────────────
        [[noreturn]] void _error(const std::string &msg, const Node *n = nullptr);

        // ── source info ──────────────────────────────────────────────────────────
        int _currentLine = 0, _currentCol = 0;
    };

} // namespace novac