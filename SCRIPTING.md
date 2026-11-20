# Per-game scripts

Some games need one fix the automatic handling cannot guess: a different camera, an object that should not show in VR,
eyes a little higher, a game option that breaks stereo. A Lua script does that for one game. Scripts work in Unity,
Godot 4.3+ and Unreal games: the same files, callbacks and shared calls everywhere, plus calls for the engine's own
objects (Unity's GameObjects, Godot's nodes, Unreal's actors and components).

## Where scripts go

In the app, open the game, then **04 Scripts → Import script**. Or put `.lua` files in
`%APPDATA%\PortXR\scripts\<game exe name>\` yourself (**Open scripts folder** opens it). For `Hollow Knight.exe` that
is `%APPDATA%\PortXR\scripts\Hollow Knight\`. Unreal games run a second exe, `<Game>-Win64-Shipping.exe`, and the
folder is named after that one (the app's button opens the right folder).

- Scripts outlive an uninstall: they are not in the game folder.
- A game loads its scripts when VR starts, and reloads them within two seconds of a file changing, so you can edit
  while playing.
- Scripts are sandboxed: no files, no programs, no network. Only the `portxr` table below and plain Lua
  (`string`, `math`, `table`, `utf8`).
- Errors go to the game's log with the file and line (**Open log** in the app): BepInEx's `LogOutput.log` in Unity
  games, `PortXR\portxr.log` beside the game's exe in Godot games and beside the Shipping exe in Unreal games. A
  script that fails is switched off until its file changes; one that runs over a second (an endless loop) is stopped,
  so it can't freeze the game.

## Callbacks

Define any of these as global functions:

| Function | Called |
|---|---|
| `on_frame(dt)` | every frame; `dt` in seconds |
| `on_scene(name)` | when the active scene changes, and once at load. Unity: the scene's name; Godot: the scene file's name (`level_2` for `res://levels/level_2.tscn`); Unreal: the world's (map's) name |
| `on_camera(name)` | when PortXR picks a new head camera (Unreal: the camera component the view follows) |

## The `portxr` table

### Every engine

| Call | Does |
|---|---|
| `log(text)` | writes to the game's log |
| `get(key)`, `set(key, value)` | reads or changes a PortXR setting by its key in the game's settings file: `WorldScale`, `Height`, `HeadAim`, `CameraName`, `Display` (`Stereo`, `Parallax`, `Theater`), `Depth`, `ScreenYaw`, `ScreenPitch`, `ScreenDistance`, `ScreenWidth`, `Mode`… |
| `scene()` | the active scene's name (as `on_scene` gets it) |
| `camera()` | the head camera's name, or nil |
| `time()` | seconds since scripts started |
| `key(vk)` | whether a key is held (Windows virtual-key code: `0x70` is F1) |
| `eye_offset(x, y, z)` | moves both eyes, metres in the player's frame (x right, y up, z forward) |
| `crosshair_area(w, h)` | how much of the middle of the game's UI moves with the crosshair while aiming (Crosshair: Game's), as fractions of the screen; default 0.18 x 0.16. Widen it for prompts that get cut |


### Unity games

Object paths are as `GameObject.Find` takes them: a name (`"Player"`) or a path (`"_GameCameras/HudCamera"`). Only
active objects are found. A path that matches nothing raises an error.

| Call | Does |
|---|---|
| `find(path)` | whether an active object exists there |
| `active(path [, on])` | reads, or sets, whether the object is active |
| `hide(path [, hidden])` | stops (or with `false`, restarts) the renderers under the object drawing; the object keeps running |
| `position(path)`, `set_position(path, x, y, z)` | world position |
| `rotation(path)`, `set_rotation(path, x, y, z)` | world rotation, Euler degrees |
| `enabled(path, type [, on])` | a component's `enabled`, by type name (`"Light"`, `"PostProcessLayer"`) |
| `field(path, type, member [, value])` | reads, or writes, one field or property of a component: numbers, booleans, strings, enums (number or name) |

### Godot games

A node is found by its name anywhere in the tree (`"Player"`), by a path from the current scene or the root (`"Player/Head"`, `"Main/Player/Head"`), or
by an absolute path (`"/root/Main/Player"`). A path that matches nothing raises an error.

| Call | Does |
|---|---|
| `find(path)` | whether a node exists there |
| `active(path [, on])` | reads, or sets, whether the node runs and shows (off: hidden, process mode Disabled) |
| `hide(path [, hidden])` | stops (or with `false`, restarts) the node drawing; it keeps running |
| `position(path)`, `set_position(path, x, y, z)` | global position (a 2D node: x, y) |
| `rotation(path)`, `set_rotation(path, x, y, z)` | global rotation, degrees (a 2D node: its rotation as z) |
| `field(path, property [, value])` | reads, or writes, any property of the node. Numbers, booleans and strings as they are; vectors read as three numbers; other types as Godot writes them (`"Color(1, 0, 0, 1)"`) |
| `call(path, method, ...)` | calls a method of the node with numbers, booleans or strings, and returns what it returns |

### Unreal games

An object is found by a name the player's objects start from (`"Pawn"`, `"PlayerController"`, `"CameraManager"`,
`"World"`), or by its object name (`"BP_Door_C_2"`, with its owner before it when the name alone is not unique:
`"BP_Door_C_2.StaticMeshComponent0"`). After a `/`, a property or component of it: `"Pawn/Mesh"`. A class's name
(`"GameplayStatics"`, `"KismetSystemLibrary"`) reaches its static functions through `call`. Paths that
`field` and `call` return for objects work as paths. A path that matches nothing raises an error. Unreal 4.25 and newer
(4.22 to 4.24 keep their properties another way: there these calls are missing).

| Call | Does |
|---|---|
| `find(path)` | whether an object exists there |
| `hide(path [, hidden])` | stops (or with `false`, restarts) an actor or component drawing, with what it carries; it keeps running |
| `position(path)`, `set_position(path, x, y, z)` | world position of an actor or component, Unreal units (centimetres; z is up) |
| `rotation(path)`, `set_rotation(path, pitch, yaw, roll)` | world rotation, degrees |
| `field(path, property [, value...])` | reads, or writes, a property; `"A.B"` reaches into a struct. Numbers and booleans as they are, names and strings read as text, objects as paths, structs as their numbers (a vector: three) |
| `call(path, function, ...)` | calls a function of the object (any `UFUNCTION`, by name) with numbers, booleans, strings, names, objects (paths) or structs (their numbers in order), and returns what it returns |

Console variables: the game's `Console` setting (Advanced in the app) runs its commands, separated by `;`, as each
level starts. From a script: `portxr.call("KismetSystemLibrary", "ExecuteConsoleCommand", "World", "r.MotionBlurQuality 0")`;
read one back with `GetConsoleVariableIntValue` (or `Float`, `Bool`). Shipping games ignore cheat variables.

Objects are reached while the game draws its world; between those frames (a loading screen) `find` is false and the
other calls do nothing.

## Examples

Use the game's own player camera, not its cutscene camera:

```lua
function on_scene(name)
  if name ~= "Menu" then portxr.set("CameraName", "PlayerCamera") end
end
```

Godot: hide the arms model the camera carries, and slow the game's own head bob:

```lua
function on_scene(name)
  if portxr.find("Arms") then portxr.hide("Arms") end
  if portxr.find("Player") then portxr.field("Player", "bob_amount", 0) end
end
```

Unreal: hide the first-person arms and walk slower, for comfort:

```lua
function on_scene(name)
  if portxr.find("Pawn/Mesh1P") then portxr.hide("Pawn/Mesh1P") end
  if portxr.find("Pawn/CharacterMovement") then portxr.field("Pawn/CharacterMovement", "MaxWalkSpeed", 400) end
end
```

Unity: hide the vignette quad stuck to the camera and turn off a depth-of-field effect:

```lua
function on_camera(name)
  portxr.hide("Main Camera/Vignette")
  portxr.enabled("Main Camera", "DepthOfField", false)
end
```

Eyes 10 cm higher and a little forward, out of the character's head:

```lua
portxr.eye_offset(0, 0.10, 0.05)
```

F1 switches between Stereo and Parallax:

```lua
local was = false
function on_frame(dt)
  local down = portxr.key(0x70)
  if down and not was then
    portxr.set("Display", portxr.get("Display") == "Parallax" and "Stereo" or "Parallax")
  end
  was = down
end
```
