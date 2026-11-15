# G1: a splash camera first (like Explosiovania's shader preload), then the player camera after one second; the player
# turns slowly so a test can see the game's own look moving under head tracking.
extends Node3D

var _t := 0.0
var _splash: Camera3D


func _ready() -> void:
	_splash = Camera3D.new()
	add_child(_splash)
	_splash.position = Vector3(0, 5, 5)
	_splash.make_current()


func _process(delta: float) -> void:
	_t += delta
	if _splash != null and _t > 1.0:
		$Player.make_current()
		_splash.queue_free()
		_splash = null
	$Player.rotation.y = sin(_t * 0.3) * 0.15
