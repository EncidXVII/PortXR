# Copyright (C) 2026 EncidXVII
# SPDX-License-Identifier: GPL-3.0-only

# PortXR for Godot 4 games: the engine's own OpenXR, switched on by override.cfg beside the game's exe, which also
# loads this script as an autoload. Nothing else in the game is changed.
#
# The game's window keeps only its 2D UI (3D off, transparent background). The headset gets the game's world from a
# viewport of our own that shares the game's World3D: its XROrigin3D follows the game's active Camera3D every frame and
# an XRCamera3D under it is the headset, so head tracking sits on top of the game's own look. The game's UI is a panel
# in front, level, following the game camera's yaw but not the head. The game's own cameras are never replaced: making
# another camera current in the game's viewport froze a game whose splash screen drives its own camera.
extends Node

const LOG := "PortXR: "
const UI_DISTANCE := 2.0
const UI_WIDTH := 1.6
const UI_LAYER := 1 << 19  # the last visual layer, for the UI panel only

var _xr: XRInterface
var _eyes: SubViewport
var _origin: XROrigin3D
var _head: XRCamera3D
var _panel: MeshInstance3D
var _frames := 0
var _centred := false
var _hands: Array[XRController3D] = []
var _pad := {}  # what the virtual gamepad last sent, by axis or button id
var _searched_at := -10000
var _view: SubViewport
var _view_picture: Control
var _hidden: CanvasItem
var _hidden_alpha := 1.0
var _last_report := 0
var _shots := OS.get_environment("PORTXR_GODOT_SHOTS")  # tests: folder for the game window's picture


func _ready() -> void:
	process_mode = Node.PROCESS_MODE_ALWAYS
	process_priority = 10000  # after the game has moved its camera this frame
	_xr = XRServer.find_interface("OpenXR")
	if _xr == null or not _xr.is_initialized():
		print(LOG, "OpenXR did not start; the game stays flat")
		return
	var root := get_tree().root
	root.disable_3d = true
	root.transparent_bg = true
	_eyes = SubViewport.new()
	_eyes.use_xr = true
	_eyes.world_3d = root.world_3d
	_eyes.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	_origin = XROrigin3D.new()
	_eyes.add_child(_origin)
	_head = XRCamera3D.new()
	_origin.add_child(_head)
	_head.current = true
	for hand in ["left_hand", "right_hand"]:
		var c := XRController3D.new()
		c.tracker = hand
		_origin.add_child(c)
		_hands.append(c)
	_panel = MeshInstance3D.new()
	_panel.mesh = QuadMesh.new()
	_panel.layers = UI_LAYER
	var m := StandardMaterial3D.new()
	m.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	m.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	m.no_depth_test = true
	m.render_priority = Material.RENDER_PRIORITY_MAX
	m.albedo_texture = root.get_texture()
	_panel.material_override = m
	_eyes.add_child(_panel)
	add_child(_eyes)
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)  # the headset paces frames
	print(LOG, "OpenXR on, renderer ", RenderingServer.get_current_rendering_driver_name())


func _process(_delta: float) -> void:
	if _eyes == null:
		return
	_frames += 1
	# The player's eyes start at the game camera: recentre once the headset reports a pose.
	if not _centred and XRServer.get_hmd_transform() != Transform3D.IDENTITY:
		XRServer.center_on_hmd(XRServer.RESET_BUT_KEEP_TILT, true)
		_centred = true
		print(LOG, "centred on the headset")
	var root := get_tree().root
	var game := _game_camera(root)
	if game != null:
		_origin.global_transform = game.global_transform.orthonormalized()
		_head.near = game.near
		_head.far = game.far
		_head.cull_mask = game.cull_mask | UI_LAYER
		_head.environment = game.environment
		_head.attributes = game.attributes
	# The panel stays level in front: the game camera's yaw, not its pitch or the head.
	var yaw := Basis(Vector3.UP, _origin.global_basis.get_euler().y)
	_panel.global_transform = Transform3D(yaw, _origin.global_position + yaw * Vector3(0, 0, -UI_DISTANCE))
	var size := root.get_texture().get_size()
	(_panel.mesh as QuadMesh).size = Vector2(UI_WIDTH, UI_WIDTH * size.y / max(size.x, 1.0))
	_update_pad()
	var now := Time.get_ticks_msec()
	if now - _last_report > 5000:
		print(LOG, "frame ", _frames, " fps ", Engine.get_frames_per_second(), " camera ", game.get_path() if game else "none")
		_last_report = now


# The game's camera: the root viewport's, else the one in the SubViewport the game shows across most of the screen.
# Many Godot games draw their 3D in a SubViewport (a low "retro" resolution, a pixel filter) and show it as a texture
# under the UI; that picture is hidden from the UI panel, since the headset now shows the world itself. Small or hidden
# SubViewports (3D icons in menus) are not the game's view, nor is one shown inside another SubViewport (an arms layer;
# not in the headset yet). The scene is searched every two seconds, in one pass.
func _game_camera(root: Window) -> Camera3D:
	var cam := root.get_camera_3d()
	if cam != null:
		_show_picture(null)
		if _eyes.world_3d != root.find_world_3d():
			_eyes.world_3d = root.find_world_3d()
		return cam
	var now := Time.get_ticks_msec()
	if now - _searched_at > 2000:
		_searched_at = now
		_search(root)
		var took := Time.get_ticks_msec() - now
		if took > 20:
			print(LOG, "camera search took ", took, " ms")
	if _view == null or not is_instance_valid(_view) or not is_instance_valid(_view_picture) or _view.get_camera_3d() == null:
		_show_picture(null)
		return null
	if _eyes.world_3d != _view.find_world_3d():
		_eyes.world_3d = _view.find_world_3d()
	_show_picture(_view_picture)
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
		if vp == _eyes or vp.get_camera_3d() == null:
			continue
		var picture: Control = vp.get_parent() if vp.get_parent() is SubViewportContainer else pictures.get(vp)
		if picture == null or not picture.is_visible_in_tree():
			continue
		# Shown on the screen itself beats shown inside another SubViewport (a gun or arms layer over the world).
		var area := picture.get_global_rect().get_area()
		var rank := area + (screen * 10 if picture.get_viewport() == root else 0.0)
		if area >= 0.5 * screen and rank > best_rank:
			_view = vp
			_view_picture = picture
			best_rank = rank


# Hides `picture` from the UI panel (null: none), bringing back the one hidden before.
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


# The controllers as an Xbox-style gamepad inside the game (device 0), through Godot's default OpenXR action map:
# sticks, triggers, A B X Y, grips as the shoulder buttons, stick clicks, and the left menu button as Start. Games read
# it like a connected pad; only changes are sent.
func _update_pad() -> void:
	var l := _hands[0]
	var r := _hands[1]
	var ls := l.get_vector2("primary")
	var rs := r.get_vector2("primary")
	_axis(JOY_AXIS_LEFT_X, ls.x)
	_axis(JOY_AXIS_LEFT_Y, -ls.y)
	_axis(JOY_AXIS_RIGHT_X, rs.x)
	_axis(JOY_AXIS_RIGHT_Y, -rs.y)
	_axis(JOY_AXIS_TRIGGER_LEFT, l.get_float("trigger"))
	_axis(JOY_AXIS_TRIGGER_RIGHT, r.get_float("trigger"))
	_button(JOY_BUTTON_A, r.is_button_pressed("ax_button"))
	_button(JOY_BUTTON_B, r.is_button_pressed("by_button"))
	_button(JOY_BUTTON_X, l.is_button_pressed("ax_button"))
	_button(JOY_BUTTON_Y, l.is_button_pressed("by_button"))
	_button(JOY_BUTTON_LEFT_SHOULDER, l.get_float("grip") > 0.6)
	_button(JOY_BUTTON_RIGHT_SHOULDER, r.get_float("grip") > 0.6)
	_button(JOY_BUTTON_LEFT_STICK, l.is_button_pressed("primary_click"))
	_button(JOY_BUTTON_RIGHT_STICK, r.is_button_pressed("primary_click"))
	_button(JOY_BUTTON_START, l.is_button_pressed("menu_button"))


func _axis(axis: JoyAxis, value: float) -> void:
	value = 0.0 if absf(value) < 0.1 else clampf(value, -1.0, 1.0)
	if is_equal_approx(_pad.get(axis, 0.0), value):
		return
	_pad[axis] = value
	var e := InputEventJoypadMotion.new()
	e.device = 0
	e.axis = axis
	e.axis_value = value
	Input.parse_input_event(e)


func _button(button: JoyButton, pressed: bool) -> void:
	if _pad.get(100 + button, false) == pressed:
		return
	_pad[100 + button] = pressed
	var e := InputEventJoypadButton.new()
	e.device = 0
	e.button_index = button
	e.pressed = pressed
	Input.parse_input_event(e)
