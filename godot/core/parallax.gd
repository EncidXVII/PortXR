# Parallax in Godot 4.3+ games: the engine adapter for the shared VR core (parallax_godot.dll, a GDExtension loaded at
# runtime from beside this script). override.cfg beside the game's exe loads this script as an autoload and puts Godot
# on Direct3D 12; Godot's own OpenXR stays off, as the core owns the VR session as it does in Unity games.
#
# The adapter only does what needs the engine: finds the game's camera, renders the two eyes where the core places
# them, and hands the core the eye images and the game's 2D UI. Settings, placement, the UI panel and the headset
# layers are the core's (native/src: view, settings, xr), shared with Unity and every engine after it.
extends Node

const LOG := "Parallax: "

var core: RefCounted
var _log_file: FileAccess  # ParallaxVR/parallax.log, which the app opens (Open log)
var _views: Array[SubViewport] = []
var _cams: Array[Camera3D] = []
var _attached := false
var _placed := false
var _frames := 0
var _eye_size := Vector2i.ZERO
var _last_report := 0
var _game: Camera3D
var _searched_at := -10000
var _view: SubViewport
var _view_picture: Control
var _hidden: CanvasItem
var _hidden_alpha := 1.0
var _pad_game := false
var _pad_sent := {}
var _aiming := false
var _udp: PacketPeerUDP
var _menu_open := false
var _mode := 0  # the core's display: 0 Stereo, 1 Parallax, 2 Theater
var _focus := 0.0
var _handles := false
var _picked_camera := ""
var _camera_names: Array[String] = []
var _pointer := Vector2(-1, -1)
var _pointer_down := false
# The Game tab's tools, as in Unity games. Weapon in hand: the camera's children moved this frame, and where they were.
var _held: Array[Node3D] = []
var _held_at: Array[Vector3] = []
# Hide objects: the tool has the laser; what it points at; where the beam ends (metres); the scene's visuals.
var _hiding := false
var _hide_target: VisualInstance3D
var _hide_reach := 3.0
var _visuals: Array[Node] = []
var _visuals_at := -10000
var _trigger_was := true
var _hid: Array[VisualInstance3D] = []
var _rehide_at := 0
# XInput button bits to Godot's pad buttons.
const PAD_BUTTONS := {0x1000: JOY_BUTTON_A, 0x2000: JOY_BUTTON_B, 0x4000: JOY_BUTTON_X, 0x8000: JOY_BUTTON_Y,
	0x10: JOY_BUTTON_START, 0x20: JOY_BUTTON_BACK, 0x40: JOY_BUTTON_LEFT_STICK, 0x80: JOY_BUTTON_RIGHT_STICK,
	0x100: JOY_BUTTON_LEFT_SHOULDER, 0x200: JOY_BUTTON_RIGHT_SHOULDER, 0x1: JOY_BUTTON_DPAD_UP, 0x2: JOY_BUTTON_DPAD_DOWN,
	0x4: JOY_BUTTON_DPAD_LEFT, 0x8: JOY_BUTTON_DPAD_RIGHT}


func _ready() -> void:
	process_mode = Node.PROCESS_MODE_ALWAYS
	process_priority = 10000  # after the game has moved its camera this frame
	var dir := (get_script() as Script).resource_path.get_base_dir()
	_log_file = FileAccess.open(dir.path_join("parallax.log"), FileAccess.WRITE)
	GDExtensionManager.load_extension(dir.path_join("parallax.gdextension"))
	if not ClassDB.class_exists("ParallaxCore"):
		_say(str("the VR core did not load; the game stays flat"))
		return
	if RenderingServer.get_current_rendering_driver_name() != "d3d12":
		_say(str("VR needs Direct3D 12 (this game runs ", RenderingServer.get_current_rendering_driver_name(), "); the game stays flat"))
		return
	core = ClassDB.instantiate("ParallaxCore")
	core.open_settings(dir.path_join("parallax.cfg"))
	var info: Dictionary = core.start(ProjectSettings.get_setting("application/config/name", "Godot game"))
	_eye_size = Vector2i(info["width"], info["height"])
	_log()
	if not info["ok"]:
		core = null
		return
	var scale := float(core.render_scale())
	for e in 2:
		var v := SubViewport.new()
		v.size = Vector2i(int(info["width"] * scale), int(info["height"] * scale))
		v.render_target_update_mode = SubViewport.UPDATE_ALWAYS
		var c := Camera3D.new()
		c.projection = Camera3D.PROJECTION_FRUSTUM
		c.keep_aspect = Camera3D.KEEP_HEIGHT
		v.add_child(c)
		add_child(v)
		c.current = true
		_views.append(v)
		_cams.append(c)
	# The game's window keeps only its 2D UI, which goes to the headset as the UI panel.
	var root := get_tree().root
	root.disable_3d = true
	root.transparent_bg = true
	RenderingServer.frame_post_draw.connect(_after_draw)
	core.set_window(DisplayServer.window_get_native_handle(DisplayServer.WINDOW_HANDLE))
	_pad_game = _reads_pad()
	var port := OS.get_environment("PARALLAX_TEST_PORT")
	if port != "":
		_udp = PacketPeerUDP.new()
		_udp.bind(int(port), "127.0.0.1")
	_say(str("VR core on, eyes ", _views[0].size, ", controls (auto): ", "gamepad" if _pad_game else "keyboard and mouse"))


func _process(_delta: float) -> void:
	if core == null:
		return
	_frames += 1
	if _attached:
		core.begin()
	var root := get_tree().root
	_game = _game_camera(root)
	# Theater: the game draws its own flat picture, which the core puts on the screen; otherwise the window keeps only
	# the 2D UI and the game's own 3D picture leaves it.
	var theater := _mode == 2
	root.disable_3d = not theater
	root.transparent_bg = not theater
	_show_picture(_view_picture if _view != null and not theater else null)
	for v in _views:
		v.render_target_update_mode = SubViewport.UPDATE_DISABLED if theater else SubViewport.UPDATE_ALWAYS
	_serve_tests()
	var game := {
		"camera_key": (str(get_tree().current_scene.scene_file_path) if get_tree().current_scene else "") + "/" + (str(_game.name) if _game else ""),
		"cursor_free": Input.mouse_mode == Input.MOUSE_MODE_VISIBLE or Input.mouse_mode == Input.MOUSE_MODE_CONFINED,
		"pad_game": _pad_game, "carried": _carried(_game), "menu_open": false,
		"ui_aspect": _ui_aspect(root),
		"fov_y": deg_to_rad(_game.fov) if _game else 1.0, "near": _game.near if _game else 0.05,
		"focus": _focus, "aim_distance": _aim_distance(), "world_pointer": _hiding, "world_reach": _hide_reach,
		"scene": _scene_name(), "camera_name": str(_game.name) if _game else "",
	}
	if _game != null:
		game["camera"] = _game.global_transform.orthonormalized()
	var result: Dictionary = core.frame(game) if _attached else {}
	var eyes: Array = result.get("eyes", [])
	_aiming = result.get("aiming", false)
	_menu_open = result.get("menu_open", false)
	_mode = result.get("mode", 0)
	_handles = result.get("handles", false)
	_placed = eyes.size() == 2
	_menu_requests(result)
	_tools(result)
	_send_pad()
	_send_pointer(result)
	if _placed and _mode == 1 and _game != null and result.has("window"):
		_window_eyes(result["window"], root)
	elif _placed and _mode == 0:
		var world := _game.get_world_3d() if _game != null else root.find_world_3d()
		for e in 2:
			var c := _cams[e]
			_views[e].world_3d = world
			c.global_transform = eyes[e]["transform"]
			var fov: Array = eyes[e]["fov"]
			var n := maxf(_game.near if _game != null else 0.05, 0.02)
			var l := tan(float(fov[0]))
			var r := tan(float(fov[1]))
			var u := tan(float(fov[2]))
			var d := tan(float(fov[3]))
			c.set_frustum(n * (u - d), Vector2(n * (r + l) / 2.0, n * (u + d) / 2.0), n, _game.far if _game != null else 4000.0)
			if _game != null:
				c.cull_mask = _game.cull_mask
				c.environment = _game.environment
				c.attributes = _game.attributes
	var now := Time.get_ticks_msec()
	if now - _last_report > 5000:
		_last_report = now
		_say(str("frame ", _frames, " state ", core.state(), " camera ", _game.get_path() if _game else "none"))
	_log()


func _after_draw() -> void:
	for i in _held.size():
		if is_instance_valid(_held[i]):
			_held[i].global_position = _held_at[i]
	_held.clear()
	_held_at.clear()
	var left := _views[0].get_texture().get_rid()
	if not _attached:
		_attached = core.attach(left)
		return
	if _placed:
		var root := get_tree().root
		if _mode == 0 and _views[0].size != _stereo_size():
			for v in _views:
				v.size = _stereo_size()
			return
		core.submit_view(left, _views[1].get_texture().get_rid(), root.get_texture().get_rid(), _ui_aspect(root))


# The game's own log and Parallax's, where script errors land too.
func _say(text: String) -> void:
	print(LOG, text)
	if _log_file != null:
		_log_file.store_line(text)
		_log_file.flush()


func _log() -> void:
	var t: String = core.poll_log()
	if t != "":
		for line in t.strip_edges().split("\n"):
			_say(str("core: ", line))


# The game's camera: the root viewport's, else the one in the SubViewport the game shows across most of the screen.
# Many Godot games draw their 3D in a SubViewport (a low "retro" resolution, a pixel filter) and show it as a texture
# under the UI; that picture is hidden from the UI panel, since the headset now shows the world itself. Small or
# hidden SubViewports (3D icons in menus) are not the game's view, nor one shown inside another SubViewport (a gun or
# arms layer). The scene is searched every two seconds, in one pass: every frame froze a big scene.
func _game_camera(root: Window) -> Camera3D:
	var now := Time.get_ticks_msec()
	var searching := now - _searched_at > 2000
	if searching:
		_searched_at = now
		_picked_camera = core.setting("Game", "CameraName", "")
		_camera_names.clear()
		for c in root.find_children("*", "Camera3D", true, false):
			if not _cams.has(c) and not _camera_names.has(str(c.name)):
				_camera_names.append(str(c.name))
	# A camera picked in the VR menu's Game tab, by name, wherever it is.
	if _picked_camera != "":
		var picked := root.find_child(_picked_camera, true, false) as Camera3D
		if picked != null and not _cams.has(picked):
			if searching:
				_offer_menu()
			return picked
	var cam := root.get_camera_3d()
	if cam != null:
		_view = null
		if searching:
			_offer_menu()
		return cam
	if searching:
		_search(root)
		_offer_menu()
	if _view == null or not is_instance_valid(_view) or not is_instance_valid(_view_picture):
		_view = null
		return null
	return _view.get_camera_3d()


func _search(root: Window) -> void:
	_view = null
	_view_picture = null
	var screen := root.get_visible_rect().get_area()
	var best_rank := 0.0
	var pictures := {}
	for node in root.find_children("*", "TextureRect", true, false):
		var tex := (node as TextureRect).texture as ViewportTexture
		if tex != null and tex.viewport_path != NodePath():
			pictures[node.get_node_or_null(tex.viewport_path)] = node
	for node in root.find_children("*", "SubViewport", true, false):
		var vp := node as SubViewport
		if _views.has(vp) or vp.get_camera_3d() == null:
			continue
		var picture: Control = vp.get_parent() if vp.get_parent() is SubViewportContainer else pictures.get(vp)
		if picture == null or not picture.is_visible_in_tree():
			continue
		var area := picture.get_global_rect().get_area()
		var rank := area + (screen * 10 if picture.get_viewport() == root else 0.0)
		if area >= 0.5 * screen and rank > best_rank:
			_view = vp
			_view_picture = picture
			best_rank = rank


func _show_picture(picture: CanvasItem) -> void:
	if picture == _hidden:
		return
	if _hidden != null and is_instance_valid(_hidden):
		_hidden.self_modulate.a = _hidden_alpha
	_hidden = picture
	if picture != null:
		_hidden_alpha = picture.self_modulate.a
		picture.self_modulate.a = 0
		_say(str("the game's view ", picture.get_path(), " is in the headset; hidden from the UI panel"))


# Parallax: the two window cameras, from the game camera moved along its right, looking where it looks, through the
# window's frustum (the core's), at a screen-shaped size a little under the eye height; and the focus the core eases
# for the next frame: the median of five rays around the middle of the view.
func _window_eyes(window: Array, root: Window) -> void:
	var aspect := _ui_aspect(root)
	var h := int(_eye_size.y * 0.8)
	var size := Vector2i(int(h * aspect), h)
	var basis := _game.global_basis.orthonormalized()
	for e in 2:
		var v := _views[e]
		if v.size != size:
			v.size = size
		v.world_3d = _game.get_world_3d()
		var c := _cams[e]
		c.global_transform = Transform3D(basis, _game.global_position + basis.x * float(window[e]["offset"]))
		var fov: Array = window[e]["fov"]
		var n := maxf(_game.near, 0.02)
		var l := tan(float(fov[0]))
		var r := tan(float(fov[1]))
		var u := tan(float(fov[2]))
		var d := tan(float(fov[3]))
		c.set_frustum(n * (u - d), Vector2(n * (r + l) / 2.0, n * (u + d) / 2.0), n, _game.far)
		c.cull_mask = _game.cull_mask
		c.environment = _game.environment
		c.attributes = _game.attributes
	var space := _game.get_world_3d().direct_space_state
	var vp := _game.get_viewport()
	var vs := vp.get_visible_rect().size
	var hits: Array[float] = []
	for at in [Vector2(0.5, 0.5), Vector2(0.4, 0.5), Vector2(0.6, 0.5), Vector2(0.5, 0.4), Vector2(0.5, 0.6)]:
		var p: Vector2 = at * vs
		var from := _game.project_ray_origin(p)
		var to := from + _game.project_ray_normal(p) * _game.far
		var hit := space.intersect_ray(PhysicsRayQueryParameters3D.create(from, to))
		if not hit.is_empty():
			hits.append(from.distance_to(hit["position"]))
	if not hits.is_empty():
		hits.sort()
		_focus = hits[hits.size() / 2]


# While aiming: how far the game camera's middle ray goes before it hits something (0: nothing), for the crosshair.
func _aim_distance() -> float:
	if not _aiming or _game == null:
		return 0.0
	var from := _game.global_position
	var hit := _game.get_world_3d().direct_space_state.intersect_ray(PhysicsRayQueryParameters3D.create(from, from - _game.global_basis.z * 40.0))
	return 0.0 if hit.is_empty() else from.distance_to(hit["position"])


# The scene as game scripts know it (on_scene): its file's name ("level_2" for res://levels/level_2.tscn), else its root's.
func _scene_name() -> String:
	var s := get_tree().current_scene
	if s == null:
		return ""
	return s.scene_file_path.get_file().get_basename() if s.scene_file_path != "" else str(s.name)


func _stereo_size() -> Vector2i:
	var scale := float(core.render_scale())
	return Vector2i(int(_eye_size.x * scale), int(_eye_size.y * scale))


# What the VR menu asks of the game: quit, follow another camera, new eye resolution.
func _menu_requests(result: Dictionary) -> void:
	if result.get("quit", false):
		_say(str("quit from the VR menu"))
		get_tree().quit()
	if result.has("camera_picked"):
		_picked_camera = result["camera_picked"]
		_searched_at = -10000
	if result.get("render_scale", false):
		var scale := float(core.render_scale())
		for v in _views:
			v.size = Vector2i(int(_eye_size.x * scale), int(_eye_size.y * scale))


# The menu's Game tab: the game's cameras and folder, every two seconds with the camera search.
func _offer_menu() -> void:
	core.set_menu_host({
		"product": ProjectSettings.get_setting("application/config/name", "Godot game"),
		"game_dir": OS.get_executable_path().get_base_dir(),
		"cameras": _camera_names, "current_camera": str(_game.name) if _game else "",
		"eye_width": _views[0].size.x, "eye_height": _views[0].size.y,
		"hand_held": true, "hide_objects": true, "hidden": _hidden_paths().size(),
	})


func _ui_aspect(root: Window) -> float:
	var size := root.get_texture().get_size()
	return size.x / maxf(size.y, 1.0)


# The core's laser pointer on the UI panel, delivered as Godot's own mouse events at window pixels (Godot maps them
# through the game's stretch settings, as a real mouse): the game's buttons and sliders work with it, and the player's
# real cursor is never moved.
func _send_pointer(result: Dictionary) -> void:
	if not result.has("pointer"):
		if _pointer_down:
			_mouse_button(_pointer, false)
		_pointer = Vector2(-1, -1)
		_pointer_down = false
		return
	var at: Vector2 = result["pointer"] * Vector2(DisplayServer.window_get_size())
	if at.distance_to(_pointer) > 0.5:
		var m := InputEventMouseMotion.new()
		m.position = at
		m.global_position = at
		m.relative = at - _pointer if _pointer.x >= 0 else Vector2.ZERO
		Input.parse_input_event(m)
		_pointer = at
	var down: bool = result.get("pointer_down", false)
	if down != _pointer_down:
		_mouse_button(at, down)
		_pointer_down = down


func _mouse_button(at: Vector2, pressed: bool) -> void:
	var b := InputEventMouseButton.new()
	b.button_index = MOUSE_BUTTON_LEFT
	b.pressed = pressed
	b.position = at
	b.global_position = at
	Input.parse_input_event(b)


# Controls Auto: a gamepad where the game's input map has gamepad events (its own, not Godot's ui_ actions).
func _reads_pad() -> bool:
	for action in InputMap.get_actions():
		if String(action).begins_with("ui_"):
			continue
		for e in InputMap.action_get_events(action):
			if e is InputEventJoypadButton or e is InputEventJoypadMotion:
				return true
	return false


# First person until the camera's turns say otherwise: a camera inside a character body, near its centre.
func _carried(cam: Camera3D) -> bool:
	if cam == null:
		return false
	var n := cam.get_parent()
	for i in 4:
		if n == null:
			return false
		if n is CharacterBody3D or n is RigidBody3D:
			var off := cam.global_position - (n as Node3D).global_position
			return Vector2(off.x, off.z).length() < 0.6
		n = n.get_parent()
	return false


# The core's emulated gamepad into Godot's input, as a connected pad 0; only changes are sent.
func _send_pad() -> void:
	var p: Dictionary = core.pad()
	if p.is_empty():
		if not _pad_sent.is_empty():
			for key in _pad_sent.keys():
				if key is String:
					_axis(int(key), 0.0)
				else:
					_button(key, false)
			_pad_sent.clear()
		return
	var buttons: int = p["buttons"]
	for bit in PAD_BUTTONS:
		_button(PAD_BUTTONS[bit], (buttons & bit) != 0)
	_axis(JOY_AXIS_LEFT_X, p["lx"])
	_axis(JOY_AXIS_LEFT_Y, -float(p["ly"]))
	_axis(JOY_AXIS_RIGHT_X, p["rx"])
	_axis(JOY_AXIS_RIGHT_Y, -float(p["ry"]))
	_axis(JOY_AXIS_TRIGGER_LEFT, p["lt"])
	_axis(JOY_AXIS_TRIGGER_RIGHT, p["rt"])


func _axis(axis: int, value: float) -> void:
	var key := str(axis)
	if is_equal_approx(_pad_sent.get(key, 0.0), value):
		return
	_pad_sent[key] = value
	var e := InputEventJoypadMotion.new()
	e.device = 0
	e.axis = axis
	e.axis_value = value
	Input.parse_input_event(e)


func _button(button: int, pressed: bool) -> void:
	if _pad_sent.get(button, false) == pressed:
		return
	_pad_sent[button] = pressed
	var e := InputEventJoypadButton.new()
	e.device = 0
	e.button_index = button
	e.pressed = pressed
	Input.parse_input_event(e)


# Tests: PARALLAX_TEST_PORT opens a UDP channel, as the Unity plugin's: stats, set, controllers, turn.
func _serve_tests() -> void:
	if _udp == null:
		return
	while _udp.get_available_packet_count() > 0:
		var cmd := _udp.get_packet().get_string_from_utf8().strip_edges()
		_udp.set_dest_address(_udp.get_packet_ip(), _udp.get_packet_port())
		_udp.put_packet(JSON.stringify(_command(cmd)).to_utf8_buffer())


func _command(cmd: String) -> Variant:
	var parts := cmd.split(" ", false, 1)
	match parts[0]:
		"stats":
			var yaw := 0.0
			if _game != null:
				var f := -_game.global_basis.z
				yaw = rad_to_deg(atan2(f.x, -f.z))
			return {"state": core.state(), "camera": str(_game.get_path()) if _game else "", "aiming": _aiming,
				"pointer": [_pointer.x, _pointer.y], "pointer_down": _pointer_down, "menu_open": _menu_open, "mode": _mode, "handles": _handles,
				"heading": yaw, "frames": _frames, "pad_game": _pad_game, "game": get_tree().root.get_meta("g1", {})}
		"set":
			var kv := parts[1].split(" ", false, 1)
			var key := kv[0].split("/")
			core.set_setting(key[0], key[1], kv[1] if kv.size() > 1 else "")
			return {"ok": true}
		"controllers":
			core.test_controllers(JSON.parse_string(parts[1]) if parts.size() > 1 else {})
			return {"ok": true}
		"point", "point_screen":
			var uv := parts[1].split(" ")
			core.test_point(float(uv[0]), float(uv[1]), parts[0] == "point_screen")
			return {"ok": true}
		"menu":
			core.toggle_menu()
			return {"ok": true}
		"capture":
			core.capture(parts[1])
			return {"ok": true}
		"setting":
			var key := parts[1].split("/")
			return {"value": core.setting(key[0], key[1], "")}
		"recenter":
			core.recenter()
			return {"ok": true}
		"turn", "play":
			# Fixture G1's own hooks: turn the player, or press PLAY.
			var main := get_tree().root.find_child("Main", true, false)
			if main != null and parts[0] == "turn" and main.has_method("turn"):
				main.turn(float(parts[1]))
			elif main != null and main.has_method("_on_play"):
				main._on_play()
			return {"ok": true}
	return {"error": "unknown " + parts[0]}


# The Game tab's tools, as in Unity games: hiding objects with the laser, and the weapon in the hand.
func _tools(result: Dictionary) -> void:
	if result.get("show_hidden", false):
		for n in _hid:
			if is_instance_valid(n):
				n.visible = true
		_hid.clear()
		core.set_setting("Game", "Hidden", "")
	if result.get("hide_objects", false):
		_hiding = true
		_trigger_was = true  # the trigger that chose the tool must be let go first
	if _hiding:
		_aim_hider(result)
	_rehide()
	if _aiming and _game != null and result.get("hand_aiming", false) and result.has("right") 			and core.setting("Controls", "WeaponInHand", "true").to_lower() == "true":
		var hand: Transform3D = result["right"]
		_hold(hand.origin - hand.basis.z * 0.08 * float(result.get("scale", 1.0)))


# The laser goes into the game world from the right hand and ends on the nearest thing it would hide; the trigger hides
# it (kept per game as node paths), B or the VR menu puts the tool away.
func _aim_hider(result: Dictionary) -> void:
	_hide_target = null
	if _menu_open or result.get("b", false):
		_hiding = false
		return
	if not result.has("right"):
		return
	var now := Time.get_ticks_msec()
	if now - _visuals_at > 1000:
		_visuals_at = now
		_visuals = get_tree().root.find_children("*", "VisualInstance3D", true, false)
	var hand: Transform3D = result["right"]
	var from := hand.origin
	var dir := -hand.basis.z.normalized()
	var best := INF
	for n in _visuals:
		var v := n as VisualInstance3D
		if v == null or not is_instance_valid(v) or not v.is_visible_in_tree():
			continue
		var hit = (v.global_transform * v.get_aabb()).intersects_ray(from, dir)
		if hit != null and from.distance_to(hit) < best:
			best = from.distance_to(hit)
			_hide_target = v
	_hide_reach = best / maxf(float(result.get("scale", 1.0)), 1e-4) if _hide_target != null else 3.0
	var trigger := float(result.get("trigger", 0.0)) > 0.6
	if trigger and not _trigger_was and _hide_target != null:
		var paths := _hidden_paths()
		paths.append(str(_hide_target.get_path()))
		core.set_setting("Game", "Hidden", ";".join(paths))
		_hide_target.visible = false
		_hid.append(_hide_target)
		_say(str("hid ", _hide_target.get_path()))
	_trigger_was = trigger


# Hidden objects stay hidden: scenes load and games show nodes again.
func _rehide() -> void:
	var now := Time.get_ticks_msec()
	if now - _rehide_at < 2000:
		return
	_rehide_at = now
	for p in _hidden_paths():
		var n := get_tree().root.get_node_or_null(NodePath(p)) as VisualInstance3D
		if n != null and n.visible:
			n.visible = false
			_hid.append(n)


func _hidden_paths() -> PackedStringArray:
	var out := PackedStringArray()
	for p in core.setting("Game", "Hidden", "").split(";"):
		if p != "" and not out.has(p):
			out.append(p)
	return out


# Weapon in hand: the camera's children that draw (the viewmodel: arms and gun) move so their middle sits in the hand
# while the eyes render, and go back once the frame is drawn, so the game's own logic never sees them moved.
func _hold(hand: Vector3) -> void:
	var roots: Array[Node3D] = []
	var middle := Vector3.ZERO
	var count := 0
	for c in _game.get_children():
		var n := c as Node3D
		if n == null or not n.is_visible_in_tree():
			continue
		var vis := n.find_children("*", "VisualInstance3D", true, false)
		if n is VisualInstance3D:
			vis.append(n)
		if vis.is_empty():
			continue
		roots.append(n)
		for v in vis:
			var vi := v as VisualInstance3D
			middle += (vi.global_transform * vi.get_aabb()).get_center()
			count += 1
	if count == 0:
		return
	var shift := hand - middle / count
	for n in roots:
		_held.append(n)
		_held_at.append(n.global_position)
		n.global_position += shift
