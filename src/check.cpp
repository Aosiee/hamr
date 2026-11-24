// hamr check: Luau's type checker over suites and the standard library,
// with cluau's strict rules. Modules resolve exactly as `require` does at
// run time (modules.cpp), and the host table is declared by host.d.luau.

#include "host.h"

#include <algorithm>
#include <map>

#include "Luau/BuiltinDefinitions.h"
#include "Luau/Config.h"
#include "Luau/Frontend.h"
#include "Luau/Parser.h"
#include "Luau/ToString.h"
#include "cluau/check.h"
#include "cluau/cluau.h"
#include "lua.h"
#include "lualib.h"

namespace hamr {

namespace {

struct ModeConfig : Luau::ConfigResolver {
    Luau::Config nonstrict;
    Luau::Config strict;
    ModeConfig() {
        nonstrict.mode = Luau::Mode::Nonstrict;
        strict.mode = Luau::Mode::Strict;
    }
    const Luau::Config& getConfig(const Luau::ModuleName& name) const override {
        return IsCluauModule(name) ? strict : nonstrict;
    }
};

struct Files : Luau::FileResolver {
    std::map<std::string, cluau::TranslateResult> translations;

    std::optional<Luau::SourceCode> readSource(const Luau::ModuleName& name) override {
        std::optional<std::string> text = ReadModuleSource(name);
        if (!text)
            return std::nullopt;
        if (!IsCluauModule(name))
            return Luau::SourceCode{*text, Luau::SourceCode::Module};
        cluau::TranslateResult& t = translations[name] = cluau::Translate(*text);
        return Luau::SourceCode{t.Luau, Luau::SourceCode::Module};
    }

    std::optional<Luau::ModuleInfo> resolveModule(const Luau::ModuleInfo* context, Luau::AstExpr* node) override {
        auto* expr = node->as<Luau::AstExprConstantString>();
        if (!expr)
            return std::nullopt;
        std::string error;
        std::optional<std::string> name =
            ResolveModule(context ? context->name : std::string(), std::string(expr->value.data, expr->value.size), error);
        if (!name)
            return std::nullopt;
        return Luau::ModuleInfo{*name};
    }

    std::string getHumanReadableModuleName(const Luau::ModuleName& name) const override { return name; }
};

struct Finding {
    std::string File;
    int Line = 0;
    int Col = 0;
    std::string Code;
    std::string Message;
    bool Warning = false;
};

void PushFinding(lua_State* L, const Finding& f, int index) {
    lua_newtable(L);
    lua_pushstring(L, f.File.c_str());
    lua_setfield(L, -2, "file");
    lua_pushinteger(L, f.Line);
    lua_setfield(L, -2, "line");
    lua_pushinteger(L, f.Col);
    lua_setfield(L, -2, "col");
    lua_pushstring(L, f.Code.c_str());
    lua_setfield(L, -2, "code");
    lua_pushstring(L, f.Message.c_str());
    lua_setfield(L, -2, "message");
    lua_pushboolean(L, f.Warning);
    lua_setfield(L, -2, "warning");
    lua_rawseti(L, -2, index);
}

// host.check({ module }) -> { finding }; a module is an absolute path or
// "@hamr/<name>". Raises when the host declarations themselves do not load.
int Check(lua_State* L) {
    std::vector<std::string> modules = StringList(L, 1);
    Files resolver;
    ModeConfig config;
    Luau::FrontendOptions options;
    options.retainFullTypeGraphs = true; // cluau_check reads the inferred types
    Luau::Frontend frontend(&resolver, &config, options);
    Luau::registerBuiltinGlobals(frontend, frontend.globals);
    std::optional<std::string> definitions = ReadHostDeclarations();
    if (!definitions)
        luaL_error(L, "hamr check: host.d.luau is missing");
    Luau::LoadDefinitionFileResult loaded =
        frontend.loadDefinitionFile(frontend.globals, frontend.globals.globalScope, *definitions, "@hamr", false, false);
    if (!loaded.success) {
        std::string why;
        for (auto& e : loaded.parseResult.errors)
            why += "\n  host.d.luau:" + std::to_string(e.getLocation().begin.line + 1) + ": " + e.getMessage();
        if (loaded.module)
            for (auto& e : loaded.module->errors)
                why += "\n  host.d.luau:" + std::to_string(e.location.begin.line + 1) + ": " + Luau::toString(e);
        luaL_error(L, "hamr check: the host declarations do not load:%s", why.c_str());
    }
    Luau::freeze(frontend.globals.globalTypes);

    std::vector<Finding> out;
    for (const std::string& file : modules) {
        Luau::CheckResult result = frontend.check(file);
        auto tr = resolver.translations.find(file);
        const cluau::TranslateResult* translation = tr == resolver.translations.end() ? nullptr : &tr->second;
        auto at = [&](Luau::Position p, Finding& f) {
            cluau::Position pos{(int)p.line, (int)p.column};
            if (translation)
                pos = translation->Map.FromLuau(pos);
            f.Line = pos.Line + 1;
            f.Col = pos.Col + 1;
        };
        if (!ReadModuleSource(file)) {
            out.push_back({file, 0, 0, "Missing", "cannot read the module", false});
            continue;
        }
        if (translation)
            for (const cluau::Diagnostic& d : translation->Diagnostics)
                out.push_back({file, d.At.Begin.Line + 1, d.At.Begin.Col + 1, "cluau", d.Message, d.Warning});
        for (const Luau::TypeError& e : result.errors) {
            if (e.moduleName != file)
                continue; // reported with its own module
            Finding f;
            f.File = file;
            f.Code = Luau::get<Luau::SyntaxError>(e) ? "SyntaxError" : "TypeError";
            f.Message = Luau::toString(e);
            at(e.location.begin, f);
            out.push_back(std::move(f));
        }
        if (translation) {
            const Luau::SourceModule* source = frontend.getSourceModule(file);
            Luau::ModulePtr module = frontend.moduleResolver.getModule(file);
            if (source && module)
                for (const cluau::check::Finding& c : cluau::check::Run(*source, *module, *translation))
                    out.push_back({file, c.At.Begin.Line + 1, c.At.Begin.Col + 1, c.Code, c.Message, c.Warning});
        }
    }
    std::sort(out.begin(), out.end(), [](const Finding& a, const Finding& b) {
        return a.File != b.File ? a.File < b.File : a.Line != b.Line ? a.Line < b.Line : a.Col < b.Col;
    });
    lua_createtable(L, (int)out.size(), 0);
    for (size_t i = 0; i < out.size(); i++)
        PushFinding(L, out[i], (int)i + 1);
    return 1;
}

int StdModules(lua_State* L) {
    PushStringList(L, StdModuleNames());
    return 1;
}

// host.resolve(from, request) -> name | (nil, error): `require`'s rules.
int Resolve(lua_State* L) {
    std::string error;
    std::optional<std::string> name = ResolveModule(CheckString(L, 1), CheckString(L, 2), error);
    if (!name) {
        lua_pushnil(L);
        lua_pushstring(L, error.c_str());
        return 2;
    }
    lua_pushstring(L, name->c_str());
    return 1;
}

// host.require(name): a module by its resolved name (an absolute path or
// "@hamr/<name>"), loaded once, as `require` does.
int RequireByName(lua_State* L) {
    std::string name = CheckString(L, 1);
    if (name.rfind("@hamr", 0) != 0) {
        std::string error;
        std::optional<std::string> resolved = ResolveModule("", name, error);
        if (!resolved) {
            lua_pushstring(L, error.c_str());
            lua_error(L);
        }
        name = *resolved;
    }
    RequireModule(L, name);
    return 1;
}

} // namespace

void OpenCheck(lua_State* L) {
    SetFunction(L, "require", RequireByName);
    SetFunction(L, "check", Check);
    SetFunction(L, "stdModules", StdModules);
    SetFunction(L, "resolve", Resolve);
}

} // namespace hamr
