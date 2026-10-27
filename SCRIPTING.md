# Per-game scripts

Some games need one fix the automatic handling cannot guess: a different camera, an object that should not show in VR,
eyes a little higher, a game option that breaks stereo. A Lua script does that for one game.

## Where scripts go

In the app, open the game, then **04 Scripts → Import script**. Or put `.lua` files in
`%APPDATA%\Parallax\scripts\<game exe name>\` yourself (**Open scripts folder** opens it). For `Hollow Knight.exe` that
is `%APPDATA%\Parallax\scripts\Hollow Knight\`.

- Scripts outlive an uninstall: they are not in the game folder.
- A game loads its scripts when VR starts, and reloads them within two seconds of a file changing, so you can edit
  while playing.
- Scripts are sandboxed: no files, no programs, no network. Only the `parallax` table below and plain Lua
  (`string`, `math`, `table`).
- Errors go to the game's log (**Open log** in the app) with the file and line. A script that fails is switched off
  until its file changes.

## Callbacks

Define any of these as global functions:

| Function | Called |
|---|---|
| `on_frame(dt)` | every frame; `dt` in seconds |
| `on_scene(name)` | when the active scene changes, and once at load |
| `on_camera(name)` | when Parallax picks a new head camera |

## The `parallax` table

Object paths are as `GameObject.Find` takes them: a name (`"Player"`) or a path (`"_GameCameras/HudCamera"`). Only
active objects are found. A path that matches nothing raises an error.

| Call | Does |
|---|---|
| `log(text)` | writes to the game's log |
| `get(key)`, `set(key, value)` | reads or changes a Parallax setting by its key in `parallax.core.cfg`: `WorldScale`, `Height`, `HeadAim`, `CameraName`, `Theater`, `Mode`… |
| `scene()` | the active scene's name |
| `camera()` | the head camera's name, or nil |
| `time()` | seconds since the game started |
| `key(vk)` | whether a key is held (Windows virtual-key code: `0x70` is F1) |
| `find(path)` | whether an active object exists there |
| `active(path [, on])` | reads, or sets, whether the object is active |
| `hide(path [, hidden])` | stops (or with `false`, restarts) the renderers under the object drawing; the object keeps running |
| `position(path)`, `set_position(path, x, y, z)` | world position |
| `rotation(path)`, `set_rotation(path, x, y, z)` | world rotation, Euler degrees |
| `enabled(path, type [, on])` | a component's `enabled`, by type name (`"Light"`, `"PostProcessLayer"`) |
| `field(path, type, member [, value])` | reads, or writes, one field or property of a component: numbers, booleans, strings, enums (number or name) |
| `eye_offset(x, y, z)` | moves both eyes, metres in the player's frame (x right, y up, z forward) |

## Examples

Use the game's own player camera, not its cutscene camera:

```lua
function on_scene(name)
  if name ~= "Menu" then parallax.set("CameraName", "PlayerCamera") end
end
```

Hide the vignette quad stuck to the camera and turn off a depth-of-field effect:

```lua
function on_camera(name)
  parallax.hide("Main Camera/Vignette")
  parallax.enabled("Main Camera", "DepthOfField", false)
end
```

Eyes 10 cm higher and a little forward, out of the character's head:

```lua
parallax.eye_offset(0, 0.10, 0.05)
```

F1 toggles theater mode:

```lua
local was = false
function on_frame(dt)
  local down = parallax.key(0x70)
  if down and not was then
    parallax.set("Theater", parallax.get("Theater") == "true" and "false" or "true")
  end
  was = down
end
```
