#include "scope.h"
#include <stdexcept>

namespace novac
{

    Scope::Scope(Kind k, std::shared_ptr<Scope> par, std::shared_ptr<Scope> gs)
        : kind(k), parent(par), globalScope(gs)
    {
        // assign localScope, parentScope and globalScope so has() could use them
        variables["localScope"] = VarDesc();
        variables["localScope"].read = [this]() -> Val
        {
            return get("localScope");
        };
        variables["parentScope"] = VarDesc();
        variables["parentScope"].read = [this]() -> Val
        {
            return get("parentScope");
        };
        variables["globalScope"] = VarDesc();
        variables["globalScope"].read = [this]() -> Val
        {
            return get("globalScope");
        };
    }

    // ── get ──────────────────────────────────────────────────────────────────────

    Val Scope::get(const std::string &name) const
    {
        // 0. localScope/globalScope/parentScope
        if (name == "localScope")
        {
            auto out = nova_obj();
            for (auto &[k, v] : variables)
            {
                out->obj->set(k, v.raw);
            }
            return out;
        }
        if (name == "parentScope")
        {
            auto out = nova_obj();
            for (auto &[k, v] : parent->variables)
            {
                out->obj->set(k, v.raw);
            }
            return out;
        }
        if (name == "globalScope")
        {
            auto out = nova_obj();
            for (auto &[k, v] : globalScope->variables)
            {
                out->obj->set(k, v.raw);
            }
            return out;
        }
        // 1. own variables
        {
            auto it = variables.find(name);
            if (it != variables.end())
            {
                auto &desc = it->second;
                if (desc.hasHooks && desc.read)
                    return desc.read();
                return desc.raw;
            }
        }

        // 2. prototype chain (class inheritance)
        for (auto &proto : prototypes)
        {
            if (!proto)
                continue;
            Val pv = proto->get(name);
            if (pv)
                return pv;
        }

        // 3. parent chain (lexical scoping)
        if (parent)
        {
            Val pv = parent->get(name);
            if (pv)
                return pv;
        }

        // 4. global scope (if distinct)
        if (globalScope && globalScope.get() != this && globalScope.get() != parent.get())
        {
            Val gv = globalScope->get(name);
            if (gv)
                return gv;
        }

        return nullptr;
    }

    // ── set ──────────────────────────────────────────────────────────────────────

    void Scope::set(const std::string &name, Val value, bool isConst)
    {
        // if trying to assign localScope/globalScope/parentScope, fail
        if (name == "localScope" || name == "globalScope" || name == "parentScope")
            throw std::runtime_error("Cannot assign to " + name);
        // If this scope already owns the variable, update here
        {
            auto it = variables.find(name);
            if (it != variables.end())
            {
                auto &desc = it->second;
                if (desc.isConst)
                    throw std::runtime_error("Cannot reassign const '" + name + "'");
                if (desc.typeCheck && value && !value.isNull() && !desc.typeCheck(value))
                    throw std::runtime_error("Type mismatch: cannot assign incompatible type to '" + name + "' (expected " + desc.typeName + ")");
                if (desc.hasHooks && desc.write)
                {
                    desc.write(value); // runs smart-var hook (frozen, nonull, setter, etc.)
                    desc.raw = value;  // ← actually persist the new value
                    return;
                }
                desc.raw = value;
                return;
            }
        }

        // Walk parent chain looking for existing binding
        Scope *target = findOwner(name);
        if (target)
        {
            auto &desc = target->variables[name];
            if (desc.isConst)
                throw std::runtime_error("Cannot reassign const '" + name + "'");
            if (desc.typeCheck && value && !value.isNull() && !desc.typeCheck(value))
                throw std::runtime_error("Type mismatch: cannot assign incompatible type to '" + name + "' (expected " + desc.typeName + ")");
            if (desc.hasHooks && desc.write)
            {
                desc.write(value); // runs smart-var hook
                desc.raw = value;  // ← actually persist the new value
                return;
            }
            desc.raw = value;
            return;
        }

        // Check global scope last
        if (globalScope && globalScope.get() != this)
        {
            auto it = globalScope->variables.find(name);
            if (it != globalScope->variables.end())
            {
                auto &desc = it->second;
                if (desc.isConst)
                    throw std::runtime_error("Cannot reassign const '" + name + "'");
                if (desc.typeCheck && value && !value.isNull() && !desc.typeCheck(value))
                    throw std::runtime_error("Type mismatch: cannot assign incompatible type to '" + name + "' (expected " + desc.typeName + ")");
                if (desc.hasHooks && desc.write)
                {
                    desc.write(value); // runs smart-var hook
                    desc.raw = value;  // ← actually persist the new value
                    return;
                }
                desc.raw = value;
                return;
            }
        }

        // New variable — create in this scope
        setOwn(name, value, isConst);
    }

    // ── setOwn ───────────────────────────────────────────────────────────────────

    void Scope::setOwn(const std::string &name, Val value, bool isConst)
    {
        auto it = variables.find(name);
        if (it != variables.end() && it->second.isConst)
            throw std::runtime_error("Cannot reassign const '" + name + "'");
        VarDesc desc;
        desc.raw = value;
        desc.isConst = isConst;
        variables[name] = desc;
    }

    // ── setDescriptor ────────────────────────────────────────────────────────────

    void Scope::setDescriptor(const std::string &name, VarDesc desc)
    {
        desc.hasHooks = (desc.read || desc.write);
        variables[name] = std::move(desc);
    }

    // ── has ──────────────────────────────────────────────────────────────────────

    bool Scope::has(const std::string &name) const
    {
        return variables.count(name) > 0;
    }

    // ── del ──────────────────────────────────────────────────────────────────────

    void Scope::del(const std::string &name)
    {
        variables.erase(name);
        if (parent)
            parent->del(name);
        if (globalScope && globalScope.get() != this)
            globalScope->variables.erase(name);
    }

    // ── snapshot ─────────────────────────────────────────────────────────────────

    std::unordered_map<std::string, Val> Scope::snapshot() const
    {
        // Start with parent snapshot, then overlay own vars
        std::unordered_map<std::string, Val> out;
        if (parent)
            out = parent->snapshot();
        for (auto &[k, desc] : variables)
        {
            Val v = (desc.hasHooks && desc.read) ? desc.read() : desc.raw;
            out[k] = v;
        }
        return out;
    }

    // ── child ────────────────────────────────────────────────────────────────────

    std::shared_ptr<Scope> Scope::child(Kind k) const
    {
        return std::make_shared<Scope>(
            k,
            const_cast<Scope *>(this)->shared_from_this(),
            globalScope);
    }

    // ── findOwner ────────────────────────────────────────────────────────────────

    Scope *Scope::findOwner(const std::string &name)
    {
        Scope *cur = parent.get();
        while (cur)
        {
            if (cur->variables.count(name))
                return cur;
            cur = cur->parent.get();
        }
        return nullptr;
    }

    // ── toString ─────────────────────────────────────────────────────────────────

    std::string Scope::toString() const
    {
        switch (kind)
        {
        case Kind::Global:
            return "GLOBAL";
        case Kind::Module:
            return "MODULE";
        case Kind::Function:
            return "FUNCTION";
        case Kind::Block:
            return "BLOCK";
        case Kind::Instance:
            return "INSTANCE";
        case Kind::Namespace:
            return "NAMESPACE";
        case Kind::Catch:
            return "CATCH";
        case Kind::With:
            return "WITH";
        case Kind::Thread:
            return "THREAD";
        default:
            return "SCOPE";
        }
    }

} // namespace novac