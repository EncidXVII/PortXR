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
var _views: Array[SubViewport] = []
var _cams: Array[Camera3D] = []
var _attached := false
var _placed := false
var _frames := 0
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
# XInput button bits to Godot's pad buttons.
const PAD_BUTTONS := {0x1000: JOY_BUTTON_A, 0x2000: JOY_BUTTON_B, 0x4000: JOY_BUTTON_X, 0x8000: JOY_BUTTON_Y,
	0x10: JOY_BUTTON_START, 0x20: JOY_BUTTON_BACK, 0x40: JOY_BUTTON_LEFT_STICK, 0x80: JOY_BUTTON_RIGHT_STICK,
	0x100: JOY_BUTTON_LEFT_SHOULDER, 0x200: JOY_BUTTON_RIGHT_SHOULDER, 0x1: JOY_BUTTON_DPAD_UP, 0x2: JOY_BUTTON_DPAD_DOWN,
	0x4: JOY_BUTTON_DPAD_LEFT, 0x8: JOY_BUTTON_DPAD_RIGHT}


func _ready() -> void:
	process_mode = Node.PROCESS_MODE_ALWAYS
	process_priority = 10000  # after the game has moved its camera this frame
	var dir := (get_script() as Script).resource_path.get_base_dir()
	GDExtensionManager.load_extension(dir.path_join("parallax.gdextension"))
	if not ClassDB.class_exists("ParallaxCore"):
		print(LOG, "the VR core did not load; the game stays flat")
		return
	if RenderingServer.get_current_rendering_driver_name() != "d3d12":
		print(LOG, "VR needs Direct3D 12 (this game runs ", RenderingServer.get_current_rendering_driver_name(), "); the game stays flat")
		return
	core = ClassDB.instantiate("ParallaxCore")
	core.open_settings(dir.path_join("parallax.cfg"))
	var info: Dictionary = core.start(ProjectSettings.get_setting("application/config/name", "Godot game"))
	_log()
	if not info["ok"]:
		core = null
		return
	var scale := clampf(float(core.setting("View", "RenderScale", "1")), 0.5, 2.0)
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
	print(LOG, "VR core on, eyes ", _views[0].size, ", controls (auto): ", "gamepad" if _pad_game else "keyboard and mouse")


func _process(_delta: float) -> void:
	if core == null:
		return
	_frames += 1
	if _attached:
		core.begin()
	var root := get_tree().root
	_game = _game_camera(root)
	_show_picture(_view_picture if _view != null else null)
	_serve_tests()
	var game := {
		"camera_key": (str(get_tree().current_scene.scene_file_path) if get_tree().current_scene else "") + "/" + (str(_game.name) if _game else ""),
		"cursor_free": Input.mouse_mode == Input.MOUSE_MODE_VISIBLE or Input.mouse_mode == Input.MOUSE_MODE_CONFINED,
		"pad_game": _pad_game, "carried": _carried(_game), "menu_open": false,
	}
	if _game != null:
		game["camera"] = _game.global_transform.orthonormalized()
	var result: Dictionary = core.frame(game) if _attached else {}
	var eyes: Array = result.get("eyes", [])
	_aiming = result.get("aiming", false)
	_placed = eyes.size() == 2
	_send_pad()
	if _placed:
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
		print(LOG, "frame ", _frames, " state ", core.state(), " camera ", _game.get_path() if _game else "none")
	_log()


func _after_draw() -> void:
	var left := _views[0].get_texture().get_rid()
	if not _attached:
		_attached = core.attach(left)
		return
	if _placed:
		var root := get_tree().root
		var size := root.get_texture().get_size()
		core.submit_view(left, _views[1].get_texture().get_rid(), root.get_texture().get_rid(), size.x / maxf(size.y, 1.0))


func _log() -> void:
	var t: String = core.poll_log()
	if t != "":
		for line in t.strip_edges().split("\n"):
			print(LOG, "core: ", line)


# The game's camera: the root viewport's, else the one in the SubViewport the game shows across most of the screen.
# Many Godot games draw their 3D in a SubViewport (a low "retro" resolution, a pixel filter) and show it as a texture
# under the UI; that picture is hidden from the UI panel, since the headset now shows the world itself. Small or
# hidden SubViewports (3D icons in menus) are not the game's view, nor one shown inside another SubViewport (a gun or
# arms layer). The scene is searched every two seconds, in one pass: every frame froze a big scene.
func _game_camera(root: Window) -> Camera3D:
	var cam := root.get_camera_3d()
	if cam != null:
		_view = null
		return cam
	var now := Time.get_ticks_msec()
	if now - _searched_at > 2000:
		_searched_at = now
		_search(root)
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
		print(LOG, "the game's view ", picture.get_path(), " is in the headset; hidden from the UI panel")


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
				"heading": yaw, "frames": _frames, "pad_game": _pad_game, "game": get_tree().root.get_meta("g1", {})}
		"set":
			var kv := parts[1].split(" ", false, 1)
			var key := kv[0].split("/")
			core.set_setting(key[0], key[1], kv[1] if kv.size() > 1 else "")
			return {"ok": true}
		"controllers":
			core.test_controllers(JSON.parse_string(parts[1]) if parts.size() > 1 else {})
			return {"ok": true}
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
