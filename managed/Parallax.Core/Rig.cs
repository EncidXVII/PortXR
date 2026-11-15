using System;
using System.Collections;
using UnityEngine;
using UnityEngine.SceneManagement;

namespace Parallax
{
    internal partial class Rig : MonoBehaviour
    {
        // Unity stores render textures bottom-up on D3D; the screen capture comes out top-down.
        const int EyeFlip = 1, CaptureFlip = 0;
        const float TheaterDistance = 3f, TheaterWidth = 3.2f, UiDrop = 0.15f, MaxRetrySeconds = 60f;
        const int ErrorsBeforeTheater = 3;
        // D3D12 grabs the UI and draws the mirror in native code at Present, where the backbuffer's state is known.
        static readonly bool D3D12 = SystemInfo.graphicsDeviceType == UnityEngine.Rendering.GraphicsDeviceType.Direct3D12;

        IntPtr renderEvent;
        internal XrState State;
        int slot;
        internal readonly Pose[] Views = new Pose[2];
        internal readonly Fov[] Fovs = new Fov[2];
        internal readonly Camera[] Eyes = new Camera[2];
        readonly RenderTexture[] eyeTargets = new RenderTexture[2];
        readonly IntPtr[] eyePtrs = new IntPtr[2];
        RenderTexture uiTarget;
        IntPtr uiPtr;

        internal Camera GameCam;
        int savedMask;
        internal int SavedMask => savedMask;
        CameraClearFlags savedClear;
        Color savedBackground;
        bool camHidden;
        Action headPost;  // undo the head camera's post-processing switch-off (URP, PPv2, HDRP)
        // Other cameras drawing to the screen while the eyes render, with what they had before we changed them; keyed
        // by instance ID, since IL2CPP hands out a new proxy object per lookup.
        struct Other { public Camera cam; public int mask; public CameraClearFlags clear; public Color background; public Action post; }
        readonly System.Collections.Generic.Dictionary<int, Other> others = new System.Collections.Generic.Dictionary<int, Other>();
        int extraMask;  // their world layers, drawn by the eyes instead
        float extraNear, extraFar;  // and the clip range they drew them in
        // Built-in pipeline: each of those cameras (sky, world over a sky camera, weapon, hands) gets its own pass in each
        // eye, in the game's depth order and with its own clear, as on the screen; merged into one pass, a weapon pokes
        // through walls and a sky dome can cover the world. Render pipelines merge them into the head's pass (URP
        // stacks cameras its own way). `stack` lists this frame's, `stackEyes` the pass cameras per eye.
        bool builtIn;
        readonly System.Collections.Generic.List<int> stack = new System.Collections.Generic.List<int>();
        readonly System.Collections.Generic.List<Camera>[] stackEyes = { new System.Collections.Generic.List<Camera>(), new System.Collections.Generic.List<Camera>() };
        readonly System.Collections.Generic.HashSet<int> eyeIds = new System.Collections.Generic.HashSet<int>();
        const int UiLayers = 1 << 5;
        bool eyesRendered;
        bool theaterShown;  // the last frame showed the theater screen (or the Parallax window), not the UI panel
        bool CurvedShown => theaterShown && CurveFits;
        // The curve is a cylinder standing around the player, so it fits a screen at about eye level only.
        bool CurveFits => curvedTheater && !windowShown && Mathf.Abs(screenPitch) < 12f;
        bool curvedTheater;  // the runtime draws curved layers (SteamVR does not)
        Camera extrasCamera;
        bool beforeRenderHooked;
        int sceneHandle;

        Vector3 originPos;
        Quaternion originYaw = Quaternion.identity;
        bool recenterPending = true;

        Controllers controllers;
        readonly int[] binds = new int[Bindings.Inputs.Length], twice = new int[Bindings.GestureInputs],
            held = new int[Bindings.GestureInputs], combo = new int[Bindings.GestureInputs];
        bool glancing, glanceTrigger, watchShown;
        Vector3 watchAt;
        const float WatchRadius = 0.022f;
        // How much the watch shows (it fades in as you look at it, like DesktopVR's clock) and how lit it is, both eased.
        float watchFade, watchGlow, watchHeld;
        bool watchLooked;
        // The watch shows when looked at, as on DesktopVR: wide to show (face within ~66 degrees of the eyes, watch
        // within ~45 degrees of where the head looks), wider to stay (~81 and ~63), held at least WatchHoldS, so
        // tremor and head bob at the edge never make it blink. Fades are linear: in quickly, out a little slower.
        const float ShowFacing = 0.4f, ShowLooking = 0.7f, KeepFacing = 0.15f, KeepLooking = 0.45f, WatchHoldS = 0.3f;
        const float WatchShowS = 0.08f, WatchHideS = 0.18f, WatchGlowS = 0.1f;
        // Comfort: snap turn state (closed loop on the game camera's yaw) and the vignette's current strength.
        bool snapArmed = true, snapping;
        // The view turns with the body yaw: it follows the game camera's heading, except under head aim, where the game
        // camera follows the head and only the right stick turns the body.
        float bodyYaw, lastHeading;
        bool bodyKnown, aiming;
        // The body stands on the player's ground: world up, or the player body's own up where the game applies its own
        // gravity (Outer Wilds' small planets), carried along without twist so headings stay continuous.
        Quaternion upFrame = Quaternion.identity;
        Rigidbody playerBody;
        bool noWorldGravity, carried;  // the head camera rides on a player body (Rigidbody or CharacterController above it): first person
        Camera playerBodyOf;
        Quaternion Body => upFrame * Quaternion.Euler(0, bodyYaw, 0);
        // Head aim loop: last game camera angles, last push, learned degrees per pixel (signed: games may invert Y),
        // and for the virtual pad the learned stick direction per axis.
        float aimLastYaw, aimLastPitch, aimPushX, aimPushY, aimDppX, aimDppY;
        float padSignX = 1, padSignY = 1;  // X stays 1: see AimLoop.LearnPixels
        int padWrongY, aimWrongX, aimWrongY;
        // Orbit check: how far in front of the game camera its rotation pivot sits, from frames where it turned.
        // First-person cameras turn about themselves (near 0); third-person cameras orbit the player (metres).
        Vector3 orbitLastPos, orbitLastFwd;
        float orbitNum, orbitDen;
        int orbitSamples;
        // Auto head aim's verdict per scene and camera name (true: first person), kept when the game switches cameras
        // or reloads the scene. Per scene, as a menu's slowly orbiting camera may share the player camera's name.
        // Without one, head aim stays off (its own pushes would swing a third-person camera, Megabonk), except for a
        // camera carried at the player body's centre, which is first person until its turns say otherwise.
        readonly System.Collections.Generic.Dictionary<string, bool> firstPerson = new System.Collections.Generic.Dictionary<string, bool>();
        string sceneName = "";
        string AimKey => sceneName + "/" + GameCam.name;
        // Pushes a camera ignores pause head aim; a carried camera without a verdict that ignores them twice is not
        // steered by the mouse or stick at all (Lunistice's Cinemachine rig under the player) and loses head aim.
        float probeIgnored, aimSuspendedUntil;
        int ignoredTimes;
        const float SmoothTurnDegPerSec = 120f;
        // Step smoothing: a game camera moved in FixedUpdate steps at the physics rate (often 50 Hz) while the headset
        // shows 90, so the world judders as you move. Seen as frames where a moving camera stood still; then the eyes
        // follow a line between its last two positions, one step behind.
        Vector3 stepFrom, stepTo;
        float stepAt, stepLen = 0.02f, lastMoveAt;
        int stepMoved, stepStill, stepFrames;
        internal bool StepSmoothing;
        float snapTarget, snapUntil, snapLastYaw, snapLastPush, degPerPixel;
        internal float VignetteNow;
        Vector3 vignetteLastPos;
        float vignetteLastYaw;
        internal bool PointerActive;
        internal readonly Hider Hider = new Hider();
        Scripts scripts;  // made on the first Update: IL2CPP's injected classes cannot pass `this` in a field initialiser
        readonly CanvasOverlay canvases = new CanvasOverlay();
        readonly ScreenQuads screenQuads = new ScreenQuads();
        readonly HandHeld handHeld = new HandHeld();
        bool vrNoted;  // played.txt beside the plugin, written at the first frame sent to the headset
        bool handAiming;  // this frame's aim follows the right controller, at handAim
        Pose handAim;
        internal int Held { get; private set; }  // viewmodel parts moved into the hand this frame
        internal int AimValid => beamOn ? 1 : 0;
        Vector2 pointerUv;
        // Laser beam in tracking space, drawn into the eye images while the pointer is wanted.
        bool beamOn;
        Vector3 beamFrom, beamTo;
        bool menuKeyDown;
        bool failureHandled, restarting;
        float retryAt, retryDelay = 5f;
        int submitErrors;
        bool forcedTheater;
        int recommendedW, recommendedH;
        // Automatic render scale: a fraction of Settings.RenderScale, lowered while the headset misses frames.
        float autoScale = 1f, nextScaleCheck, p95BeforeDrop;
        int calmChecks, slowChecks;
        bool justDropped, cpuBound;

        internal Menu Menu;
        internal TestChannel Test;

        // ScreenCapture.CaptureScreenshotIntoRenderTexture exists from Unity 2019.1; older engines use the native grab.
        static readonly Action<RenderTexture> captureIntoTexture = FindCapture();
        readonly Optional managedCapture = new Optional("Screen capture into a texture"), mirror = new Optional("Desktop mirror"),
            recreate = new Optional("Eye target recreation"), resolve = new Optional("MSAA resolve"),
            menuDrawing = new Optional("VR menu drawing"), pointerDot = new Optional("Pointer dot"), resetView = new Optional("Eye view matrix reset"),
            bodyLookup = new Optional("Player body lookup");
        bool viewMatrixReset;

        static Action<RenderTexture> FindCapture()
        {
            var method = Type.GetType("UnityEngine.ScreenCapture, UnityEngine.ScreenCaptureModule")?.GetMethod(
                "CaptureScreenshotIntoRenderTexture", new[] { typeof(RenderTexture) });
            return method == null ? null : (Action<RenderTexture>)Delegate.CreateDelegate(typeof(Action<RenderTexture>), method);
        }

        void Start()
        {
            if (Native.prx_init(Application.productName, D3D12 ? 12 : 11, out int w, out int h) == 0)
            {
                DrainLog();
                Plugin.Log.LogError("OpenXR unavailable; VR stays off.");
                enabled = false;
                return;
            }
            Native.prx_install_hooks();
            renderEvent = Native.prx_render_event_func();
            nextScaleCheck = Time.realtimeSinceStartup + 10f;
            recommendedW = w;
            recommendedH = h;
            CreateEyeTargets();
            for (int e = 0; e < 2; e++)
            {
                var go = new GameObject("Parallax Eye " + e);
                DontDestroyOnLoad(go);
                Eyes[e] = go.AddComponent<Camera>();
                Eyes[e].enabled = false;
                eyeIds.Add(Eyes[e].GetInstanceID());
            }
            builtIn = BuiltInPipeline();
            Native.prx_set_init_texture(eyePtrs[0]);
            GL.IssuePluginEvent(renderEvent, Native.EvInit);
            curvedTheater = CurvedLayers();

            Application.runInBackground = true;
#if IL2CPP
            // Il2Cpp events take Il2Cpp delegates (the interop converts System ones). Accessors the game never used can
            // be stripped; then the eyes are placed in LateUpdate instead, a frame behind the game camera.
            try
            {
                Application.add_onBeforeRender((UnityEngine.Events.UnityAction)new Action(BeforeRender));
                beforeRenderHooked = true;
            }
            catch (NotSupportedException)
            {
                Plugin.Log.LogWarning("Application.onBeforeRender is stripped in this game; eyes follow the camera from LateUpdate.");
            }
            StartCoroutine(BepInEx.Unity.IL2CPP.Utils.Collections.CollectionExtensions.WrapToIl2Cpp(EndOfFrameLoop()));
#else
            Application.onBeforeRender += BeforeRender;
            beforeRenderHooked = true;
            StartCoroutine(EndOfFrameLoop());
#endif
            Menu = new Menu(this);
            Test = TestChannel.FromEnvironment();
        }

        // GraphicsSettings.currentRenderPipeline (2019.3+) also sees a quality level's pipeline; older engines only have
        // the global asset.
        static bool BuiltInPipeline()
        {
            try
            {
                var gs = typeof(UnityEngine.Rendering.GraphicsSettings);
                var p = gs.GetProperty("currentRenderPipeline") ?? gs.GetProperty("renderPipelineAsset");
                return p.GetValue(null, null) == null;
            }
            catch (Exception)
            {
                return false;  // unknown: merge passes, which works under every pipeline
            }
        }

        void CreateEyeTargets()
        {
            float scale = Mathf.Clamp(Settings.RenderScale.Value, 0.25f, 2f) * autoScale;
            int w = Mathf.RoundToInt(recommendedW * scale), h = Mathf.RoundToInt(recommendedH * scale);
            // URP sizes its intermediate buffer from the camera's target, so an 8-bit eye target would clamp HDR and
            // starve bloom. Linear-space games get half-float eyes (linear values); gamma-space games stay 8-bit.
            var format = QualitySettings.activeColorSpace == ColorSpace.Linear ? RenderTextureFormat.ARGBHalf : RenderTextureFormat.ARGB32;
            for (int e = 0; e < 2; e++)
            {
                // Frames already queued for the render thread keep the old texture alive: Unity destroys it after them.
                if (eyeTargets[e] != null)
                {
                    if (Eyes[e] != null) Eyes[e].targetTexture = null;
                    Destroy(eyeTargets[e]);
                }
                // 32-bit depth: with the near plane of a weapon camera and a far plane of a kilometre, 24 bits z-fight.
                eyeTargets[e] = new RenderTexture(w, h, 32, format, RenderTextureReadWrite.Default)
                {
                    name = "Parallax Eye " + e,
                    // The D3D12 path reads the camera's own target, which must not be multisampled.
                    antiAliasing = D3D12 ? 1 : Mathf.Max(1, QualitySettings.antiAliasing),
                };
                eyeTargets[e].Create();
                eyePtrs[e] = eyeTargets[e].GetNativeTexturePtr();
            }
            Plugin.Log.LogInfo($"Eye targets {w}x{h} {format}, MSAA {eyeTargets[0].antiAliasing}");
        }

        // Every 4 s (a fresh 256-frame timing window): two slow windows in a row lower the eye resolution 10%; a drop
        // that does not help means the game is CPU-bound, so it is undone and scaling stops. Headroom raises it again.
        // Startup and scene loads hitch (shader compiles, streaming), so the first 10 s after them do not count.
        void AutoRenderScale()
        {
            if (!Settings.AutoRenderScale.Value || cpuBound || State != XrState.Running || !eyesRendered) return;
            float now = Time.realtimeSinceStartup;
            if (now < nextScaleCheck) return;
            nextScaleCheck = now + 4f;
            Native.prx_get_stats(out var s);
            float p95 = (float)s.p95Ms, period = (float)s.periodMs;
            if (justDropped)
            {
                justDropped = false;
                if (p95 > p95BeforeDrop * 0.95f)
                {
                    cpuBound = true;
                    autoScale = Mathf.Min(1f, autoScale + 0.1f);
                    CreateEyeTargets();
                    Plugin.Log.LogInfo("Lower resolution did not help (the game is CPU-bound); render scale stays.");
                    return;
                }
            }
            slowChecks = p95 > period * 1.2f ? slowChecks + 1 : 0;
            if (slowChecks >= 2 && autoScale > 0.55f)
            {
                slowChecks = 0;
                p95BeforeDrop = p95;
                justDropped = true;
                calmChecks = 0;
                autoScale -= 0.1f;
                CreateEyeTargets();
            }
            else if (p95 < period * 1.05f && autoScale < 1f && ++calmChecks >= 3)
            {
                calmChecks = 0;
                autoScale = Mathf.Min(1f, autoScale + 0.1f);
                CreateEyeTargets();
            }
        }

#if IL2CPP
        public Rig(IntPtr pointer) : base(pointer) { }
#endif

        void OnApplicationQuit()
        {
            if (renderEvent != IntPtr.Zero) GL.IssuePluginEvent(renderEvent, Native.EvShutdown);
        }

        void Update()
        {
            DrainLog();
            Test?.Poll(this);
            State = (XrState)Native.prx_state();
            if (State == XrState.Failed || restarting)
            {
                Recover();
                return;
            }
            // The headset paces frames through xrWaitFrame; desktop vsync or a 60 fps cap would halve it.
            if (QualitySettings.vSyncCount != 0) QualitySettings.vSyncCount = 0;
            if (Application.targetFrameRate != -1) Application.targetFrameRate = -1;
            // Polled rather than SceneManager.sceneLoaded, whose accessor IL2CPP games often strip. GetHashCode is the
            // handle on every version; Scene.handle itself changed from int to a struct in Unity 6.
            int scene = SceneManager.GetActiveScene().GetHashCode();
            if (scene != sceneHandle)
            {
                sceneHandle = scene;
                sceneName = SceneManager.GetActiveScene().name;
                ReleaseCamera();
                nextScaleCheck = Time.realtimeSinceStartup + 10f;
                slowChecks = 0;
            }

            scripts = scripts ?? new Scripts(this);
            scripts.Update();
            AutoRenderScale();
            if (MenuKeyPressed()) Menu.Toggle();
            Menu.Update();
            UpdatePointer();
            var pointerAt = new Vector2(pointerUv.x * Screen.width, pointerUv.y * Screen.height);
            Menu.Point(PointerActive && Menu.Open, pointerAt, controllers.rTrigger);
            bool uiTaken = UiPointer.Update(PointerActive && !Menu.Open, new Vector2(pointerAt.x, Screen.height - pointerAt.y), controllers.rTrigger, controllers.ry);
            Hider.Apply();
            var cfg = new InputConfig
            {
                mode = (int)Settings.InputNow,
                neutral = Menu.Open || Hider.Active || OnHandles ? 1 : 0,
                pointerActive = PointerActive ? 1 : 0,
                pointerX = Mathf.RoundToInt(pointerUv.x * Screen.width),
                pointerY = Mathf.RoundToInt(pointerUv.y * Screen.height),
                pointerClick = Menu.Open || uiTaken ? 0 : 1,
                mouseSpeed = Settings.MouseSpeed.Value,
                binds = binds,
                twice = twice,
                held = held,
                combo = combo,
            };
            aiming = HeadAimWanted();
            if (aiming)
            {
                cfg.snap = 1;
                cfg.aim = 1;
                HeadAim(Settings.InputNow == InputMode.Gamepad);
                cfg.turnX = aimPushX;
                cfg.turnY = aimPushY;
            }
            // Snap turns are for a camera that turns about itself. A third-person camera (Lunistice), or one not seen
            // turning yet, gets the right stick as is, so it orbits the player as on a flat screen.
            else if (Settings.Turning.Value != TurnMode.Smooth && Settings.InputNow != InputMode.Off && GameCam != null && !Menu.Open && !windowShown
                     && (snapping || (orbitSamples >= 10 && OrbitRadius < 0.45f)))
            {
                cfg.snap = 1;
                cfg.turnX = SnapTurn(Settings.InputNow == InputMode.Gamepad);
            }
            Bindings.Codes(ref cfg);
            uint events = Native.prx_input_update(ref cfg, Test?.ControllerOverride, out controllers);
            if ((events & Native.EvMenuToggle) != 0) Menu.Toggle();
            // Test runs drive the menu themselves; real players get the welcome card once, when VR first runs.
            if (Settings.FirstRun.Value && Test == null && State == XrState.Running && !Menu.Open) Menu.Show(-1);
            if (Test?.Wrist == true)
            {
                controllers.aimL = Xr.ToPose(originPos + originYaw * new Vector3(-0.05f, -0.22f, 0.33f), originYaw * Quaternion.Euler(-20, -90, 0));
                controllers.aimValidL = 1;
            }
            bool glance = WristGlance(), trigger = controllers.rTrigger > 0.6f;
            if (glance && !glancing) Native.prx_haptic_pulse(0, 0.2f, 0.02f);
            if (glance && trigger && !glanceTrigger) Menu.Toggle();
            glancing = glance;
            glanceTrigger = trigger;
            VignetteNow = Vignette();
            VirtualPad.Update();
            GL.IssuePluginEvent(renderEvent, Native.EvBegin);
        }

        void LateUpdate()
        {
            if (!beforeRenderHooked) BeforeRender();
        }

        void BeforeRender()
        {
            eyesRendered = false;
            bool located = State == XrState.Running && Native.prx_locate(Views, Fovs) != 0;
            if (located)
            {
                Test?.ApplyHead(Views);
                if (recenterPending) Recenter();
            }
            var cam = located && Settings.Display.Value != DisplayMode.Theater && !forcedTheater && !VideoWatch.Playing() ? EnsureCamera() : null;
            windowShown = cam != null && Settings.Display.Value == DisplayMode.Parallax;
            if (cam == null)
            {
                SetEyes(false);
                RestoreCamera();
                return;
            }
            HideGameCamera();
            Sink();
            HideOtherCameras();
            canvases.Apply(c => c == GameCam || (others.TryGetValue(c.GetInstanceID(), out var o) && !c.orthographic && (o.mask & ~UiLayers) != 0));
            // Pipeline settings rarely change; a refresh every second catches in-game graphics option changes. The
            // eyes copy them as the game set them, so the head camera's post-processing is back on for the copy.
            bool refreshExtras = extrasCamera != cam || Time.frameCount % 60 == 0;
            extrasCamera = cam;
            if (refreshExtras)
            {
                headPost?.Invoke();
                headPost = null;
            }
            var camT = cam.transform;
            screenQuads.Grow(cam);
            TrackBody(camT);
            var yaw = Body;
            var basePos = Smoothed(camT.position);
            var unYaw = Quaternion.Inverse(originYaw);
            UpdateFlatScale(cam);
            float scale = Scale;
            var lift = Vector3.up * Settings.Height.Value;
            eyeBase = basePos + lift;
            eyeScale = scale;
            if (windowShown) WindowEyes(cam);
            for (int e = 0; e < 2; e++)
            {
                var eye = Eyes[e];
                eye.CopyFrom(cam);
                // A view matrix the game set by hand (portals, mirrors, camera effects) would pin both eyes to its view.
                if (!viewMatrixReset) viewMatrixReset = resetView.Run(() => { eye.ResetWorldToCameraMatrix(); eye.ResetCullingMatrix(); });
                else
                {
                    eye.ResetWorldToCameraMatrix();
                    eye.ResetCullingMatrix();
                }
                eye.cullingMask = savedMask | (builtIn ? 0 : extraMask);
                if (extraMask != 0 && !builtIn)
                {
                    eye.nearClipPlane = Mathf.Min(eye.nearClipPlane, extraNear);
                    eye.farClipPlane = Mathf.Max(eye.farClipPlane, extraFar);
                }
                eye.clearFlags = savedClear;
                eye.backgroundColor = savedBackground;
                eye.targetTexture = windowShown ? windowTargets[e] : eyeTargets[e];
                eye.rect = new Rect(0, 0, 1, 1);
                if (refreshExtras) CameraExtras.Copy(cam, eye);
                if (windowShown) eye.transform.SetPositionAndRotation(camT.position + camT.rotation * windowEye[e], camT.rotation);
                else
                {
                    var rel = unYaw * (Xr.Position(Views[e]) - originPos);
                    var nudge = scripts != null ? scripts.EyeOffset : Vector3.zero;
                    eye.transform.SetPositionAndRotation(basePos + yaw * ((rel + nudge) * scale) + lift,
                        yaw * unYaw * Xr.Rotation(Views[e]));
                }
                SetProjection(eye, EyeFov(e));
                eye.enabled = true;
                StackPasses(e, eye, refreshExtras);
            }
            if (refreshExtras) headPost = CameraPost.Off(cam);
            PlaceReticle(camT);
            Held = 0;
            if (aiming && handAiming && Settings.WeaponInHand.Value)
            {
                // The newest hand pose; the one aim used this frame when tracking dropped since.
                var p = controllers.aimValidR != 0 ? controllers.aimR : handAim;
                handHeld.Move(cam, ToWorld(Xr.Position(p) + Xr.Rotation(p) * new Vector3(0, 0, 0.08f)));
                Held = handHeld.Held;
            }
            eyesRendered = true;
        }

        // One pass per stacked camera, turned like the head pass. The first pass to draw clears colour even when its
        // camera only cleared depth on the screen (it drew over the previous frame there).
        void StackPasses(int e, Camera head, bool refreshExtras)
        {
            var passes = stackEyes[e];
            var offset = head.transform.position - GameCam.transform.position;
            float first = head.depth;
            int n = builtIn ? stack.Count : 0;
            for (int k = 0; k < n; k++) first = Mathf.Min(first, others[stack[k]].cam.depth);
            for (int k = 0; k < n; k++)
            {
                var o = others[stack[k]];
                if (k == passes.Count)
                {
                    var go = new GameObject("Parallax Eye " + e + "." + k);
                    DontDestroyOnLoad(go);
                    var c = go.AddComponent<Camera>();
                    eyeIds.Add(c.GetInstanceID());
                    passes.Add(c);
                }
                var pass = passes[k];
                pass.CopyFrom(o.cam);
                if (viewMatrixReset)
                {
                    pass.ResetWorldToCameraMatrix();
                    pass.ResetCullingMatrix();
                }
                pass.cullingMask = o.mask & ~UiLayers;
                pass.clearFlags = o.clear;
                pass.backgroundColor = o.background;
                pass.targetTexture = head.targetTexture;
                pass.rect = new Rect(0, 0, 1, 1);
                // Where the game put that camera, moved as the head pass is moved from the head camera (a 3D-skybox
                // camera sits elsewhere and turns along).
                bool flat = windowShown && o.cam.depth > GameCam.depth && Overlays(o.clear);
                pass.transform.SetPositionAndRotation(o.cam.transform.position + (flat ? Vector3.zero : offset), head.transform.rotation);
                SetProjection(pass, flat ? windowFov[2] : EyeFov(e));
                if (refreshExtras)
                {
                    o.post?.Invoke();
                    CameraExtras.Copy(o.cam, pass);
                    o.post = CameraPost.Off(o.cam);
                    others[stack[k]] = o;
                }
                pass.enabled = true;
            }
            for (int k = n; k < passes.Count; k++) passes[k].enabled = false;
            foreach (var c in passes)
                if (c.enabled && c.depth == first && Overlays(c.clearFlags)) c.clearFlags = CameraClearFlags.SolidColor;
            if (head.depth == first && Overlays(head.clearFlags)) head.clearFlags = CameraClearFlags.SolidColor;
        }

        // Parallax mode: the game in 3D inside the theater screen, rendered as on a 3D monitor. The game's camera stays
        // the game's (no head aim, no turning) and the head does not steer the view: two eyes an eye-width apart look
        // through the screen, whose plane sits at the focus distance in the game, so what is there appears on the screen,
        // nearer things in front of it and farther ones behind. Cameras drawn over the game (a weapon, a HUD) show flat
        // on the screen, inside its frame.
        RenderTexture[] windowTargets = new RenderTexture[2];
        readonly IntPtr[] windowPtrs = new IntPtr[2];
        readonly Vector3[] windowEye = new Vector3[2];
        readonly Fov[] windowFov = new Fov[3];  // left, right, and the flat middle view
        bool windowShown;
        float focus = 5f;
        readonly Optional focusRay = new Optional("Parallax focus");

        internal bool WindowShown => windowShown;
        internal float Focus => focus;

        Fov EyeFov(int e) => windowShown ? windowFov[e] : Fovs[e];

        void WindowEyes(Camera cam)
        {
            EnsureWindowTargets();
            Placement(false, out _, out _, out float w, out float h);
            float ipd = Mathf.Clamp((Xr.Position(Views[1]) - Xr.Position(Views[0])).magnitude, 0.05f, 0.08f);
            UpdateFocus(cam);
            // The window in the game: at the focus distance, as large as the flat view there. Game units per metre of
            // screen, times the depth setting, scale the eyes' offsets: at full depth distant things sit an eye-width
            // apart on the screen. Never wider than two eye-widths at the world scale, though: tied to the focus alone
            // the eyes went metres apart when the view looked far (54 m down Peripeteia's first street), so near things
            // split and the picture slid as the focus moved. Past that, far views just come out flatter.
            float gh = 2 * focus * Xr.Tan(cam.fieldOfView * Mathf.Deg2Rad / 2), gw = gh * w / h;
            float k = Mathf.Min(gh / h, MaxSeparation * Scale) * Mathf.Clamp01(Settings.Depth.Value);
            for (int e = 0; e < 3; e++)
            {
                float x = e == 2 ? 0 : (e == 0 ? -ipd : ipd) / 2 * k;
                if (e < 2) windowEye[e] = new Vector3(x, 0, 0);
                windowFov[e] = new Fov
                {
                    left = Xr.Atan((-gw / 2 - x) / focus),
                    right = Xr.Atan((gw / 2 - x) / focus),
                    down = Xr.Atan(-gh / 2 / focus),
                    up = Xr.Atan(gh / 2 / focus),
                };
            }
        }

        const float MaxSeparation = 2f, MaxFocus = 15f;

        // What sits at the middle of the view: the play plane of a 2D game, else the median of five rays around the
        // centre (the character of a third-person game, the wall ahead in first person). No farther than 15 m at the
        // world scale, and eased over about 0.7 s, so the screen plane does not jump as the view sweeps past things.
        void UpdateFocus(Camera cam)
        {
            float target = focus;
            if (flatScale > 1f) target = flatScale * FlatPlaneDistance;
            else focusRay.Run(() =>
            {
                var hits = new System.Collections.Generic.List<float>();
                for (int i = 0; i < 5; i++)
                {
                    var at = new Vector3(0.5f + (i == 1 ? -0.1f : i == 2 ? 0.1f : 0), 0.5f + (i == 3 ? -0.1f : i == 4 ? 0.1f : 0), 0);
                    if (Physics.Raycast(cam.ViewportPointToRay(at), out var hit, cam.farClipPlane, savedMask, QueryTriggerInteraction.Ignore)) hits.Add(hit.distance);
                }
                if (hits.Count == 0) return;
                hits.Sort();
                target = hits[hits.Count / 2];
            });
            float near = Mathf.Max(cam.nearClipPlane * 4, 0.5f);
            target = Mathf.Clamp(target, near, Mathf.Max(near, MaxFocus * Scale, flatScale * FlatPlaneDistance));
            focus = Mathf.Lerp(focus, target, 1 - (float)System.Math.Exp(-Time.unscaledDeltaTime * 1.5f));
        }

        // Screen-shaped, a little under the eye height in pixels: the window covers less of the view than the eyes do.
        void EnsureWindowTargets()
        {
            var eye = eyeTargets[0];
            int h = Mathf.RoundToInt(eye.height * 0.8f), w = Mathf.RoundToInt(h * (float)Screen.width / Mathf.Max(1, Screen.height));
            if (windowTargets[0] != null && windowTargets[0].width == w && windowTargets[0].height == h && windowTargets[0].format == eye.format) return;
            for (int e = 0; e < 2; e++)
            {
                if (windowTargets[e] != null)
                {
                    if (Eyes[e] != null && Eyes[e].targetTexture == windowTargets[e]) Eyes[e].targetTexture = null;
                    Destroy(windowTargets[e]);
                }
                windowTargets[e] = new RenderTexture(w, h, 32, eye.format, RenderTextureReadWrite.Default) { name = "Parallax Window " + e, antiAliasing = eye.antiAliasing };
                windowTargets[e].Create();
                windowPtrs[e] = windowTargets[e].GetNativeTexturePtr();
            }
        }

        void SetProjection(Camera c, Fov fov)
        {
            c.projectionMatrix = Xr.Projection(fov, c.nearClipPlane, c.farClipPlane);
#if !IL2CPP
            var keep = c.GetComponent<EyeProjection>();
            if (keep != null) keep.Matrix = c.projectionMatrix;
#endif
        }

        static bool Overlays(CameraClearFlags clear) => clear == CameraClearFlags.Depth || clear == CameraClearFlags.Nothing;

        IEnumerator EndOfFrameLoop()
        {
            var wait = new WaitForEndOfFrame();
            while (true)
            {
                yield return wait;
                if (State != XrState.Running) continue;
                try
                {
                    SubmitFrame();
                }
                catch (Exception ex)
                {
                    if (++submitErrors == 1) Plugin.Log.LogError("Frame submit failed: " + ex);
                    if (submitErrors == ErrorsBeforeTheater)
                    {
                        forcedTheater = true;
                        Plugin.Log.LogWarning("Stereo keeps failing in this game; switched to theater mode.");
                    }
                }
            }
        }

        void SubmitFrame()
        {
            screenQuads.Restore();
            handHeld.Restore();
            // The game camera drew nothing, so the backbuffer now holds only overlay UI and IMGUI.
            bool native = D3D12 || captureIntoTexture == null || !managedCapture.Run(() =>
            {
                EnsureUiTarget();
                captureIntoTexture(uiTarget);
            });
            if (native && !D3D12) GL.IssuePluginEvent(renderEvent, Native.EvCaptureBackbuffer);
            bool window = eyesRendered && windowShown, stereo = eyesRendered && !window;
            // The theater screen and the Parallax window still need eye images for the hand menu and the beam:
            // transparent ones, drawn over the screen.
            bool layered = stereo || window || ((watchShown || watchFade > 0 || beamOn) && !VideoWatch.Playing());
            var f = new Frame { stereo = layered ? 1 : 0, quad = 1, quadFromBackbuffer = native ? 1 : 0, quadAlpha = stereo || window ? 1 : 0 };
            if (window)
            {
                foreach (var rt in windowTargets)
                    if (rt.antiAliasing > 1) resolve.Run(rt.ResolveAntiAliasedSurface);
                f.window = 1;
                f.windowL = new Layer { texture = windowPtrs[0], flipY = EyeFlip };
                f.windowR = new Layer { texture = windowPtrs[1], flipY = EyeFlip };
            }
            if (layered)
            {
                // A managed blit would land in the D3D12 UI grab; there, and where Graphics.Blit is stripped, Present draws it.
                f.mirror = (stereo || window) && (D3D12 || !mirror.Run(Mirror)) ? 1 : 0;
                for (int e = 0; e < 2; e++)
                {
                    var rt = eyeTargets[e];
                    recreate.Run(() =>
                    {
                        if (rt.IsCreated()) return;
                        rt.Create();
                        eyePtrs[e] = rt.GetNativeTexturePtr();
                    });
                    if (!stereo)
                    {
                        var was = RenderTexture.active;
                        RenderTexture.active = rt;
                        GL.Clear(true, true, Color.clear);
                        RenderTexture.active = was;
                    }
                    // Unity resolves a camera's MSAA target itself; the explicit resolve is a safety net.
                    else if (rt.antiAliasing > 1) resolve.Run(rt.ResolveAntiAliasedSurface);
                }
                f.eyeL = new Layer { texture = eyePtrs[0], flipY = EyeFlip };
                f.eyeR = new Layer { texture = eyePtrs[1], flipY = EyeFlip };
                f.poseL = Views[0];
                f.poseR = Views[1];
                f.fovL = Fovs[0];
                f.fovR = Fovs[1];
            }
            Placement(stereo, out var center, out var rot, out f.quadWidth, out f.quadHeight);
            f.quadLayer = new Layer { texture = uiPtr, flipY = CaptureFlip };
            // Where the runtime can, the theater screen curves around the player: its layer pose is the cylinder's centre.
            // The Parallax window takes the theater screen's place, flat: its view is computed for a plane.
            theaterShown = !stereo;
            bool curved = CurvedShown;
            f.quadPose = Xr.ToPose(curved ? CurveCenter : center, rot);
            f.quadRadius = curved ? CurveRadius : 0;
            f.vignette = stereo ? VignetteNow : 0;
            f.beam = layered && beamOn ? 1 : 0;
            // Native projects with the OpenXR eye poses: right-handed, so Z mirrored back.
            f.fromX = beamFrom.x; f.fromY = beamFrom.y; f.fromZ = -beamFrom.z;
            f.toX = beamTo.x; f.toY = beamTo.y; f.toZ = -beamTo.z;
            f.watch = layered && watchFade > 0.004f ? 1 : 0;
            f.watchLit = watchGlow;
            f.watchAlpha = watchFade;
            var accent = Menu.Lime;
            f.accentR = accent.r; f.accentG = accent.g; f.accentB = accent.b;
            f.passthrough = Settings.Display.Value != DisplayMode.Stereo && Settings.Background.Value == ScreenBackground.Passthrough ? 1 : 0;
            f.watchX = watchAt.x; f.watchY = watchAt.y; f.watchZ = -watchAt.z;
            f.watchRadius = WatchRadius;
            // Auto: once the head camera draws off screen and the screen is cleared for the UI, the panel's alpha is
            // usually the game's own again, so native measures it afresh (3) and keys it only if it is still opaque
            // (keying a clean panel made dark HUD frames and crosshairs see-through).
            f.uiKey = Settings.UiBlack.Value == UiBackground.SeeThrough ? 1 : Settings.UiBlack.Value == UiBackground.Keep ? 2
                : screenCleared ? 3 : 0;
            f.scene = sceneHandle;
            if (stereo && reticleShown)
            {
                var area = scripts != null ? scripts.CrosshairArea : Vector2.zero;
                f.patchW = area.x > 0 ? area.x / 2 : PatchW;
                f.patchH = area.y > 0 ? area.y / 2 : PatchH;
                if (PatchShown)
                {
                    // Same size to the eye as on the panel, facing the head.
                    var headAt = (Xr.Position(Views[0]) + Xr.Position(Views[1])) / 2;
                    var at = ToTracking(ReticleAt);
                    float grow = (at - headAt).magnitude / Mathf.Max(0.1f, (center - headAt).magnitude);
                    f.patchShown = 1;
                    f.aimPose = Xr.ToPose(at, Quaternion.LookRotation(at - headAt, originYaw * Vector3.up));
                    f.aimW = 2 * f.patchW * f.quadWidth * grow;
                    f.aimH = 2 * f.patchH * f.quadHeight * grow;
                }
                else
                {
                    f.aimLX = reticle[0]; f.aimLY = reticle[1]; f.aimLR = reticle[2];
                    f.aimRX = reticle[3]; f.aimRY = reticle[4]; f.aimRR = reticle[5];
                }
            }
            if (Menu.Open)
            {
                var r = Menu.ScreenRect;
                f.keepX0 = r.xMin / Screen.width;
                f.keepY0 = r.yMin / Screen.height;
                f.keepX1 = r.xMax / Screen.width;
                f.keepY1 = r.yMax / Screen.height;
            }
            if (!vrNoted)
            {
                vrNoted = true;
                try { System.IO.File.WriteAllText(System.IO.Path.Combine(System.IO.Path.GetDirectoryName(typeof(Rig).Assembly.Location), "played.txt"), DateTime.UtcNow.ToString("o")); }
                catch (Exception e) { Plugin.Log.LogWarning("Couldn't note the VR start for the app: " + e.Message); }
            }
            Native.prx_set_frame(slot, ref f);
            GL.IssuePluginEvent(renderEvent, Native.EvSubmit | (slot << 8));
            slot = (slot + 1) & 3;
        }

        // The desktop shows the left eye, as the native mirror does. Not the captured UI over it: games often fill that
        // capture opaque (black under their HUD), which hid the world on the screen.
        void Mirror()
        {
            var eye = windowShown ? windowTargets[0] : eyeTargets[0];
            float visible = Mathf.Min(1f, (float)eye.width / eye.height / ((float)Screen.width / Screen.height));
            Graphics.Blit(eye, (RenderTexture)null, new Vector2(1, visible), new Vector2(0, (1 - visible) / 2));
        }

        void EnsureUiTarget()
        {
            if (uiTarget != null && uiTarget.IsCreated() && uiTarget.width == Screen.width && uiTarget.height == Screen.height) return;
            if (uiTarget != null) Destroy(uiTarget);
            uiTarget = new RenderTexture(Screen.width, Screen.height, 0, RenderTextureFormat.ARGB32, RenderTextureReadWrite.Default)
            {
                name = "Parallax UI",
            };
            uiTarget.Create();
            uiPtr = uiTarget.GetNativeTexturePtr();
        }

        internal void RequestRecenter() => recenterPending = true;

        static bool CurvedLayers()
        {
            Native.prx_get_stats(out var s);
            return s.curvedSupported != 0;
        }

        // Runtime restarts, headset sleep and session loss end the session; the game goes flat and we retry with
        // a growing delay until the runtime is back.
        void Recover()
        {
            if (!failureHandled)
            {
                failureHandled = true;
                SetEyes(false);
                ReleaseCamera();
                retryAt = Time.realtimeSinceStartup + retryDelay;
                Plugin.Log.LogError($"VR session lost; the game continues flat. Retrying in {retryDelay:0} s.");
            }
            if (!restarting)
            {
                if (Time.realtimeSinceStartup < retryAt) return;
                restarting = true;
                GL.IssuePluginEvent(renderEvent, Native.EvShutdown);
                return;
            }
            if (State != XrState.None) return;  // the render thread has not shut down yet
            restarting = false;
            failureHandled = false;
            retryDelay = Mathf.Min(retryDelay * 2, MaxRetrySeconds);
            if (Native.prx_init(Application.productName, D3D12 ? 12 : 11, out _, out _) == 0)
            {
                DrainLog();
                return;  // Failed again: the next Update schedules another try
            }
            recenterPending = true;
            Native.prx_set_init_texture(eyePtrs[0]);
            GL.IssuePluginEvent(renderEvent, Native.EvInit);
            curvedTheater = CurvedLayers();
            Plugin.Log.LogInfo("OpenXR runtime found again; restarting VR.");
        }

        void Recenter()
        {
            originPos = (Xr.Position(Views[0]) + Xr.Position(Views[1])) * 0.5f;
            originYaw = Xr.Yaw(Xr.Rotation(Views[0]));
            recenterPending = false;
        }

        Camera EnsureCamera()
        {
            // Every two seconds a better camera may have come up (the game camera enabled again after a menu that
            // only had a weapon or UI camera running).
            if (GameCam != null && GameCam.isActiveAndEnabled && (Time.frameCount % 120 != 0 || PickCamera() == GameCam)) return GameCam;
            ReleaseCamera();
            GameCam = PickCamera();
            if (GameCam != null)
            {
                savedMask = GameCam.cullingMask;
                savedClear = GameCam.clearFlags;
                savedBackground = GameCam.backgroundColor;
                Plugin.Log.LogInfo("Head camera: " + GameCam.name);
            }
            return GameCam;
        }

        Camera PickCamera()
        {
            string wanted = Settings.CameraName.Value;
            Camera best = null;
            int bestScore = int.MinValue;
            foreach (var cam in Camera.allCameras)
            {
                // Orthographic cameras draw UI or 2D layers: as a head they flatten everything into a card in front of
                // both eyes. With none else the game shows on the theater screen.
                if (IsEye(cam) || cam.orthographic || !Candidate(cam)) continue;
                if (wanted.Length > 0 && cam.name == wanted) return cam;
                // tag, not CompareTag: the Unity 6 IL2CPP bindings route CompareTag through a span method that is missing there.
                // Masks we changed are scored as the game set them. The tag counts only on a camera that draws a world
                // (pixel-art games tag the camera that merely shows the world's texture on a plane).
                // A camera that clears only depth (or nothing) draws over another one: usually a weapon, hands or an
                // overlay, so it loses to a camera that clears, unless the MainCamera tag says it is the world camera
                // (drawn over a sky camera).
                bool known = others.TryGetValue(cam.GetInstanceID(), out var o);
                int mask = cam == GameCam ? savedMask : known ? o.mask : cam.cullingMask;
                var clear = cam == GameCam ? savedClear : known ? o.clear : cam.clearFlags;
                bool overlay = clear == CameraClearFlags.Depth || clear == CameraClearFlags.Nothing;
                int layers = BitCount(mask);
                // A camera under another candidate camera is its helper (Hollow Knight's blur and darkness cameras).
                int score = layers + (cam.tag == "MainCamera" && layers > 2 ? 100 : 0) - (cam.targetTexture != null && cam.targetTexture != sink ? 10 : 0) - (overlay ? 50 : 0)
                            - (UnderCamera(cam) ? 60 : 0);
                if (score > bestScore || (score == bestScore && cam == GameCam))  // ties keep the current head
                {
                    best = cam;
                    bestScore = score;
                }
            }
            return best;
        }

        // Cameras drawing to the screen, and ones drawing the world into a screen-shaped texture that the game then
        // shows on a quad (low-resolution pixel art, CRT looks): the world camera is the head there. Our own preview
        // camera and small or odd-shaped textures (mirrors, minimaps, portraits) are not candidates.
        bool UnderCamera(Camera cam)
        {
            for (var t = cam.transform.parent; t != null; t = t.parent)
            {
                var c = t.GetComponent<Camera>();
                if (c != null && !IsEye(c) && !c.orthographic && Candidate(c)) return true;
            }
            return false;
        }

        internal static bool Candidate(Camera cam)
        {
            var rt = cam.targetTexture;
            if (rt == null) return true;
            if (cam.orthographic || cam.name.StartsWith("Parallax")) return false;
            float screen = (float)Screen.width / Mathf.Max(1, Screen.height), aspect = (float)rt.width / Mathf.Max(1, rt.height);
            return Mathf.Abs(aspect - screen) < screen * 0.1f;
        }

        internal void ReleaseCamera()
        {
            RestoreCamera();
            GameCam = null;
            bodyKnown = false;
            orbitSamples = 0;
            orbitNum = orbitDen = 0;
            orbitLastFwd = Vector3.zero;
            probeIgnored = 0;
            ignoredTimes = 0;
        }

        // 2D games drawn by a perspective camera (Hollow Knight): the sprites lie in a plane tens of units ahead, so at
        // human scale the game is a small card far away and its menus fill the view. The player is scaled up until that
        // plane sits 2.5 m off; the game's layers in front of and behind it then show their depth. The plane is where
        // its 2D physics bodies are, for a camera looking straight along +Z. Checked per head camera and every 5 s.
        const float FlatPlaneDistance = 2.5f;
        float flatScale = 1f;
        Camera flatScaleOf;
        readonly Optional flatLookup = new Optional("2D scene scale");
        float Scale => Settings.WorldScale.Value * flatScale;

        void UpdateFlatScale(Camera cam)
        {
            if (flatScaleOf == cam && Time.frameCount % 300 != 0) return;
            flatScaleOf = cam;
            float was = flatScale;
            flatScale = 1f;
            flatLookup.Run(() =>
            {
                var t = cam.transform;
                if (Xr.Angle(t.forward, Vector3.forward) > 5f) return;
                var depths = new System.Collections.Generic.List<float>();
                foreach (var body in Find.All<Rigidbody2D>())
                {
                    float d = body.transform.position.z - t.position.z;
                    if (d > 1f) depths.Add(d);
                }
                if (depths.Count == 0) return;
                depths.Sort();
                flatScale = Mathf.Max(1f, depths[depths.Count / 2] / FlatPlaneDistance);
            });
            if (Mathf.Abs(flatScale - was) > 0.5f) Plugin.Log.LogInfo($"2D scene seen by {cam.name}: player scaled {flatScale:0.0}x so its plane sits {FlatPlaneDistance} m away");
        }

        // The UI panel came out opaque: an effect on the hidden head camera still painted the whole screen (image
        // effects, HDRP post, Outer Wilds, Shadows of Doubt; a pipeline pass drawing the sun, Neon White), which keying
        // black only half removes. From then on the head camera draws into a screen-sized texture of its own, off
        // screen, and the screen is cleared to transparent for the UI. Not for a URP camera stack: its overlay
        // cameras (often the HUD) would land in that texture too.
        RenderTexture sink;
        bool sinkWanted, screenCleared;
        readonly Optional clearScreen = new Optional("Screen clear");

        void Sink()
        {
            if (!sinkWanted)
            {
                if (Time.frameCount % 30 != 0 || Native.prx_ui_keyed() == 0 || CameraPost.HasStack(GameCam)) return;
                sinkWanted = true;
                Plugin.Log.LogInfo($"{GameCam.name}'s effects drew over the UI; it now renders off screen");
            }
            if (sink == null || sink.width != Screen.width || sink.height != Screen.height)
            {
                if (GameCam.targetTexture == sink) GameCam.targetTexture = null;
                if (sink != null) Destroy(sink);
                sink = new RenderTexture(Screen.width, Screen.height, 24, RenderTextureFormat.ARGB32) { name = "Parallax Sink" };
            }
            if (GameCam.targetTexture == null) GameCam.targetTexture = sink;
            screenCleared = false;
            if (GameCam.targetTexture != sink) return;
            screenCleared = clearScreen.Run(() =>
            {
                var was = RenderTexture.active;
                RenderTexture.active = null;
                GL.Clear(true, true, Color.clear);
                RenderTexture.active = was;
            });
        }

        void HideGameCamera()
        {
            if (GameCam.cullingMask != 0) savedMask = GameCam.cullingMask;
            if (GameCam.clearFlags != CameraClearFlags.SolidColor || GameCam.backgroundColor != Color.clear)
            {
                savedClear = GameCam.clearFlags;
                savedBackground = GameCam.backgroundColor;
            }
            GameCam.cullingMask = 0;
            GameCam.clearFlags = CameraClearFlags.SolidColor;
            GameCam.backgroundColor = Color.clear;
            camHidden = true;
        }

        // Weapon and hand cameras (perspective, world layers) draw over the head camera's picture: the eyes take their
        // layers, and on the screen they keep only the UI layer, clearing to transparent if they cleared at all.
        // Orthographic cameras are left drawing: they usually carry the HUD. None may run post-processing.
        void HideOtherCameras()
        {
            extraMask = 0;
            extraNear = float.MaxValue;
            extraFar = 0;
            stack.Clear();
            foreach (var cam in Camera.allCameras)
            {
                if (cam == GameCam || IsEye(cam) || cam.targetTexture != null || cam.rect.width < 0.99f || cam.rect.height < 0.99f) continue;
                bool known = others.TryGetValue(cam.GetInstanceID(), out var o);
                if (!known) o = new Other { cam = cam, mask = cam.cullingMask, clear = cam.clearFlags, background = cam.backgroundColor, post = CameraPost.Off(cam) };
                int world = cam.orthographic ? 0 : o.mask & ~UiLayers;
                // A mask that is not ours means the game changed it since last frame.
                if (known && cam.cullingMask != (world != 0 ? o.mask & UiLayers : o.mask))
                {
                    o.mask = cam.cullingMask;
                    world = cam.orthographic ? 0 : o.mask & ~UiLayers;
                }
                if (world != 0)
                {
                    extraMask |= world;
                    stack.Add(cam.GetInstanceID());
                    extraNear = Mathf.Min(extraNear, Mathf.Max(0.01f, cam.nearClipPlane));
                    extraFar = Mathf.Max(extraFar, cam.farClipPlane);
                    cam.cullingMask = o.mask & UiLayers;
                    if (cam.clearFlags != CameraClearFlags.Depth && cam.clearFlags != CameraClearFlags.Nothing)
                    {
                        if (cam.backgroundColor != Color.clear || cam.clearFlags != CameraClearFlags.SolidColor)
                        {
                            o.clear = cam.clearFlags;
                            o.background = cam.backgroundColor;
                        }
                        cam.clearFlags = CameraClearFlags.SolidColor;
                        cam.backgroundColor = Color.clear;
                    }
                }
                others[cam.GetInstanceID()] = o;
            }
        }

        void RestoreCamera()
        {
            screenCleared = false;
            canvases.Restore();
            screenQuads.Restore();
            handHeld.Restore();
            if (camHidden && GameCam != null)
            {
                GameCam.cullingMask = savedMask;
                GameCam.clearFlags = savedClear;
                GameCam.backgroundColor = savedBackground;
                if (sink != null && GameCam.targetTexture == sink) GameCam.targetTexture = null;
            }
            headPost?.Invoke();
            headPost = null;
            extrasCamera = null;
            foreach (var o in others.Values)
            {
                if (o.cam == null) continue;
                o.cam.cullingMask = o.mask;
                o.cam.clearFlags = o.clear;
                o.cam.backgroundColor = o.background;
                o.post?.Invoke();
            }
            others.Clear();
            extraMask = 0;
            camHidden = false;
        }

        void SetEyes(bool on)
        {
            foreach (var eye in Eyes)
                if (eye != null) eye.enabled = on;
            if (!on)
                foreach (var passes in stackEyes)
                    foreach (var pass in passes) pass.enabled = false;
        }

        internal bool IsEye(Camera cam) => eyeIds.Contains(cam.GetInstanceID());

        // The menu's preview of the head camera, which is blanked while the eyes render in its place.
        internal void RestoreForPreview(Camera preview)
        {
            if (!camHidden) return;
            preview.cullingMask = savedMask;
            preview.clearFlags = savedClear;
            preview.backgroundColor = savedBackground;
        }

        internal void ApplyRenderScale() => CreateEyeTargets();

        static float SnapAngle(TurnMode m) => m == TurnMode.Snap15 ? 15f : m == TurnMode.Snap45 ? 45f : 30f;

        // A flick of the right stick turns the game camera by the snap angle. Games turn by mouse or stick at their own
        // sensitivity, so this pushes until the camera's yaw arrives: mouse pixels from a learned degrees-per-pixel
        // (keyboard + mouse), or a proportional stick (gamepad). Returns this frame's push (PrxInputConfig.turnX).
        float SnapTurn(bool pad)
        {
            float yaw = Heading(GameCam.transform), rx = controllers.rx;
            if (!snapping)
            {
                if (Mathf.Abs(rx) < 0.3f) snapArmed = true;
                if (!snapArmed || Mathf.Abs(rx) < 0.7f) return 0;
                snapArmed = false;
                snapping = true;
                snapTarget = yaw + Mathf.Sign(rx) * SnapAngle(Settings.Turning.Value);
                snapUntil = Time.unscaledTime + 0.6f;
                snapLastYaw = yaw;
                snapLastPush = 0;
            }
            float moved = Mathf.DeltaAngle(snapLastYaw, yaw);
            if (!pad && Mathf.Abs(snapLastPush) >= 4 && Mathf.Abs(moved) > 0.05f && Mathf.Sign(moved) == Mathf.Sign(snapLastPush))
            {
                float sample = moved / snapLastPush;
                degPerPixel = degPerPixel > 0 ? Mathf.Lerp(degPerPixel, sample, 0.5f) : sample;
            }
            snapLastYaw = yaw;
            float err = Mathf.DeltaAngle(yaw, snapTarget);
            if (Mathf.Abs(err) < 1.5f || Time.unscaledTime > snapUntil)
            {
                snapping = false;
                return snapLastPush = 0;
            }
            // Half the remaining error per frame: the game may apply input a frame late, so a full step would overshoot.
            if (pad) snapLastPush = Mathf.Sign(err) * Mathf.Clamp(Mathf.Abs(err) / 20f, 0.3f, 1f);
            else snapLastPush = degPerPixel > 0 ? Mathf.Clamp(err * 0.5f / degPerPixel, -600, 600) : Mathf.Sign(err) * 40f;
            return snapLastPush;
        }

        // Comfort vignette: strongest while the player moves or turns (stick input, or the game camera itself moving),
        // eased in fast and out slowly. Jumps faster than 50 m/s are teleports or scene loads, not motion.
        float Vignette()
        {
            var level = Settings.Vignette.Value;
            if (level == VignetteLevel.Off || GameCam == null) return 0;
            float dt = Mathf.Max(Time.unscaledDeltaTime, 1e-3f);
            var pos = GameCam.transform.position;
            float yaw = bodyYaw;
            float speed = (pos - vignetteLastPos).magnitude / dt, turn = Mathf.Abs(Mathf.DeltaAngle(vignetteLastYaw, yaw)) / dt;
            vignetteLastPos = pos;
            vignetteLastYaw = yaw;
            float stick = Xr.Sqrt(controllers.lx * controllers.lx + controllers.ly * controllers.ly);
            float motion = Mathf.Max(Mathf.Max(stick, snapping ? 1f : 0f), Mathf.Max(speed < 50 ? speed / 3f : 0f, turn / 90f));
            float target = (level == VignetteLevel.High ? 1f : 0.55f) * Mathf.Clamp01(motion);
            return Mathf.MoveTowards(VignetteNow, target, dt * (target > VignetteNow ? 4f : 1.5f));
        }

        Vector3 Smoothed(Vector3 pos)
        {
            float now = Time.realtimeSinceStartup;
            if (pos != stepTo)
            {
                if ((pos - stepTo).sqrMagnitude > 4f) stepFrom = pos;  // a jump: teleport or cut, no line to it
                else stepFrom = stepTo;
                stepLen = Mathf.Clamp(now - stepAt, 0.005f, 0.1f);
                stepTo = pos;
                stepAt = lastMoveAt = now;
                stepMoved++;
            }
            else if (now - lastMoveAt < 0.1f) stepStill++;
            // Judged every 90 frames: a fifth of the moving frames standing still turns it on, a twentieth turns it off.
            if (++stepFrames >= 90)
            {
                float still = stepMoved > 20 ? (float)stepStill / (stepMoved + stepStill) : 0;
                if (still > 0.2f) StepSmoothing = true;
                else if (still < 0.05f) StepSmoothing = false;
                stepFrames = stepMoved = stepStill = 0;
            }
            return StepSmoothing ? Vector3.Lerp(stepFrom, stepTo, (now - stepAt) / stepLen) : pos;
        }

        // Compass heading of a camera, stable when it looks straight up or down: the forward's horizontal part vanishes
        // there and the up vector's takes over.
        float Heading(Transform t)
        {
            var un = Quaternion.Inverse(upFrame);
            Vector3 f = un * t.forward, h = f - un * t.up * f.y;
            return Xr.Atan2(h.x, h.z) * Mathf.Rad2Deg;
        }

        float Pitch(Transform t) => Xr.Asin(Mathf.Clamp((Quaternion.Inverse(upFrame) * t.forward).y, -1, 1)) * Mathf.Rad2Deg;

        Vector3 PlayerUp()
        {
            if (playerBodyOf != GameCam || Time.frameCount % 120 == 0)
            {
                playerBodyOf = GameCam;
                playerBody = null;
                bodyLookup.Run(() =>
                {
                    for (var t = GameCam.transform; t != null && playerBody == null; t = t.parent) playerBody = t.GetComponent<Rigidbody>();
                    noWorldGravity = Physics.gravity.sqrMagnitude < 0.01f;
                });
                Transform carrier = playerBody != null && playerBody.transform != GameCam.transform ? playerBody.transform : null;
                if (carrier == null) bodyLookup.Run(() =>
                {
                    for (var t = GameCam.transform.parent; t != null && carrier == null; t = t.parent)
                        if (t.GetComponent<CharacterController>() != null) carrier = t;
                });
                // Over the body's centre, as a head is; a third-person rig parented to the player sits metres behind.
                carried = false;
                if (carrier != null)
                {
                    var off = GameCam.transform.position - carrier.position;
                    carried = (off - Vector3.Project(off, carrier.up)).magnitude < 0.6f;
                }
            }
            // Only where the game turned world gravity off and pulls the body itself: an FPS body that merely skips
            // Unity's gravity may still pitch or roll with the view (Neon White drifted sideways).
            return playerBody != null && !playerBody.useGravity && noWorldGravity ? playerBody.transform.up : Vector3.up;
        }

        void TrackBody(Transform camT)
        {
            upFrame = Quaternion.FromToRotation(upFrame * Vector3.up, PlayerUp()) * upFrame;
            float heading = Heading(camT);
            if (!bodyKnown) bodyYaw = heading;
            else if (!aiming) bodyYaw += Mathf.DeltaAngle(lastHeading, heading);
            bodyKnown = true;
            lastHeading = heading;
            SampleOrbit(camT);
        }

        void SampleOrbit(Transform t)
        {
            Vector3 p = t.position, f = t.forward, dc = p - orbitLastPos, df = f - orbitLastFwd;
            bool primed = orbitLastFwd != Vector3.zero;
            orbitLastPos = p;
            orbitLastFwd = f;
            // A turn of at least 0.3 degrees; a jump over half a metre is a teleport, a cut or a dash, which says nothing
            // about the pivot. Nor does a turn while the player walks: circle-strafing a target moves a first-person
            // camera exactly like an orbit (ULTRAKILL flipped between verdicts), so only turns standing still count.
            bool walking = controllers.lx * controllers.lx + controllers.ly * controllers.ly > 0.04f;
            if (!primed || walking || df.sqrMagnitude < 2.7e-5f || dc.sqrMagnitude > 0.25f) return;
            orbitNum = orbitNum * 0.99f - Vector3.Dot(dc, df);
            orbitDen = orbitDen * 0.99f + df.sqrMagnitude;
            orbitSamples++;
        }

        internal float OrbitRadius => orbitDen > 0 ? orbitNum / orbitDen : 0;
        internal bool Aiming => aiming;

        // Tracking space (metres from the recenter point) to the game world, as the eyes are placed this frame.
        Vector3 eyeBase;
        float eyeScale = 1;
        Vector3 ToWorld(Vector3 tracking) => eyeBase + Body * (Quaternion.Inverse(originYaw) * (tracking - originPos) * eyeScale);
        Vector3 ToTracking(Vector3 world) => originPos + originYaw * (Quaternion.Inverse(Body) * (world - eyeBase) / eyeScale);

        // The game's crosshair sits at the middle of its UI, which in VR is a panel that stays put while the camera
        // follows the head or hand, so it points elsewhere. While aiming, that middle (with the game's grab and use
        // icons, prompts and hit markers) moves to where the game camera actually aims, at the depth of what it hits,
        // or a lime reticle is drawn there instead.
        readonly float[] reticle = new float[6];
        bool reticleShown;
        internal float[] Reticle => reticleShown ? reticle : new float[3];
        internal Vector3 ReticleAt { get; private set; }
        internal bool PatchShown => reticleShown && Settings.Crosshair.Value == CrosshairMode.Game;
        readonly Optional aimRay = new Optional("Crosshair distance");
        const float ReticleDeg = 0.45f, ReticleFar = 40f, PatchW = 0.09f, PatchH = 0.08f;

        void PlaceReticle(Transform camT)
        {
            reticleShown = !windowShown && aiming && Settings.Crosshair.Value != CrosshairMode.Off;
            if (!reticleShown) return;
            float dist = ReticleFar;
            aimRay.Run(() =>
            {
                if (Physics.Raycast(camT.position, camT.forward, out var hit, ReticleFar, Physics.DefaultRaycastLayers, QueryTriggerInteraction.Ignore))
                    dist = Mathf.Max(hit.distance, 0.3f);
            });
            ReticleAt = camT.position + camT.forward * dist;
            for (int e = 0; e < 2; e++)
            {
                var t = Eyes[e].transform;
                var p = Quaternion.Inverse(t.rotation) * (ReticleAt - t.position);
                var fov = Fovs[e];
                float l = Xr.Tan(fov.left), r = Xr.Tan(fov.right), u = Xr.Tan(fov.up), d = Xr.Tan(fov.down);
                bool front = p.z > 0.01f;
                reticle[e * 3] = front ? (p.x / p.z - l) / (r - l) : 0;
                reticle[e * 3 + 1] = front ? (u - p.y / p.z) / (u - d) : 0;
                reticle[e * 3 + 2] = front ? Xr.Tan(ReticleDeg * Mathf.Deg2Rad) / (u - d) : 0;
            }
        }
        internal float BodyYaw => bodyYaw;

        // On is unconditional outside the VR menu. Auto needs the game's cursor hidden (play, not its menus); after 40
        // turning frames the camera gets a verdict (first person below 0.45 m), which only strong evidence flips (third
        // person above 1 m, first person again below 0.2 m), so camera shake and dashes do not toggle it.
        bool HeadAimWanted()
        {
            var mode = Settings.HeadAim.Value;
            if (mode == AimMode.Off || Settings.InputNow == InputMode.Off || GameCam == null || !camHidden || !bodyKnown
                || Menu.Open || Hider.Active || State != XrState.Running || windowShown)
                return false;
            if (Time.unscaledTime < aimSuspendedUntil) return false;
            // A free cursor is a menu: mouse pushes would drag it sideways (stick pushes leave it be when forced on).
            if (Cursor.visible && Cursor.lockState != CursorLockMode.Locked && (mode != AimMode.On || Settings.InputNow != InputMode.Gamepad)) return false;
            if (mode == AimMode.On) return true;
            string key = AimKey;
            bool known = firstPerson.TryGetValue(key, out bool fp);
            if (orbitSamples >= 40)
            {
                float r = OrbitRadius;
                if (!known) fp = r < 0.45f;
                else if (fp && r > 1f) fp = false;
                else if (!fp && r < 0.2f) fp = true;
                if (!known || firstPerson[key] != fp) Plugin.Log.LogInfo($"Head aim (auto): {key} turns about {r:0.00} m from itself, {(fp ? "first" : "third")} person");
                firstPerson[key] = fp;
                return fp;
            }
            if (known) return fp;
            return carried && ignoredTimes < 2;
        }

        // Steers the game camera to where the head looks, closed loop on its angles: mouse pixels from a learned
        // degrees-per-pixel (half the error per frame, as the game may apply input a frame late), or a proportional
        // right stick. The right stick turns the body instead, smoothly or in snaps. Sets this frame's push (aimPushX/Y).
        // No ref parameters on Rig: IL2CPP's class injector cannot convert them.
        void HeadAim(bool pad)
        {
            float rx = controllers.rx;
            if (Settings.Turning.Value == TurnMode.Smooth) bodyYaw += rx * Mathf.Abs(rx) * SmoothTurnDegPerSec * Mathf.Min(Time.unscaledDeltaTime, 0.1f);
            else if (Mathf.Abs(rx) < 0.3f) snapArmed = true;
            else if (snapArmed && Mathf.Abs(rx) > 0.7f)
            {
                snapArmed = false;
                bodyYaw += Mathf.Sign(rx) * SnapAngle(Settings.Turning.Value);
            }
            handAiming = Settings.AimWith.Value == AimHand.Hand && controllers.aimValidR != 0;
            if (handAiming) handAim = controllers.aimR;
            var look = Quaternion.Inverse(originYaw) * Xr.Rotation(handAiming ? controllers.aimR : Views[0]) * Vector3.forward;
            float wantYaw = bodyYaw + Xr.Atan2(look.x, look.z) * Mathf.Rad2Deg;
            float wantPitch = Mathf.Clamp(Xr.Asin(Mathf.Clamp(look.y, -1, 1)) * Mathf.Rad2Deg, -80, 80);
            var camT = GameCam.transform;
            float yaw = Heading(camT), pitch = Pitch(camT);
            float movedX = Mathf.DeltaAngle(aimLastYaw, yaw), movedY = pitch - aimLastPitch;
            float errX = Mathf.DeltaAngle(yaw, wantYaw), errY = wantPitch - pitch;
            if (pad)
            {
                AimLoop.LearnStick(ref padSignY, ref padWrongY, movedY, aimPushY);
                aimPushX = Mathf.Abs(errX) < 1f ? 0 : Mathf.Sign(errX) * Mathf.Clamp(Mathf.Abs(errX) / 25f, 0.3f, 1f) * padSignX;
                aimPushY = Mathf.Abs(errY) < 1f ? 0 : Mathf.Sign(errY) * Mathf.Clamp(Mathf.Abs(errY) / 25f, 0.3f, 1f) * padSignY;
            }
            else
            {
                AimLoop.LearnPixels(ref aimDppX, ref aimWrongX, movedX, aimPushX, false);
                AimLoop.LearnPixels(ref aimDppY, ref aimWrongY, movedY, aimPushY, true);
                aimPushX = AimLoop.PixelPush(errX, aimDppX);
                aimPushY = AimLoop.PixelPush(errY, aimDppY);
            }
            aimLastYaw = yaw;
            aimLastPitch = pitch;
            // Pushes the camera ignores for 1.5 s (a camera the game does not steer, input it drops) pause head aim for
            // 3 s in any mode, so it never spins the view endlessly; then it tries again.
            if (Mathf.Abs(errX) > 5f && Mathf.Abs(movedX) < 0.05f) probeIgnored += Time.unscaledDeltaTime;
            else if (Mathf.Abs(movedX) > 0.2f) probeIgnored = 0;
            if (probeIgnored > 1.5f)
            {
                probeIgnored = 0;
                aimSuspendedUntil = Time.unscaledTime + 3f;
                ignoredTimes++;
                Plugin.Log.LogInfo($"Head aim: {AimKey} ignores the pushes; paused for 3 s");
            }
        }

        // Watch check, as DesktopVR's clock: the left controller close to the face with the back of the hand (its outer,
        // -X side) turned toward the eyes, and the eyes on it. The face fades in with both, so it is out of sight while
        // you play; glanced at (face turned within 45 degrees, mostly shown) it lights and the trigger opens the menu.
        // The right hand pointing at a shown watch counts as a glance too.
        bool WristGlance()
        {
            watchShown = Settings.WristMenu.Value && controllers.aimValidL != 0 && State == XrState.Running && !Menu.Open;
            bool looked = false;
            bool glance = false;
            float dt = Time.unscaledDeltaTime;
            if (watchShown)
            {
                var hand = Xr.Rotation(controllers.aimL);
                watchAt = Xr.Position(controllers.aimL) + hand * new Vector3(-0.035f, 0.01f, -0.12f);
                var toHead = (Xr.Position(Views[0]) + Xr.Position(Views[1])) / 2 - watchAt;
                float dist = toHead.magnitude;
                if (dist <= 0.7f && dist > 1e-4f)
                {
                    var toEye = toHead / dist;
                    float facing = Vector3.Dot(hand * Vector3.left, toEye);
                    float looking = Vector3.Dot(Xr.Rotation(Views[0]) * Vector3.forward, -toEye);
                    // Test runs hold the wrist up without turning the null headset's view to it.
                    if (Test?.Wrist == true) looking = 1f;
                    looked = watchLooked ? facing > KeepFacing && looking > KeepLooking : facing > ShowFacing && looking > ShowLooking;
                    // Lit (a trigger pull opens the menu) when turned toward the eyes, or under the laser.
                    glance = looked && facing > 0.6f;
                    if (!glance && looked && controllers.aimValidR != 0)
                    {
                        var from = Xr.Position(controllers.aimR);
                        var dir = Xr.Rotation(controllers.aimR) * Vector3.forward;
                        var to = watchAt - from;
                        float along = Vector3.Dot(to, dir);
                        glance = along > 0 && along < 0.6f && (to - dir * along).magnitude < 0.06f;
                    }
                }
            }
            watchHeld += dt;
            if (looked != watchLooked && (looked || watchHeld >= WatchHoldS))
            {
                watchLooked = looked;
                watchHeld = 0;
            }
            float target = watchShown && watchLooked ? 1f : 0f;
            watchFade = !Motion.On ? target
                : target > watchFade ? Mathf.Min(target, watchFade + dt / WatchShowS)
                : Mathf.Max(target, watchFade - dt / WatchHideS);
            watchGlow = Motion.Follow(watchGlow, glance ? 1f : 0f, WatchGlowS, dt);
            return glance;
        }

        static int BitCount(int v)
        {
            int n = 0;
            for (uint u = (uint)v; u != 0; u &= u - 1) n++;
            return n;
        }

        bool MenuKeyPressed()
        {
            bool down = Native.prx_key_down(Settings.MenuKey.Value) != 0;
            bool pressed = down && !menuKeyDown;
            menuKeyDown = down;
            return pressed;
        }

        static void DrainLog()
        {
            string line;
            while ((line = Native.PollLog()) != null) Plugin.Log.LogInfo("[native] " + line);
        }

        internal Stats GetStats()
        {
            Native.prx_get_stats(out var s);
            return s;
        }

        internal Vector2Int EyeSize => new Vector2Int(eyeTargets[0].width, eyeTargets[0].height);
    }

    // Head aim loop helpers, outside Rig so the IL2CPP class injector never sees their ref parameters.
    internal static class AimLoop
    {
        // Over 2 degrees per pixel is not our push. Movement against the expected direction (up is up until shown
        // otherwise) is noise: bob, recoil, screen shake. Only a camera clearly running away from strong pushes, five
        // frames in a row, inverts an axis that may be inverted (Y); horizontal look never is, as a wrong sign there
        // holds the camera facing backwards. A noise-learned flip on Y pinned ULTRAKILL's view to the floor.
        internal static void LearnPixels(ref float dpp, ref int wrong, float moved, float push, bool invertible)
        {
            if (Mathf.Abs(push) < 4 || Mathf.Abs(moved) < 0.05f) return;
            float sample = moved / push;
            if (Mathf.Abs(sample) > 2f) return;
            if (Mathf.Sign(sample) != (dpp != 0 ? Mathf.Sign(dpp) : 1f))
            {
                wrong = Mathf.Abs(push) >= 20 && Mathf.Abs(moved) >= 1f ? wrong + 1 : 0;
                if (!invertible || wrong < 5) return;
                dpp = 0;
            }
            wrong = 0;
            dpp = dpp != 0 ? Mathf.Lerp(dpp, sample, 0.3f) : sample;
        }

        internal static void LearnStick(ref float sign, ref int wrong, float moved, float push)
        {
            if (Mathf.Abs(push) < 0.3f || Mathf.Abs(moved) < 0.05f) return;
            if (Mathf.Sign(moved) == Mathf.Sign(push))  // push is already signed
            {
                wrong = 0;
                return;
            }
            wrong = Mathf.Abs(push) >= 0.5f && Mathf.Abs(moved) >= 1f ? wrong + 1 : 0;
            if (wrong < 5) return;
            sign = -sign;
            wrong = 0;
        }

        // Before the rate is learned, a small probe in the error's direction.
        internal static float PixelPush(float err, float dpp)
        {
            if (Mathf.Abs(err) < 0.3f) return 0;
            return dpp != 0 ? Mathf.Clamp(err * 0.5f / dpp, -800, 800) : Mathf.Sign(err) * Mathf.Min(Mathf.Abs(err) * 2, 30);
        }
    }
}
