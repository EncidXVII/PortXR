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
	print(LOG, "VR core on, eyes ", _views[0].size)


func _process(_delta: float) -> void:
	if core == null:
		return
	_frames += 1
	if _attached:
		core.begin()
	var root := get_tree().root
	_game = _game_camera(root)
	_show_picture(_view_picture if _view != null else null)
	var cam_transform := _game.global_transform.orthonormalized() if _game != null else Transform3D()
	var eyes: Array = core.place_eyes(_game != null, cam_transform) if _attached else []
	_placed = eyes.size() == 2
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
