#include "type_registry.h"
#include "../vm/vm.h"
#include <stdexcept>

namespace novac {

// ── registration ─────────────────────────────────────────────────────────────

void TypeRegistry::registerType(const std::string& n, std::shared_ptr<Node> d)      { types[n]      = d; }
void TypeRegistry::registerStruct(const std::string& n, std::shared_ptr<Node> d)    { structs[n]    = d; }
void TypeRegistry::registerInterface(const std::string& n, std::shared_ptr<Node> d) { interfaces[n] = d; }
void TypeRegistry::registerEnum(const std::string& n, std::shared_ptr<Node> d)      { enums[n]      = d; }
void TypeRegistry::registerTrait(const std::string& n, std::shared_ptr<Node> d)     { traits[n]     = d; }
void TypeRegistry::registerCustom(const std::string& n, std::function<bool(Val)> v) { customs[n]    = v; }

// ── check ─────────────────────────────────────────────────────────────────────

bool TypeRegistry::check(Val value, const std::string& typeName) const {
    TypeExpr te;
    te.kind = TypeExpr::Kind::Named;
    te.name = typeName;
    return check(value, te);
}

bool TypeRegistry::check(Val value, const TypeExpr& expr) const {
    switch (expr.kind) {
        case TypeExpr::Kind::Named:
            return _checkNamed(value, expr.name);

        case TypeExpr::Kind::Union: {
            for (const auto& variant : expr.args)
                if (check(value, variant)) return true;
            return false;
        }

        case TypeExpr::Kind::Intersect: {
            for (const auto& part : expr.args)
                if (!check(value, part)) return false;
            return true;
        }

        case TypeExpr::Kind::Shape:
            return value && value.isObject();

        case TypeExpr::Kind::Array: {
            bool isArr = value && value.isArray();
            if (!isArr) return false;
            bool akit = true;
            for (int i = 0; i < value->arr->length(); i++) {
                if (!(TypeRegistry::check(value->arr->get(i), *expr.of))) akit = false;
            }
            return akit;
        }

        case TypeExpr::Kind::Function:
            return value && value.isFunction();

        case TypeExpr::Kind::Value: {
            // Literal value type
            if (!value) return expr.literalValue == "null";
            const std::string& lv = expr.literalValue;
            if (lv == "true")  return value.isBool() && value->bval;
            if (lv == "false") return value.isBool() && !value->bval;
            if (lv == "null")  return value.isNull();
            // String or numeric literal
            if (value.isString()) return value->sval == lv;
            if (value.isNumber()) {
                try { return value->nval == std::stod(lv); } catch (...) {}
            }
            return false;
        }

        default:
            return true;
    }
}

bool TypeRegistry::_checkNamed(Val value, const std::string& name) const {
    if (!value) return name == "null" || name == "any";
    if (name == "any")    return true;
    if (name == "void")   return !value || value.isNull();
    if (name == "null")   return value.isNull();
    if (name == "bool")   return value.isBool();
    if (name == "number" || name == "int" || name == "float")
                          return value.isNumber();
    if (name == "string") return value.isString();
    if (name == "array")  return value.isArray();
    if (name == "object") return value.isObject() || value.isStruct();
    if (name == "func" || name == "function")
                          return value.isFunction();
    if (name == "range")  return value.isRange();
    if (name == "pointer")return value.isPointer();

    // Custom type
    auto cit = customs.find(name);
    if (cit != customs.end()) return cit->second(value);

    // Struct check
    if (structs.count(name))
        return value.isStruct() && value->strct && value->strct->typeName == name;

    // Enum check
    if (enums.count(name))
        return value.isEnum() && value->enm && value->enm->typeName == name;

    return true; // unknown type — pass through
}

// ── createStruct ─────────────────────────────────────────────────────────────

Val TypeRegistry::createStruct(const std::string& name,
                                const ValMap&      values,
                                Executor&          exe) const {
    auto it = structs.find(name);
    if (it == structs.end())
        throw std::runtime_error("Unknown struct: " + name);

    const Node& def = *it->second;
    ValMap inner;

    for (const auto& field : def.structFields) {
        Val val;
        auto vit = values.find(field.name);
        if (vit != values.end()) {
            val = vit->second;
        } else if (field.defaultValue) {
            val = exe.evaluate(field.defaultValue.get(), exe.globalScope);
        }

        // Type check
        if ((val && field.type.kind != TypeExpr::Kind::Named) || !field.type.name.empty()) {
            if (!check(val, field.type)) {
                throw std::runtime_error("Struct " + name + "." + field.name +
                    ": type mismatch");
            }
        }

        inner[field.name] = val ? val : nova_null();
    }

    return NovaValue::makeStruct(name, inner);
}

// ── satisfies ────────────────────────────────────────────────────────────────

bool TypeRegistry::satisfies(Val value, const std::string& ifaceName, Executor&) const {
    auto it = interfaces.find(ifaceName);
    if (it == interfaces.end()) return false;
    const Node& iface = *it->second;

    // Get the underlying map from the value
    const ValMap* map = nullptr;
    if (value && value.isObject())       map = &value->obj->inner;
    else if (value && value.isStruct())  map = &value->strct->inner;

    if (!map) return false;

    for (const auto& m : iface.ifaceMembers) {
        if (m.isOptional) continue;
        if (map->find(m.name) == map->end()) return false;
    }
    return true;
}

} // namespace novac
