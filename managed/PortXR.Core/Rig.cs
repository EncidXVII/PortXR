// Copyright (C) 2026 EncidXVII
// SPDX-License-Identifier: GPL-3.0-only

using System;
using System.Collections;
using UnityEngine;
using UnityEngine.SceneManagement;

namespace PortXR
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
        Camera extrasCamera;
        bool beforeRenderHooked;
        int sceneHandle;

        // The core's frame (shared with every engine): the controls, the laser, the watch and the screen run there; this
        // side reports the game camera and what only Unity knows, and renders the eyes where the core puts them.
        internal RigOut Out;
        bool located;
        bool uiTaken;            // Unity's own UI events took the laser's click last frame
        float worldReach = 3f;   // the hide-object tool's beam: metres to what it would hide
        // The player body under the head camera: its own up where the game applies its own gravity (Outer Wilds' small
        // planets), and whether the camera rides on it (first person until its turns say otherwise).
        Rigidbody playerBody;
        bool noWorldGravity, carried;
        Camera playerBodyOf;
        string sceneName = "";
        internal bool StepSmoothing => Out.smoothing != 0;
        internal float VignetteNow => Out.vignette;
        internal bool PointerActive;
        internal readonly Hider Hider = new Hider();
        Scripts scripts;  // made on the first Update: IL2CPP's injected classes cannot pass `this` in a field initialiser
        readonly CanvasOverlay canvases = new CanvasOverlay();
        readonly ScreenQuads screenQuads = new ScreenQuads();
        readonly HandHeld handHeld = new HandHeld();
        bool vrNoted;  // played.txt beside the plugin, written at the first frame sent to the headset
        internal int Held { get; private set; }  // viewmodel parts moved into the hand this frame
        internal int AimValid => Out.beam;
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
            pointerDot = new Optional("Pointer dot"), resetView = new Optional("Eye view matrix reset"),
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
                var go = new GameObject("PortXR Eye " + e);
                DontDestroyOnLoad(go);
                Eyes[e] = go.AddComponent<Camera>();
                Eyes[e].enabled = false;
                eyeIds.Add(Eyes[e].GetInstanceID());
            }
            builtIn = BuiltInPipeline();
            Native.prx_set_init_texture(eyePtrs[0]);
            GL.IssuePluginEvent(renderEvent, Native.EvInit);

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
                    name = "PortXR Eye " + e,
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
            Menu.Update();
            // The laser on the game's UI (placed by the core last frame): Unity's own UI events take the click when they
            // can, the OS mouse button otherwise.
            var pointerAt = new Vector2(Out.pointerU * Screen.width, (1 - Out.pointerV) * Screen.height);
            uiTaken = UiPointer.Update(PointerActive, pointerAt, Out.controllers.rTrigger, Out.controllers.ry);
            Hider.Apply();
            // Without a located headset the core runs no frame: the emulated keys and buttons are let go.
            if (!located)
            {
                var idle = new InputConfig { mode = 2, neutral = 1, binds = new int[12], twice = new int[10], held = new int[10], combo = new int[10] };
                Native.prx_input_update(ref idle, null, out _);
            }
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
            if (State != XrState.Running)
            {
                located = false;
                SetEyes(false);
                RestoreCamera();
                return;
            }
            var cam = Settings.Display.Value != DisplayMode.Theater && !forcedTheater && !VideoWatch.Playing() ? EnsureCamera() : null;
            // What only Unity knows this frame; the core (shared with every engine) runs the controls and places the eyes.
            var game = new Game
            {
                cursorFree = Cursor.visible && Cursor.lockState != CursorLockMode.Locked ? 1 : 0,
                padGame = Settings.PadGame() ? 1 : 0,
                tests = Test != null ? 1 : 0,
                uiAspect = (float)Screen.width / Mathf.Max(1, Screen.height),
                upY = 1,
                sceneScale = 1,
                worldPointer = Hider.Active ? 1 : 0,
                worldReach = worldReach,
                screenW = Screen.width,
                screenH = Screen.height,
                pointerClick = uiTaken ? 0 : 1,
                cameraKey = "",
            };
            if (cam != null)
            {
                var camT = cam.transform;
                var up = PlayerUp();
                UpdateFlatScale(cam);
                var nudge = scripts != null ? scripts.EyeOffset : Vector3.zero;
                game.hasCamera = 1;
                game.camera = Xr.ToPose(camT.position, camT.rotation);
                game.cameraKey = sceneName + "/" + cam.name;
                game.carried = carried ? 1 : 0;
                game.fovY = cam.fieldOfView * Mathf.Deg2Rad;
                game.nearClip = cam.nearClipPlane;
                game.focus = windowShown ? FocusRays(cam) : 0;
                game.upX = up.x; game.upY = up.y; game.upZ = -up.z;
                game.sceneScale = flatScale;
                game.offsetX = nudge.x; game.offsetY = nudge.y; game.offsetZ = -nudge.z;
            }
            Test?.Apply(this);
            located = Native.prx_bridge_frame(ref game, out Out) != 0;
            if (!located)
            {
                SetEyes(false);
                RestoreCamera();
                return;
            }
            Views[0] = Out.viewL;
            Views[1] = Out.viewR;
            Fovs[0] = Out.fovL;
            Fovs[1] = Out.fovR;
            Menu.Opened(Out.menuOpen != 0);
            if (Out.quit != 0) Application.Quit();
            if (Out.renderScale != 0) ApplyRenderScale();
            if (Out.cameraPicked != 0) ReleaseCamera();
            if (Out.showHidden != 0) Hider.ShowAll();
            if (Out.hideObjects != 0) Hider.Start();
            AimHider();
            PointerActive = Out.pointer != 0;
            windowShown = cam != null && Out.mode == 1;
            if (cam == null || Out.mode == 2)
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
            var camT2 = cam.transform;
            screenQuads.Grow(cam);
            if (windowShown) EnsureWindowTargets();
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
                if (windowShown)
                {
                    // PortXR: the window's two cameras, beside the game camera along its right, through the core's frustum.
                    float offset = e == 0 ? Out.windowOffsetL : Out.windowOffsetR;
                    eye.transform.SetPositionAndRotation(camT2.position + camT2.rotation * new Vector3(offset, 0, 0), camT2.rotation);
                    SetProjection(eye, e == 0 ? Out.windowFovL : Out.windowFovR);
                }
                else
                {
                    var pose = e == 0 ? Out.eyeL : Out.eyeR;
                    eye.transform.SetPositionAndRotation(Xr.Position(pose), Xr.Rotation(pose));
                    SetProjection(eye, Fovs[e]);
                }
                eye.enabled = true;
                StackPasses(e, eye, refreshExtras);
            }
            if (refreshExtras) headPost = CameraPost.Off(cam);
            PlaceReticle(camT2);
            Held = 0;
            if (Out.aiming != 0 && Out.handAiming != 0 && Settings.WeaponInHand.Value)
            {
                var p = Out.handAim;
                handHeld.Move(cam, ToWorld(Xr.Position(p) + Xr.Rotation(p) * new Vector3(0, 0, 0.08f)));
                Held = handHeld.Held;
            }
            eyesRendered = true;
        }

        // The hide-object tool has the laser: it goes out into the game world from where the right hand is there, and
        // ends on what it would hide (next frame's beam). Opening the VR menu ends the tool.
        void AimHider()
        {
            if (!Hider.Active) return;
            if (Out.menuOpen != 0)
            {
                Hider.Stop();
                return;
            }
            var c = Out.controllers;
            if (c.aimValidR == 0 || GameCam == null) return;
            var from = Xr.Position(c.aimR);
            var dir = DirToWorld(Xr.Rotation(c.aimR) * Vector3.forward);
            Hider.Aim(new Ray(ToWorld(from), dir), c.rTrigger > 0.6f, (c.buttons & Buttons.B) != 0);
            worldReach = Hider.Target != null ? Hider.TargetDistance / Mathf.Max(1e-4f, Out.scale) : 3f;
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
                    var go = new GameObject("PortXR Eye " + e + "." + k);
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
                SetProjection(pass, flat ? MiddleFov() : EyeFov(e));
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
        bool windowShown;
        readonly Optional focusRay = new Optional("PortXR focus");

        internal bool WindowShown => windowShown;
        internal float Focus => Out.focus;

        Fov EyeFov(int e) => windowShown ? (e == 0 ? Out.windowFovL : Out.windowFovR) : Fovs[e];

        // The flat view from between the window's two cameras: tangents are linear in the cameras' offset.
        Fov MiddleFov()
        {
            Fov a = Out.windowFovL, b = Out.windowFovR;
            float Mid(float x, float y) => Xr.Atan((Xr.Tan(x) + Xr.Tan(y)) / 2);
            return new Fov { left = Mid(a.left, b.left), right = Mid(a.right, b.right), up = Mid(a.up, b.up), down = Mid(a.down, b.down) };
        }

        // What sits at the middle of the view: the median of five rays around the centre (the character of a
        // third-person game, the wall ahead in first person); 0 when they hit nothing. The core keeps it within reach
        // and eases it, and a 2D scene's play plane wins there.
        float FocusRays(Camera cam)
        {
            float found = 0;
            focusRay.Run(() =>
            {
                var hits = new System.Collections.Generic.List<float>();
                for (int i = 0; i < 5; i++)
                {
                    var at = new Vector3(0.5f + (i == 1 ? -0.1f : i == 2 ? 0.1f : 0), 0.5f + (i == 3 ? -0.1f : i == 4 ? 0.1f : 0), 0);
                    if (Physics.Raycast(cam.ViewportPointToRay(at), out var hit, cam.farClipPlane, savedMask, QueryTriggerInteraction.Ignore)) hits.Add(hit.distance);
                }
                if (hits.Count == 0) return;
                hits.Sort();
                found = hits[hits.Count / 2];
            });
            return found;
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
                windowTargets[e] = new RenderTexture(w, h, 32, eye.format, RenderTextureReadWrite.Default) { name = "PortXR Window " + e, antiAliasing = eye.antiAliasing };
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
            // The theater screen and the PortXR window still need eye images for the hand menu and the beam:
            // transparent ones, drawn over the screen.
            bool layered = stereo || window || ((Out.watch != 0 || Out.beam != 0) && !VideoWatch.Playing());
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
            }
            f.quadLayer = new Layer { texture = uiPtr, flipY = CaptureFlip };
            // The core's parts: eye poses, the UI panel or the (curved, where the runtime can) screen, beam, watch,
            // vignette, theme accent, passthrough.
            Native.prx_bridge_layers(ref f, (float)Screen.width / Mathf.Max(1, Screen.height));
            if (!layered) f.beam = f.watch = 0;
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
                    float grow = (at - headAt).magnitude / Mathf.Max(0.1f, (Xr.Position(f.quadPose) - headAt).magnitude);
                    f.patchShown = 1;
                    f.aimPose = Xr.ToPose(at, Quaternion.LookRotation(at - headAt, Vector3.up));
                    f.aimW = 2 * f.patchW * f.quadWidth * grow;
                    f.aimH = 2 * f.patchH * f.quadHeight * grow;
                }
                else
                {
                    f.aimLX = reticle[0]; f.aimLY = reticle[1]; f.aimLR = reticle[2];
                    f.aimRX = reticle[3]; f.aimRY = reticle[4]; f.aimRR = reticle[5];
                }
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
                name = "PortXR UI",
            };
            uiTarget.Create();
            uiPtr = uiTarget.GetNativeTexturePtr();
        }

        internal void RequestRecenter() => Native.prx_bridge_recenter();

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
            Native.prx_bridge_recenter();
            Native.prx_set_init_texture(eyePtrs[0]);
            GL.IssuePluginEvent(renderEvent, Native.EvInit);
            Plugin.Log.LogInfo("OpenXR runtime found again; restarting VR.");
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
            if (cam.orthographic || cam.name.StartsWith("PortXR")) return false;
            float screen = (float)Screen.width / Mathf.Max(1, Screen.height), aspect = (float)rt.width / Mathf.Max(1, rt.height);
            return Mathf.Abs(aspect - screen) < screen * 0.1f;
        }

        internal void ReleaseCamera()
        {
            RestoreCamera();
            GameCam = null;
        }

        static int BitCount(int v)
        {
            int n = 0;
            for (uint u = (uint)v; u != 0; u &= u - 1) n++;
            return n;
        }

        // 2D games drawn by a perspective camera (Hollow Knight): the sprites lie in a plane tens of units ahead, so at
        // human scale the game is a small card far away and its menus fill the view. The player is scaled up until that
        // plane sits 2.5 m off; the game's layers in front of and behind it then show their depth. The plane is where
        // its 2D physics bodies are, for a camera looking straight along +Z. Checked per head camera and every 5 s.
        const float FlatPlaneDistance = 2.5f;
        float flatScale = 1f;
        Camera flatScaleOf;
        readonly Optional flatLookup = new Optional("2D scene scale");


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
                sink = new RenderTexture(Screen.width, Screen.height, 24, RenderTextureFormat.ARGB32) { name = "PortXR Sink" };
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

        internal void ApplyRenderScale() => CreateEyeTargets();

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

        internal float OrbitRadius => Out.orbitRadius;
        internal bool Aiming => Out.aiming != 0;

        // Tracking space (Unity's handedness, metres from the recenter point) to the game world, as the core placed the
        // eyes this frame; and back.
        internal Vector3 OriginPos => Xr.Position(Out.origin);
        internal Quaternion OriginYaw => Xr.Rotation(Out.origin);
        Quaternion Body => new Quaternion(-Out.bodyX, -Out.bodyY, Out.bodyZ, Out.bodyW);
        Vector3 Base => new Vector3(Out.baseX, Out.baseY, -Out.baseZ);
        Vector3 ToWorld(Vector3 tracking) => Base + Body * (Quaternion.Inverse(OriginYaw) * (tracking - OriginPos) * Out.scale);
        internal Vector3 ToTracking(Vector3 world) => OriginPos + OriginYaw * (Quaternion.Inverse(Body) * (world - Base) / Mathf.Max(1e-4f, Out.scale));
        Vector3 DirToWorld(Vector3 dir) => Body * (Quaternion.Inverse(OriginYaw) * dir);

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
            reticleShown = !windowShown && Out.aiming != 0 && Settings.Crosshair.Value != CrosshairMode.Off;
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
        internal float BodyYaw => Out.bodyYaw;

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
}
