#pragma once
#include "value.h"
#include "../parse/ast.h"
#include <string>
#include <unordered_map>
#include <memory>
#include <functional>

namespace novac {

class Executor;

// ════════════════════════════════════════════════════════════════════════════
//  TypeRegistry
// ════════════════════════════════════════════════════════════════════════════

class TypeRegistry {
public:
    // ── registration ─────────────────────────────────────────────────────────
    void registerType(const std::string& name, std::shared_ptr<Node> def);
    void registerStruct(const std::string& name, std::shared_ptr<Node> def);
    void registerInterface(const std::string& name, std::shared_ptr<Node> def);
    void registerEnum(const std::string& name, std::shared_ptr<Node> def);
    void registerTrait(const std::string& name, std::shared_ptr<Node> def);

    // Custom named type with a C++ validator function
    void registerCustom(const std::string& name,
                        std::function<bool(Val)> validator);

    // ── runtime type check: does `value` satisfy `typeExpr`? ─────────────────
    bool check(Val value, const TypeExpr& expr) const;
    bool check(Val value, const std::string& typeName) const;

    // ── struct instantiation ─────────────────────────────────────────────────
    Val createStruct(const std::string& name,
                     const ValMap& values,
                     Executor& exe) const;

    // ── interface check ──────────────────────────────────────────────────────
    bool satisfies(Val value, const std::string& ifaceName, Executor& exe) const;

    // ── storage ──────────────────────────────────────────────────────────────
    std::unordered_map<std::string, std::shared_ptr<Node>> types;
    std::unordered_map<std::string, std::shared_ptr<Node>> structs;
    std::unordered_map<std::string, std::shared_ptr<Node>> interfaces;
    std::unordered_map<std::string, std::shared_ptr<Node>> enums;
    std::unordered_map<std::string, std::shared_ptr<Node>> traits;
    std::unordered_map<std::string, std::function<bool(Val)>> customs;

private:
    bool _checkNamed(Val value, const std::string& name) const;
};

} // namespace novac
