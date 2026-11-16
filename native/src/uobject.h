#pragma once
// Unreal's object system, read from the game's memory: the global object list, names, classes, properties and
// functions, and ProcessEvent to call any UFunction. Nothing is hard-coded per version: each layout (UE 4.25 to 5.x) is
// found at runtime and checked against objects every engine has (Object, Actor, Pawn). Game thread only.
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

struct lua_State;

namespace ue {
using Obj = uint8_t;    // a UObject
using Field = uint8_t;  // an FProperty

// Finds the layout once (the first call scans; later calls return the answer). False: not found, and the log says why.
bool init();
// The game thread, where objects may be used: set by the adapter from the engine's own calls there.
void set_game_thread();
bool on_game_thread();

std::string name(const Obj* o);         // "BP_Player_C_0"
std::string path(const Obj* o);         // outers below the level or package: "BP_Player_C_0.CharacterMesh0"
Obj* class_of(const Obj* o);
Obj* outer(const Obj* o);
bool is_a(const Obj* o, const Obj* cls);       // o's class is cls or a subclass
bool inherits(const Obj* c, const Obj* base);  // class c is base or a subclass
bool alive(const Obj* o);               // still in the object list
bool keep(const Obj* o);                // never garbage collected (in the root set)
Obj* find_class(const std::string& name);
// Live objects (not class defaults or archetypes) of a class or its subclasses.
std::vector<Obj*> objects_of(const Obj* cls);

Field* prop(const Obj* cls, const char* name);  // on the class or its supers
std::string prop_type(const Field* f);           // "ObjectProperty", "FloatProperty", ...
int32_t prop_offset(const Field* f);
Obj* func(const Obj* cls, const char* name);
// Calls fn on o with a parameter block laid out as fn's properties (prop/params). False when ProcessEvent isn't known.
bool call(Obj* o, Obj* fn, void* params);
int32_t params_size(const Obj* fn);

// A class's default object, for static functions ("KismetSystemLibrary").
Obj* statics(const char* cls);
// An object by its script path (SCRIPTING.md: "Pawn/Mesh", "BP_Door_C_2"); null off the game thread.
Obj* find(const std::string& path);
// A property of o: its numbers (a number, or a struct's: a vector's three), an object, or an array of objects.
int get_numbers(const Obj* o, const char* property, double* out, int max);
Obj* get_object(const Obj* o, const char* property);
std::vector<Obj*> get_objects(const Obj* o, const char* property);

// A UFunction call from C++: inputs set by name (numbers, structs of numbers, objects), then run(), then outputs read by
// name. Unset inputs stay zero.
struct Invoke {
    Invoke(Obj* target, const char* fn);
    explicit operator bool() const { return fn != nullptr; }
    Invoke& set(const char* param, std::initializer_list<double> values);
    Invoke& set(const char* param, Obj* value);
    bool run();
    int get(const char* param, double* out, int max) const;
    Obj* get(const char* param) const;
    Obj *target = nullptr, *fn = nullptr;
    std::vector<uint64_t> block;
};

// The local player's objects (null when there is none yet).
Obj* engine();
Obj* world();
Obj* player_controller();
Obj* pawn();
Obj* camera_manager();

// Scripts: find, hide, position, rotation, field and call on the game's objects (SCRIPTING.md).
void script_calls(lua_State* L);
}  // namespace ue
