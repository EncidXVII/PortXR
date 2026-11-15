# Spike: Parallax's own VR core inside a Godot game. Loads the GDExtension from beside this script, starts the core on
# Godot's Direct3D 12 device, renders the two eyes at the headset's eye poses (from the game camera) and submits them
# as the stereo layer, as the Unity plugin does.
extends Node

var core: RefCounted
var views: Array[SubViewport] = []
var cams: Array[Camera3D] = []
var attached := false
var located := false
var frames := 0
var best: SubViewport


func _ready() -> void:
	process_mode = Node.PROCESS_MODE_ALWAYS
	process_priority = 10000
	var dir := (get_script() as Script).resource_path.get_base_dir()
	var status := GDExtensionManager.load_extension(dir.path_join("parallax.gdextension"))
	print("Parallax spike: load_extension ", status, ", class ", ClassDB.class_exists("ParallaxCore"), ", driver ", RenderingServer.get_current_rendering_driver_name())
	if not ClassDB.class_exists("ParallaxCore"):
		return
	core = ClassDB.instantiate("ParallaxCore")
	var info: Dictionary = core.start("Parallax spike")
	print("Parallax spike: start ", info)
	for e in 2:
		var v := SubViewport.new()
		v.size = Vector2i(info["width"], info["height"])
		v.render_target_update_mode = SubViewport.UPDATE_ALWAYS
		v.world_3d = get_tree().root.world_3d
		var c := Camera3D.new()
		c.projection = Camera3D.PROJECTION_FRUSTUM
		v.add_child(c)
		add_child(v)
		c.current = true
		views.append(v)
		cams.append(c)
	RenderingServer.frame_post_draw.connect(_after_draw)


func _game_camera() -> Camera3D:
	var game := get_tree().root.get_camera_3d()
	if game != null:
		return game
	if frames % 120 == 0 or best == null or not is_instance_valid(best):
		best = null
		for node in get_tree().root.find_children("*", "SubViewport", true, false):
			var vp := node as SubViewport
			if not views.has(vp) and vp.get_camera_3d() != null and (best == null or vp.size.x * vp.size.y > best.size.x * best.size.y):
				best = vp
	return best.get_camera_3d() if best != null else null


func _process(_delta: float) -> void:
	if core == null:
		return
	frames += 1
	if attached:
		core.begin()
	var game := _game_camera()
	var eyes: Array = core.locate() if attached else []
	located = eyes.size() == 2
	if game != null and located:
		var origin := Transform3D(Basis(Vector3.UP, game.global_rotation.y), game.global_position)
		for e in 2:
			var c := cams[e]
			views[e].world_3d = game.get_world_3d()
			c.global_transform = origin * (eyes[e]["transform"] as Transform3D)
			var fov: Array = eyes[e]["fov"]
			var n := maxf(game.near, 0.05)
			var l := tan(float(fov[0]))
			var r := tan(float(fov[1]))
			var u := tan(float(fov[2]))
			var d := tan(float(fov[3]))
			c.keep_aspect = Camera3D.KEEP_HEIGHT
			c.set_frustum(n * (u - d), Vector2(n * (r + l) / 2, n * (u + d) / 2), n, game.far)
			c.environment = game.environment
	if frames % 300 == 0:
		print("Parallax spike: frame ", frames, " state ", core.state(), " eyes ", located, " camera ", game.get_path() if game else "none")
	var t: String = core.poll_log()
	if t != "":
		for line in t.strip_edges().split("\n"):
			print("Parallax core: ", line)


func _after_draw() -> void:
	var left := views[0].get_texture().get_rid()
	if not attached:
		attached = core.attach(left)
		print("Parallax spike: attach ", attached)
		return
	if located:
		core.submit_eyes(left, views[1].get_texture().get_rid(), false)
