// Unreal's object system read from memory (uobject.h). How each part is found:
// - Names: the name pool's block list, whose first block starts with the entries "None" and "ByteProperty".
// - Objects: the chunked object array (64K items a chunk), told by its counts; an item's object holds its own index.
// - Classes: Class is the class of every class; the struct layout comes from Actor and Pawn (their supers are Object
//   and Actor), properties' from Actor's (owner, names, offsets in order) and RootComponent (class SceneComponent).
// - ProcessEvent: the base object's virtual that tests a function's FUNC_Native flag, proved by calling
//   KismetMathLibrary's Add_IntInt(2, 3).
#include "uobject.h"

#include "script.h"
#include <lauxlib.h>
#include <lua.h>
#include <windows.h>
#include <psapi.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <intrin.h>
#include <unordered_map>

void unreal_note(const char* fmt, ...);  // unreal.cpp: the adapter's log

namespace ue {
namespace {

// --- Memory -------------------------------------------------------------------------------------------------------------
bool peek(const void* p, void* out, size_t n) {
    __try {
        memcpy(out, p, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <class T>
T rd(const void* p, ptrdiff_t off = 0) {
    T v{};
    if (p) peek((const uint8_t*)p + off, &v, sizeof(T));
    return v;
}

bool pointer_like(uintptr_t v) { return v >= 0x10000 && v < 0x7FFFFFFFFFFF && !(v & 7); }

struct Span {
    uint8_t *begin, *end;
};

// Writable sections of the exe (monolithic builds) and CoreUObject (modular builds): where the engine's globals live.
std::vector<Span> data_spans() {
    std::vector<Span> out;
    HMODULE mods[1024];
    DWORD bytes = 0;
    K32EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &bytes);
    HMODULE exe = GetModuleHandleW(nullptr);
    for (DWORD i = 0; i < bytes / sizeof(HMODULE); ++i) {
        wchar_t name[MAX_PATH];
        GetModuleBaseNameW(GetCurrentProcess(), mods[i], name, MAX_PATH);
        if (mods[i] != exe && !wcsstr(name, L"CoreUObject")) continue;
        auto* base = (uint8_t*)mods[i];
        auto* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
        auto* sec = IMAGE_FIRST_SECTION(nt);
        for (int s = 0; s < nt->FileHeader.NumberOfSections; ++s)
            if (sec[s].Characteristics & IMAGE_SCN_MEM_WRITE)
                out.push_back({base + sec[s].VirtualAddress, base + sec[s].VirtualAddress + (sec[s].Misc.VirtualSize & ~7u)});
    }
    return out;
}

// --- Layout ---------------------------------------------------------------------------------------------------------------
// UObjectBase: vtable, flags, index, class, name, outer. UField::Next follows (functions in a class's Children).
constexpr int kFlags = 0x8, kIndex = 0xC, kClass = 0x10, kName = 0x18, kOuter = 0x20, kNext = 0x28;
constexpr uint32_t RF_ClassDefaultObject = 0x10, RF_ArchetypeObject = 0x20;
constexpr uint64_t CPF_ConstParm = 0x2, CPF_Parm = 0x80, CPF_OutParm = 0x100, CPF_ReturnParm = 0x400;

struct Layout {
    uint8_t* blocks = nullptr;  // the name pool's block pointers
    int nameShift = 6;          // an entry's length: header >> 6
    uint8_t* objects = nullptr; // FChunkedFixedUObjectArray: chunks, preallocated, max, count, max chunks, chunks
    int stride = 0x18;          // FUObjectItem
    Obj* uclass = nullptr;
    int super = 0, children = 0, childProps = 0, propsSize = 0;  // UStruct
    int fieldNext = 0x20, fieldName = 0x28;                       // FField
    int arrayDim = 0, elemSize = 0, propFlags = 0, offset = 0, propEnd = 0;  // FProperty
    int funcFlags = 0;          // UFunction::FunctionFlags
    int processEvent = -1;      // its vtable slot
} g;

int g_state;  // 0 not tried, 1 ready, -1 failed
DWORD g_gameThread;
std::unordered_map<uint64_t, std::string> g_names;  // name id → text
std::unordered_map<std::string, Obj*> g_classes;

std::string utf8(const wchar_t* w, int n) {
    std::string s(WideCharToMultiByte(CP_UTF8, 0, w, n, nullptr, 0, nullptr, nullptr), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, n, s.data(), (int)s.size(), nullptr, nullptr);
    return s;
}

const std::string& entry(uint32_t id) {
    auto it = g_names.find(id);
    if (it != g_names.end()) return it->second;
    std::string s;
    const uint8_t* block = rd<const uint8_t*>(g.blocks, (ptrdiff_t)(id >> 16) * 8);
    if (block) {
        const uint8_t* e = block + (id & 0xFFFF) * 2;
        uint16_t h = rd<uint16_t>(e);
        int len = h >> g.nameShift;
        if (len > 0 && len < 1024) {
            if (h & 1) {
                std::wstring w(len, L'\0');
                if (peek(e + 2, w.data(), len * 2)) s = utf8(w.data(), len);
            } else {
                s.resize(len);
                if (!peek(e + 2, s.data(), len)) s.clear();
            }
        }
    }
    return g_names[id] = s;
}

std::string fname(const void* p) {
    std::string s = entry(rd<uint32_t>(p));
    if (uint32_t n = rd<uint32_t>(p, 4)) s += "_" + std::to_string(n - 1);
    return s;
}

int count() { return rd<int32_t>(g.objects, 0x14); }

Obj* at(int i) {
    const uint8_t* chunk = rd<const uint8_t*>(rd<const uint8_t*>(g.objects), (ptrdiff_t)(i >> 16) * 8);
    return chunk ? rd<Obj*>(chunk, (ptrdiff_t)(i & 0xFFFF) * g.stride) : nullptr;
}

// The name pool: a pointer to a block that starts "None", "ByteProperty"; the block list it starts holds the current
// block's index just before it and no blocks past that.
bool find_names(const std::vector<Span>& spans) {
    for (const Span& s : spans)
        for (uint8_t* p = s.begin; p + 8 <= s.end; p += 8) {
            uintptr_t v = rd<uintptr_t>(p);
            if (!pointer_like(v)) continue;
            uint8_t head[20];
            if (!peek((void*)v, head, 20) || memcmp(head + 2, "None", 4) || memcmp(head + 8, "ByteProperty", 12)) continue;
            uint16_t h0 = *(uint16_t*)head, h1 = *(uint16_t*)(head + 6);
            int shift = (h0 >> 6) == 4 && (h1 >> 6) == 12 ? 6 : (h0 >> 1) == 4 && (h1 >> 1) == 12 ? 1 : 0;
            uint32_t current = rd<uint32_t>(p, -8);
            if (!shift || current >= 8192) continue;
            bool ok = rd<uintptr_t>(p, (ptrdiff_t)(current + 1) * 8) == 0;
            for (uint32_t b = 0; ok && b <= current; ++b) ok = rd<uintptr_t>(p, b * 8) != 0;
            if (!ok) continue;
            g.blocks = p, g.nameShift = shift;
            return true;
        }
    return false;
}

bool find_objects(const std::vector<Span>& spans) {
    for (const Span& s : spans)
        for (uint8_t* p = s.begin; p + 0x20 <= s.end; p += 8) {
            int32_t max = rd<int32_t>(p, 0x10), num = rd<int32_t>(p, 0x14), maxChunks = rd<int32_t>(p, 0x18),
                    chunks = rd<int32_t>(p, 0x1C);
            if (num < 1000 || num > max || maxChunks < 1 || max != maxChunks * 0x10000 || chunks < 1 || chunks > maxChunks ||
                num > chunks * 0x10000 || !pointer_like(rd<uintptr_t>(p)))
                continue;
            const uint8_t* chunk0 = rd<const uint8_t*>(rd<const uint8_t*>(p));
            for (int stride : {0x18, 0x10, 0x20}) {
                int good = 0;
                for (int i = 1; i <= 200; ++i) {
                    Obj* o = rd<Obj*>(chunk0, (ptrdiff_t)i * stride);
                    if (o && rd<int32_t>(o, kIndex) == i) ++good;
                }
                if (good < 150) continue;
                g.objects = p, g.stride = stride;
                return true;
            }
        }
    return false;
}

Obj* native_class(const char* name) {
    for (int i = 0, n = count(); i < n; ++i)
        if (Obj* o = at(i); o && rd<Obj*>(o, kClass) == g.uclass && entry(rd<uint32_t>(o, kName)) == name) return o;
    return nullptr;
}

std::string type_of(const Field* f) { return fname(rd<const uint8_t*>(f, 8)); }

const Field* first_prop(const Obj* s) { return rd<const Field*>(s, g.childProps); }
const Field* next_prop(const Field* f) { return rd<const Field*>(f, g.fieldNext); }

// FField: class, owner (16 bytes, or 8 in some UE 5 builds), next, name; FProperty: array size, element size, flags,
// replication, offset, ... then each kind's own (an object property's class: found as RootComponent's SceneComponent).
bool find_fields(Obj* actor, Obj* scene) {
    const Field* f = first_prop(actor);
    if (!f || (rd<uintptr_t>(f, 0x10) & ~1ull) != (uintptr_t)actor) return false;
    g.fieldName = rd<uint8_t>(f, 0x18) == 1 ? 0x28 : 0x20;
    g.fieldNext = g.fieldName - 8;
    int b = g.fieldName + 0x10;
    g.arrayDim = b, g.elemSize = b + 4, g.propFlags = b + 8, g.offset = b + 0x14;
    int size = rd<int32_t>(actor, g.propsSize), last = 0x28, n = 0;
    const Field* root = nullptr;
    for (; f && n < 400; f = next_prop(f), ++n) {
        std::string type = type_of(f), name = fname(f + g.fieldName);
        int32_t off = rd<int32_t>(f, g.offset), dim = rd<int32_t>(f, g.arrayDim), el = rd<int32_t>(f, g.elemSize);
        if (type.size() < 9 || type.compare(type.size() - 8, 8, "Property") || name.empty() || off < last || off >= size ||
            dim < 1 || el < 1)
            return unreal_note("Unreal objects: Actor's property %d (%s %s at %d) doesn't fit", n, type.c_str(), name.c_str(), off), false;
        last = off;
        if (name == "RootComponent") root = f;
    }
    if (!root) return false;
    for (int off = g.offset + 4; off < g.offset + 0x80; off += 4)
        if (rd<Obj*>(root, off) == scene) return g.propEnd = off, true;
    return false;
}

using ProcessEventFn = void (*)(Obj*, Obj*, void*);

bool guarded_call(void* fn, Obj* o, Obj* f, void* params) {
    __try {
        ((ProcessEventFn)fn)(o, f, params);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

const uint8_t* follow(const uint8_t* p) {
    for (int i = 0; i < 4; ++i) {
        if (rd<uint8_t>(p) == 0xE9) p = p + 5 + rd<int32_t>(p, 1);
        else if (rd<uint8_t>(p) == 0xFF && rd<uint8_t>(p, 1) == 0x25) p = rd<const uint8_t*>(p + 6 + rd<int32_t>(p, 2));
        else break;
    }
    return p;
}

Obj* default_object(const Obj* cls) {
    for (int i = 0, n = count(); i < n; ++i)
        if (Obj* o = at(i); o && rd<Obj*>(o, kClass) == cls && (rd<uint32_t>(o, kFlags) & RF_ClassDefaultObject)) return o;
    return nullptr;
}

// ProcessEvent tests FUNC_Native (0x400) in the function's flags early on: test dword [reg+flags], 400h, or
// test byte [reg+flags+1], 4. Candidates in the base object's vtable are tried on Add_IntInt until one adds.
bool find_process_event(Obj* object) {
    Obj* base = default_object(object);
    Obj* math = find_class("KismetMathLibrary");
    Obj* add = math ? func(math, "Add_IntInt") : nullptr;
    Obj* mathDefault = math ? default_object(math) : nullptr;
    if (!base || !add || !mathDefault) return unreal_note("Unreal objects: no Object default or Add_IntInt to test with"), false;
    void** vt = rd<void**>(base);
    uint8_t dword[8], byte[5];
    memcpy(dword, &g.funcFlags, 4), memcpy(dword + 4, "\x00\x04\x00\x00", 4);
    int flagsHigh = g.funcFlags + 1;
    memcpy(byte, &flagsHigh, 4), byte[4] = 4;
    for (int i = 0; i < 0x100; ++i) {
        const uint8_t* fn = follow(rd<const uint8_t*>(vt, i * 8));
        uint8_t code[0x200];
        if (!fn || !peek(fn, code, sizeof(code))) continue;
        bool tests = false;
        for (size_t k = 0; k + 8 <= sizeof(code) && !tests; ++k)
            tests = !memcmp(code + k, dword, 8) || !memcmp(code + k, byte, 5);
        if (!tests) continue;
        Field *a = prop(add, "A"), *b = prop(add, "B"), *r = prop(add, "ReturnValue");
        std::vector<uint8_t> params(params_size(add) + 16);
        if (!a || !b || !r) return false;
        *(int32_t*)&params[prop_offset(a)] = 2, *(int32_t*)&params[prop_offset(b)] = 3;
        bool ran = guarded_call((void*)fn, mathDefault, add, params.data());
        unreal_note("Unreal objects: ProcessEvent candidate slot %d: %s, 2 + 3 = %d", i, ran ? "ran" : "faulted",
                    *(int32_t*)&params[prop_offset(r)]);
        if (ran && *(int32_t*)&params[prop_offset(r)] == 5) return g.processEvent = i, true;
    }
    return false;
}

bool find_all() {
    std::vector<Span> spans = data_spans();
    if (!find_names(spans)) return unreal_note("Unreal objects: no name pool (UE 4.22 and older keep names differently)"), false;
    if (!find_objects(spans)) return unreal_note("Unreal objects: no object array"), false;
    for (int i = 0; i < 64 && !g.uclass; ++i) {
        Obj* c = rd<Obj*>(at(i), kClass);
        Obj* cc = rd<Obj*>(c, kClass);
        if (cc && rd<Obj*>(cc, kClass) == cc) g.uclass = cc;
    }
    if (!g.uclass || entry(rd<uint32_t>(g.uclass, kName)) != "Class") return unreal_note("Unreal objects: no Class class"), false;
    Obj *object = native_class("Object"), *actor = native_class("Actor"), *pawn = native_class("Pawn"),
        *structCls = native_class("Struct"), *scene = native_class("SceneComponent");
    if (!object || !actor || !pawn || !structCls || !scene) return unreal_note("Unreal objects: core classes missing"), false;
    for (int off = 0x30; off <= 0x80 && !g.super; off += 8)
        if (rd<Obj*>(actor, off) == object && rd<Obj*>(pawn, off) == actor) g.super = off;
    g.children = g.super + 8, g.childProps = g.super + 0x10, g.propsSize = g.super + 0x18;
    if (!g.super || rd<int32_t>(object, g.propsSize) != 0x28)
        return unreal_note("Unreal objects: struct layout not found (UE 4.24 and older keep properties as objects)"), false;
    if (!find_fields(actor, scene)) return unreal_note("Unreal objects: property layout not found"), false;
    g.funcFlags = (rd<int32_t>(structCls, g.propsSize) + 7) & ~7;
    // Every class, by name: native ones and blueprints (their class is a subclass of Class).
    for (int i = 0, n = count(); i < n; ++i) {
        Obj* o = at(i);
        if (!o || !inherits(class_of(o), g.uclass)) continue;
        g_classes.emplace(entry(rd<uint32_t>(o, kName)), o);
    }
    if (!find_process_event(object)) return unreal_note("Unreal objects: no ProcessEvent"), false;
    unreal_note("Unreal objects: %d objects, %zu classes; struct super %#x, field name %#x, property offset %#x, end %#x, "
                "function flags %#x, ProcessEvent slot %d", count(), g_classes.size(), g.super, g.fieldName, g.offset,
                g.propEnd, g.funcFlags, g.processEvent);
    return true;
}

Obj* object_prop(const Obj* o, const char* name) {
    const Field* f = o ? prop(class_of(o), name) : nullptr;
    return f ? rd<Obj*>(o, prop_offset(f)) : nullptr;
}

}  // namespace

bool init() {
    if (g_state == 0) {
        ULONGLONG t0 = GetTickCount64();
        g_state = find_all() ? 1 : -1;
        unreal_note("Unreal objects: %s in %llu ms", g_state > 0 ? "ready" : "unavailable", GetTickCount64() - t0);
    }
    return g_state > 0;
}

void set_game_thread() { g_gameThread = GetCurrentThreadId(); }
bool on_game_thread() { return g_gameThread == GetCurrentThreadId(); }

std::string name(const Obj* o) { return o ? fname(o + kName) : std::string(); }
Obj* class_of(const Obj* o) { return rd<Obj*>(o, kClass); }
Obj* outer(const Obj* o) { return rd<Obj*>(o, kOuter); }

bool inherits(const Obj* c, const Obj* base) {
    for (; c; c = rd<Obj*>(c, g.super))
        if (c == base) return true;
    return false;
}

bool is_a(const Obj* o, const Obj* cls) { return inherits(class_of(o), cls); }

bool alive(const Obj* o) {
    int32_t i = rd<int32_t>(o, kIndex);
    return o && i >= 0 && i < count() && at(i) == o;
}

// Kept from garbage collection: the item's RootSet flag (EInternalObjectFlags, 1 << 30, UE 4.25 to 5.5).
bool keep(const Obj* o) {
    if (!alive(o) || g.stride < 0x10) return false;
    int32_t i = rd<int32_t>(o, kIndex);
    uint8_t* chunk = rd<uint8_t*>(rd<const uint8_t*>(g.objects), (ptrdiff_t)(i >> 16) * 8);
    auto* flags = (volatile long*)(chunk + (ptrdiff_t)(i & 0xFFFF) * g.stride + 8);
    _InterlockedOr(flags, 1 << 30);
    return true;
}

std::string path(const Obj* o) {
    std::string out = name(o);
    for (const Obj* outer = rd<Obj*>(o, kOuter); outer; outer = rd<Obj*>(outer, kOuter)) {
        std::string c = name(class_of(outer));
        if (c == "Level" || c == "Package" || c == "World") break;
        out = name(outer) + "." + out;
    }
    return out;
}

Obj* find_class(const std::string& n) {
    auto it = g_classes.find(n);
    return it == g_classes.end() ? nullptr : it->second;
}

std::vector<Obj*> objects_of(const Obj* cls) {
    std::vector<Obj*> out;
    for (int i = 0, n = count(); i < n; ++i)
        if (Obj* o = at(i); o && !(rd<uint32_t>(o, kFlags) & (RF_ClassDefaultObject | RF_ArchetypeObject)) && is_a(o, cls))
            out.push_back(o);
    return out;
}

// Found fields and functions are kept by their struct's address and name, and checked on reuse (a struct unloaded and
// another loaded at its address).
std::string key(const Obj* s, const char* n) { return std::string((const char*)&s, sizeof(s)) + n; }

Field* prop(const Obj* s, const char* n) {
    static std::unordered_map<std::string, Field*> found;
    auto it = found.find(key(s, n));
    if (it != found.end() && it->second && fname(it->second + g.fieldName) == n) {
        const Obj* owner = (const Obj*)(rd<uintptr_t>(it->second, 0x10) & ~1ull);
        for (const Obj* c = s; c; c = rd<Obj*>(c, g.super))
            if (c == owner) return it->second;
    }
    Field* out = nullptr;
    for (const Obj* c = s; c && !out; c = rd<Obj*>(c, g.super))
        for (const Field* f = first_prop(c); f && !out; f = next_prop(f))
            if (fname(f + g.fieldName) == n) out = (Field*)f;
    if (out) found[key(s, n)] = out;
    return out;
}

std::string prop_type(const Field* f) { return type_of(f); }
int32_t prop_offset(const Field* f) { return rd<int32_t>(f, g.offset); }
int32_t params_size(const Obj* fn) { return rd<int32_t>(fn, g.propsSize); }

Obj* func(const Obj* cls, const char* n) {
    static std::unordered_map<std::string, Obj*> found;
    auto it = found.find(key(cls, n));
    if (it != found.end() && alive(it->second) && name(it->second) == n) return it->second;
    for (const Obj* c = cls; c; c = rd<Obj*>(c, g.super))
        for (Obj* f = rd<Obj*>(c, g.children); f; f = rd<Obj*>(f, kNext))
            if (name(f) == n) return found[key(cls, n)] = f;
    return nullptr;
}

Obj* statics(const char* cls) {
    static std::unordered_map<std::string, Obj*> found;
    Obj*& o = found[cls];
    if (!alive(o)) o = find_class(cls) ? default_object(find_class(cls)) : nullptr;
    return o;
}

bool call(Obj* o, Obj* fn, void* params) {
    if (g.processEvent < 0 || !o || !fn) return false;
    return guarded_call(rd<void*>(rd<void**>(o), g.processEvent * 8), o, fn, params);
}

Obj* engine() {
    static Obj* cached;
    if (cached && alive(cached)) return cached;
    Obj* cls = find_class("GameEngine");
    if (!cls) cls = find_class("Engine");
    auto all = cls ? objects_of(cls) : std::vector<Obj*>{};
    return cached = all.empty() ? nullptr : all[0];
}

Obj* world() { return object_prop(object_prop(engine(), "GameViewport"), "World"); }

Obj* player_controller() {
    Obj* gi = object_prop(world(), "OwningGameInstance");
    const Field* f = gi ? prop(class_of(gi), "LocalPlayers") : nullptr;
    if (!f || rd<int32_t>(gi, prop_offset(f) + 8) < 1) return nullptr;
    return object_prop(rd<Obj*>(rd<Obj**>(gi, prop_offset(f))), "PlayerController");
}

Obj* pawn() { return object_prop(player_controller(), "Pawn"); }
Obj* camera_manager() { return object_prop(player_controller(), "PlayerCameraManager"); }

// --- Scripts ------------------------------------------------------------------------------------------------------------
namespace {
const Obj* struct_of(const Field* f) { return rd<const Obj*>(f, g.propEnd); }

bool is_int(const std::string& t) {
    return t == "Int8Property" || t == "Int16Property" || t == "IntProperty" || t == "Int64Property" || t == "ByteProperty" ||
           t == "UInt16Property" || t == "UInt32Property" || t == "UInt64Property" || t == "EnumProperty";
}
bool is_plain(const std::string& t) {
    return is_int(t) || t == "FloatProperty" || t == "DoubleProperty" || t == "BoolProperty" || t == "StructProperty";
}

// A property's value at `base` (the object or struct holding it) onto the Lua stack: numbers, booleans, names, strings,
// objects (as their paths) and structs of numbers (a vector: three). Returns how many values.
int push_value(lua_State* L, const Field* f, const uint8_t* base) {
    std::string t = type_of(f);
    const uint8_t* p = base + prop_offset(f);
    int size = rd<int32_t>(f, g.elemSize);
    if (t == "BoolProperty") return lua_pushboolean(L, (rd<uint8_t>(p, rd<uint8_t>(f, g.propEnd + 1)) & rd<uint8_t>(f, g.propEnd + 2)) != 0), 1;
    if (t == "FloatProperty") return lua_pushnumber(L, rd<float>(p)), 1;
    if (t == "DoubleProperty") return lua_pushnumber(L, rd<double>(p)), 1;
    if (is_int(t)) {
        int64_t v = 0;
        peek(p, &v, size < 8 ? size : 8);
        if (t.rfind("Int", 0) == 0 && size < 8) v = (v << (64 - size * 8)) >> (64 - size * 8);  // signed: extend
        return lua_pushinteger(L, v), 1;
    }
    if (t == "NameProperty") return lua_pushstring(L, fname(p).c_str()), 1;
    if (t == "StrProperty") {
        int n = rd<int32_t>(p, 8) - 1;
        std::wstring w(n > 0 && n < 65536 ? n : 0, L'\0');
        if (!w.empty()) peek(rd<const wchar_t*>(p), w.data(), w.size() * 2);
        return lua_pushstring(L, utf8(w.data(), (int)w.size()).c_str()), 1;
    }
    if (t == "ObjectProperty" || t == "ClassProperty") {
        Obj* o = rd<Obj*>(p);
        o ? (void)lua_pushstring(L, path(o).c_str()) : lua_pushnil(L);
        return 1;
    }
    if (t == "StructProperty") {
        int n = 0;
        for (const Field* c = first_prop(struct_of(f)); c; c = next_prop(c))
            if (is_plain(type_of(c))) n += push_value(L, c, p);
        return n;
    }
    script::fail(L, "%s is a %s, which scripts can't read", fname(f + g.fieldName).c_str(), t.c_str());
}

Obj* resolve(const std::string& at);

// Lua values from argument i on into the property at `base`; returns the next argument.
int read_value(lua_State* L, int i, const Field* f, uint8_t* base) {
    std::string t = type_of(f);
    uint8_t* p = base + prop_offset(f);
    int size = rd<int32_t>(f, g.elemSize);
    if (t == "BoolProperty") {
        uint8_t& b = p[rd<uint8_t>(f, g.propEnd + 1)];
        uint8_t mask = rd<uint8_t>(f, g.propEnd + 2);
        b = lua_toboolean(L, i) ? b | mask : b & ~mask;
    } else if (t == "FloatProperty") {
        *(float*)p = (float)luaL_checknumber(L, i);
    } else if (t == "DoubleProperty") {
        *(double*)p = luaL_checknumber(L, i);
    } else if (is_int(t)) {
        int64_t v = (int64_t)luaL_checknumber(L, i);
        memcpy(p, &v, size < 8 ? size : 8);
    } else if (t == "ObjectProperty") {
        Obj* o = lua_isnil(L, i) ? nullptr : resolve(luaL_checkstring(L, i));
        if (!lua_isnil(L, i) && !o) script::fail(L, "no object at %s", lua_tostring(L, i));
        Obj* cls = rd<Obj*>(f, g.propEnd);
        if (o && !is_a(o, cls)) script::fail(L, "%s is not a %s", lua_tostring(L, i), name(cls).c_str());
        *(Obj**)p = o;
    } else if (t == "StructProperty") {
        for (const Field* c = first_prop(struct_of(f)); c; c = next_prop(c))
            if (is_plain(type_of(c))) i = read_value(L, i, c, p);
        return i;
    } else {
        script::fail(L, "%s is a %s, which scripts can't write", fname(f + g.fieldName).c_str(), t.c_str());
    }
    return i + 1;
}

// Calls fn on o with the Lua arguments from `first` on as its inputs (in order; missing ones stay zero), and pushes its
// return value.
int invoke(lua_State* L, Obj* o, const char* fnName, int first) {
    Obj* fn = func(class_of(o), fnName);
    if (!fn) script::fail(L, "%s has no function %s", path(o).c_str(), fnName);
    std::vector<uint64_t> params((params_size(fn) + 15) / 8);
    uint8_t* block = (uint8_t*)params.data();
    const Field* ret = nullptr;
    int i = first, top = lua_gettop(L);
    for (const Field* f = first_prop(fn); f; f = next_prop(f)) {
        uint64_t flags = rd<uint64_t>(f, g.propFlags);
        if (!(flags & CPF_Parm)) continue;
        if (flags & CPF_ReturnParm) ret = f;
        else if ((flags & CPF_OutParm) && !(flags & CPF_ConstParm)) continue;  // an output
        else if (i <= top) i = read_value(L, i, f, block);
    }
    if (!call(o, fn, block)) script::fail(L, "%s: %s failed", path(o).c_str(), fnName);
    return ret ? push_value(L, ret, block) : 0;
}

// Paths: "Pawn", "PlayerController", "CameraManager", "World", or an object's name ("BP_Door_C_2"), with its outers
// before it if needed ("BP_Door_C_2.Mesh"); then "/" and a property or component of it ("Pawn/Mesh"). Found objects
// are kept (until they go); misses are tried again after a second.
struct Found {
    Obj* o = nullptr;
    ULONGLONG missedAt = 0;
};
std::unordered_map<std::string, Found> g_found;

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    for (size_t a = 0, b; a <= s.size(); a = b + 1) {
        b = s.find(sep, a);
        if (b == std::string::npos) b = s.size();
        out.push_back(s.substr(a, b - a));
    }
    return out;
}

bool matches(const Obj* o, const std::vector<std::string>& names) {
    for (size_t k = names.size(); k-- > 0; o = rd<Obj*>(o, kOuter))
        if (!o || name(o) != names[k]) return false;
    return true;
}

// An object by its name (and outers), under `outer` when given.
// ponytail: compares every object's name as text (~10 ms in a 50K-object game); compare name ids if misses repeat often.
Obj* search(const Obj* outer, const std::string& dotted) {
    std::vector<std::string> names = split(dotted, '.');
    for (int i = 0, n = count(); i < n; ++i) {
        Obj* o = at(i);
        if (!o || (rd<uint32_t>(o, kFlags) & (RF_ClassDefaultObject | RF_ArchetypeObject)) || !matches(o, names)) continue;
        if (!outer || rd<Obj*>(o, kOuter) == outer) return o;
    }
    return nullptr;
}

Obj* resolve(const std::string& at) {
    Found& c = g_found[at];
    if (c.o && alive(c.o)) return c.o;
    if (!c.o && c.missedAt && GetTickCount64() - c.missedAt < 1000) return nullptr;
    Obj* o = nullptr;
    std::vector<std::string> segs = split(at, '/');
    for (size_t k = 0; k < segs.size(); ++k) {
        const std::string& seg = segs[k];
        if (k == 0) {
            o = seg == "Pawn" ? pawn() : seg == "PlayerController" ? player_controller() : seg == "CameraManager" ? camera_manager()
              : seg == "World" ? world() : find_class(seg) ? statics(seg.c_str())  // a class: its static functions
              : search(nullptr, seg);
        } else {
            const Field* f = prop(class_of(o), seg.c_str());
            o = f && type_of(f) == "ObjectProperty" ? rd<Obj*>(o, prop_offset(f)) : search(o, seg);
        }
        if (!o) break;
    }
    c = {o, o ? 0 : GetTickCount64()};
    return o;
}

// The object a call names, or null off the game thread (between 3D frames the calls do nothing).
Obj* target(lua_State* L) {
    const char* at = luaL_checkstring(L, 1);
    if (!on_game_thread() || !init()) return nullptr;
    Obj* o = resolve(at);
    if (!o) script::fail(L, "no object at %s", at);
    return o;
}

bool is_actor(const Obj* o) {
    static Obj* actor = find_class("Actor");
    return is_a(o, actor);
}

Obj* scene_or_actor(lua_State* L) {
    Obj* o = target(L);
    static Obj* scene = find_class("SceneComponent");
    if (o && !is_actor(o) && !is_a(o, scene)) script::fail(L, "%s is neither an actor nor a scene component", lua_tostring(L, 1));
    return o;
}
// The numbers of a property at `base` (a number, or a struct's numbers in order), read into or written from v.
int numbers(const Field* f, uint8_t* base, double* v, int max, bool write) {
    std::string t = type_of(f);
    uint8_t* p = base + prop_offset(f);
    if (t == "StructProperty") {
        int n = 0;
        for (const Field* c = first_prop(struct_of(f)); c; c = next_prop(c))
            if (is_plain(type_of(c))) n += numbers(c, p, v + n, max - n, write);
        return n;
    }
    if (max < 1) return 0;
    int size = rd<int32_t>(f, g.elemSize);
    if (t == "BoolProperty") {
        uint8_t& b = p[rd<uint8_t>(f, g.propEnd + 1)];
        uint8_t mask = rd<uint8_t>(f, g.propEnd + 2);
        if (write) b = v[0] != 0 ? b | mask : b & ~mask;
        else v[0] = (b & mask) ? 1 : 0;
    } else if (t == "FloatProperty") {
        if (write) *(float*)p = (float)v[0];
        else v[0] = *(float*)p;
    } else if (t == "DoubleProperty") {
        if (write) *(double*)p = v[0];
        else v[0] = *(double*)p;
    } else if (is_int(t)) {
        int64_t i = 0;
        if (write) i = (int64_t)v[0], memcpy(p, &i, size < 8 ? size : 8);
        else memcpy(&i, p, size < 8 ? size : 8), v[0] = (double)i;
    } else {
        return 0;
    }
    return 1;
}
}  // namespace

Invoke::Invoke(Obj* o, const char* name) : target(o), fn(o ? func(class_of(o), name) : nullptr) {
    if (fn) block.resize((params_size(fn) + 15) / 8);
}

Invoke& Invoke::set(const char* param, std::initializer_list<double> values) {
    if (const Field* f = fn ? prop(fn, param) : nullptr) numbers(f, (uint8_t*)block.data(), (double*)values.begin(), (int)values.size(), true);
    return *this;
}

Invoke& Invoke::set(const char* param, Obj* value) {
    if (const Field* f = fn ? prop(fn, param) : nullptr) *(Obj**)((uint8_t*)block.data() + prop_offset(f)) = value;
    return *this;
}

bool Invoke::run() { return fn && call(target, fn, block.data()); }

int Invoke::get(const char* param, double* out, int max) const {
    const Field* f = fn ? prop(fn, param) : nullptr;
    return f ? numbers(f, (uint8_t*)block.data(), out, max, false) : 0;
}

Obj* Invoke::get(const char* param) const {
    const Field* f = fn ? prop(fn, param) : nullptr;
    return f ? rd<Obj*>(block.data(), prop_offset(f)) : nullptr;
}

int get_numbers(const Obj* o, const char* property, double* out, int max) {
    const Field* f = o ? prop(class_of(o), property) : nullptr;
    return f ? numbers(f, (uint8_t*)o, out, max, false) : 0;
}

Obj* get_object(const Obj* o, const char* property) { return object_prop(o, property); }

std::vector<Obj*> get_objects(const Obj* o, const char* property) {
    const Field* f = o ? prop(class_of(o), property) : nullptr;
    std::vector<Obj*> out;
    if (!f || type_of(f) != "ArrayProperty") return out;
    Obj** items = rd<Obj**>(o, prop_offset(f));
    int n = rd<int32_t>(o, prop_offset(f) + 8);
    for (int i = 0; i < n && i < 4096; ++i) out.push_back(rd<Obj*>(items, i * 8));
    return out;
}

Obj* find(const std::string& at) { return on_game_thread() && init() ? resolve(at) : nullptr; }

void script_calls(lua_State* L) {
    auto add = [&](const char* n, lua_CFunction f) { lua_pushcfunction(L, f), lua_setfield(L, -2, n); };
    add("find", [](lua_State* L) {
        const char* at = luaL_checkstring(L, 1);
        return lua_pushboolean(L, on_game_thread() && init() && resolve(at) != nullptr), 1;
    });
    add("hide", [](lua_State* L) {
        Obj* o = scene_or_actor(L);
        if (!o) return 0;
        bool hidden = lua_isnone(L, 2) || lua_toboolean(L, 2);
        lua_settop(L, 1);
        lua_pushboolean(L, hidden);
        if (is_actor(o)) return invoke(L, o, "SetActorHiddenInGame", 2), 0;
        lua_pushboolean(L, true);  // its children too
        return invoke(L, o, "SetHiddenInGame", 2), 0;
    });
    add("position", [](lua_State* L) {
        Obj* o = scene_or_actor(L);
        return o ? invoke(L, o, is_actor(o) ? "K2_GetActorLocation" : "K2_GetComponentLocation", 2) : 0;
    });
    add("set_position", [](lua_State* L) {
        Obj* o = scene_or_actor(L);
        if (!o) return 0;
        lua_settop(L, 4);
        lua_pushboolean(L, false), lua_pushboolean(L, true);  // no sweep, teleport
        return invoke(L, o, is_actor(o) ? "K2_SetActorLocation" : "K2_SetWorldLocation", 2), 0;
    });
    add("rotation", [](lua_State* L) {
        Obj* o = scene_or_actor(L);
        return o ? invoke(L, o, is_actor(o) ? "K2_GetActorRotation" : "K2_GetComponentRotation", 2) : 0;
    });
    add("set_rotation", [](lua_State* L) {
        Obj* o = scene_or_actor(L);
        if (!o) return 0;
        lua_settop(L, 4);
        if (is_actor(o)) return lua_pushboolean(L, true), invoke(L, o, "K2_SetActorRotation", 2), 0;
        lua_pushboolean(L, false), lua_pushboolean(L, true);
        return invoke(L, o, "K2_SetWorldRotation", 2), 0;
    });
    add("field", [](lua_State* L) {
        Obj* o = target(L);
        if (!o) return 0;
        std::string at = luaL_checkstring(L, 2);
        const Field* f = nullptr;
        uint8_t* base = o;
        const Obj* s = class_of(o);
        for (const std::string& seg : split(at, '.')) {
            if (f) {
                if (type_of(f) != "StructProperty") script::fail(L, "%s is not a struct", fname(f + g.fieldName).c_str());
                base += prop_offset(f), s = struct_of(f);
            }
            f = prop(s, seg.c_str());
            if (!f) script::fail(L, "%s has no property %s", lua_tostring(L, 1), at.c_str());
        }
        if (lua_gettop(L) >= 3) return read_value(L, 3, f, base), 0;
        return push_value(L, f, base);
    });
    add("call", [](lua_State* L) {
        Obj* o = target(L);
        return o ? invoke(L, o, luaL_checkstring(L, 2), 3) : 0;
    });
}

}  // namespace ue
