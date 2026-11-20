# Copyright (C) 2026 EncidXVII
# SPDX-License-Identifier: GPL-3.0-only

# G1: a splash camera first (like Explosiovania's shader preload), then the player. The player starts in a menu (cursor
# shown, a PLAY button); PLAY captures the mouse and the player looks with the mouse or the right stick and walks with
# WASD or the left stick, like a first-person game. Esc shows the cursor again. Everything the game receives is kept in
# the root's "g1" meta for tests: keys and buttons held, the last mouse motion, the camera's yaw and pitch, clicks.
extends Node3D

const MOUSE_DEG := 0.15  # degrees per mouse pixel
const STICK_DEG := 120.0  # degrees per second at full right stick

var _t := 0.0
var _splash: Camera3D
var _keys := {}
var _buttons := {}
var _clicks := 0
var _mouse := Vector2.ZERO


func _ready() -> void:
	_splash = Camera3D.new()
	_splash.name = "Splash"
	add_child(_splash)
	_splash.position = Vector3(0, 5, 5)
	_splash.make_current()
	$Hud/Play.pressed.connect(_on_play)
	Input.mouse_mode = Input.MOUSE_MODE_VISIBLE


func _on_play() -> void:
	_clicks += 1
	Input.mouse_mode = Input.MOUSE_MODE_CAPTURED
	$Hud/Play.visible = false


func _input(event: InputEvent) -> void:
	if event is InputEventKey:
		_keys[OS.get_keycode_string(event.keycode)] = event.pressed
		if event.pressed and event.keycode == KEY_ESCAPE:
			Input.mouse_mode = Input.MOUSE_MODE_VISIBLE
			$Hud/Play.visible = true
	elif event is InputEventJoypadButton:
		_buttons[event.button_index] = event.pressed
	elif event is InputEventMouseButton:
		_buttons[100 + event.button_index] = event.pressed
	elif event is InputEventMouseMotion and Input.mouse_mode == Input.MOUSE_MODE_CAPTURED:
		_mouse = event.relative
		$Player.rotation.y -= deg_to_rad(event.relative.x * MOUSE_DEG)
		$Player.rotation.x = clampf($Player.rotation.x - deg_to_rad(event.relative.y * MOUSE_DEG), -1.4, 1.4)


func _process(delta: float) -> void:
	_t += delta
	# The splash camera stays in the scene, no longer current: a second camera for the camera picker's tests.
	if _splash != null and _t > 1.0:
		$Player.make_current()
		_splash = null
	if Input.mouse_mode == Input.MOUSE_MODE_CAPTURED:
		var look := Vector2(Input.get_joy_axis(0, JOY_AXIS_RIGHT_X), Input.get_joy_axis(0, JOY_AXIS_RIGHT_Y))
		$Player.rotation.y -= deg_to_rad(look.x * STICK_DEG * delta)
		$Player.rotation.x = clampf($Player.rotation.x - deg_to_rad(look.y * STICK_DEG * delta), -1.4, 1.4)
	var held := []
	for k in _keys:
		if _keys[k]:
			held.append(k)
	var pressed := []
	for b in _buttons:
		if _buttons[b]:
			pressed.append(b)
	get_tree().root.set_meta("g1", {
		"keys": held, "buttons": pressed, "clicks": _clicks, "mouse": [_mouse.x, _mouse.y],
		"yaw": rad_to_deg($Player.rotation.y), "pitch": rad_to_deg($Player.rotation.x),
		"captured": Input.mouse_mode == Input.MOUSE_MODE_CAPTURED,
		"left_x": Input.get_joy_axis(0, JOY_AXIS_LEFT_X), "left_y": Input.get_joy_axis(0, JOY_AXIS_LEFT_Y),
	})


# Lets a test turn the player to check that head aim brings the camera back.
func turn(degrees: float) -> void:
	$Player.rotation.y += deg_to_rad(degrees)
