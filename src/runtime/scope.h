#pragma once
#include "../parse/parser.h"
#include "value.h"
#include <unordered_map>
#include <string>
#include <memory>
#include <vector>
#include <functional>

namespace novac {

// ── Variable descriptor — supports read/write hooks (smart vars) ─────────────
struct VarDesc {
    Val                       raw;      // plain value (when hooks are null)
    std::function<Val()>      read;     // getter hook (lazy, getter modifier)
    std::function<void(Val)>  write;    // setter hook (setter, frozen, nonull, etc.)
    bool                      isConst  = false;
    bool                      hasHooks = false;
    std::optional<TypeExpr>   explicitType;
    // type enforcement: set on typed declarations, checked on every assignment
    std::string               typeName; // e.g. "number", "string" — empty means untyped
    std::function<bool(Val)>  typeCheck; // returns true if value is compatible
};

// ════════════════════════════════════════════════════════════════════════════
//  Scope
// ════════════════════════════════════════════════════════════════════════════

class Scope : public std::enable_shared_from_this<Scope> {
public:
    enum class Kind { Global, Module, Function, Block, Instance, Namespace, Catch, With, Thread };

    Kind                        kind;
    std::shared_ptr<Scope>      parent;
    std::shared_ptr<Scope>      globalScope;

    // variable storage
    std::unordered_map<std::string, VarDesc> variables;

    // prototype chain (for class instances inheriting from parent classes)
    std::vector<std::shared_ptr<Scope>> prototypes;

    // metadata
    std::string  funcName;        // set on function scopes — used by __func__ dvar
    std::string  namespaceName;   // set on namespace scopes — used by __namespace__
    int          loopIndex = -1;  // set on loop body scopes — used by __iter__

    // ── construction ─────────────────────────────────────────────────────────
    explicit Scope(Kind k = Kind::Block,
                   std::shared_ptr<Scope> par = nullptr,
                   std::shared_ptr<Scope> gs  = nullptr);

    // ── lookup ───────────────────────────────────────────────────────────────
    // get: walks parent chain; honours read hooks; returns nullptr if missing
    Val  get(const std::string& name) const;

    // set: updates existing binding in whichever scope owns it;
    //      falls back to setOwn if not found anywhere
    void set(const std::string& name, Val value, bool isConst = false);

    // setOwn: always writes to THIS scope (used for declarations / shadowing)
    void setOwn(const std::string& name, Val value, bool isConst = false);

    // setDescriptor: register a read/write hook pair (smart variable modifiers)
    void setDescriptor(const std::string& name, VarDesc desc);

    // has: checks only this scope's own variables
    bool has(const std::string& name) const;

    // del: removes from this scope and all parents
    void del(const std::string& name);

    // ── helpers ───────────────────────────────────────────────────────────────
    // snapshot: returns a flat map of all visible variables (for thread serialisation)
    std::unordered_map<std::string, Val> snapshot() const;

    // child: creates a child scope with this as parent
    std::shared_ptr<Scope> child(Kind k = Kind::Block) const;

    std::string toString() const;

private:
    // Walk up the chain to find the scope that owns `name` (excluding global)
    Scope* findOwner(const std::string& name);
};

} // namespace novac