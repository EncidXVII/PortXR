using System;
using System.Collections;
using UnityEngine;
using UnityEngine.SceneManagement;

namespace Parallax
{
    internal class Rig : MonoBehaviour
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
        CameraClearFlags savedClear;
        Color savedBackground;
        bool camHidden;
        bool eyesRendered;
        Camera extrasCamera;
        bool beforeRenderHooked;
        int sceneHandle;

        Vector3 originPos;
        Quaternion originYaw = Quaternion.identity;
        bool recenterPending = true;

        Controllers controllers;
        internal bool PointerActive;
        Vector2 pointerUv;
        bool menuKeyDown;
        bool failureHandled, restarting;
        float retryAt, retryDelay = 5f;
        int submitErrors;
        bool forcedTheater;

        internal Menu Menu;
        internal TestChannel Test;

        // ScreenCapture.CaptureScreenshotIntoRenderTexture exists from Unity 2019.1; older engines use the native grab.
        static readonly Action<RenderTexture> captureIntoTexture = FindCapture();
        readonly Optional managedCapture = new Optional("Screen capture into a texture"), mirror = new Optional("Desktop mirror"),
            recreate = new Optional("Eye target recreation"), resolve = new Optional("MSAA resolve"),
            menuDrawing = new Optional("VR menu drawing"), pointerDot = new Optional("Pointer dot");

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
            float scale = Mathf.Clamp(Settings.RenderScale.Value, 0.25f, 2f);
            w = Mathf.RoundToInt(w * scale);
            h = Mathf.RoundToInt(h * scale);
            // URP sizes its intermediate buffer from the camera's target, so an 8-bit eye target would clamp HDR and
            // starve bloom. Linear-space games get half-float eyes (linear values); gamma-space games stay 8-bit.
            var format = QualitySettings.activeColorSpace == ColorSpace.Linear ? RenderTextureFormat.ARGBHalf : RenderTextureFormat.ARGB32;
            for (int e = 0; e < 2; e++)
            {
                eyeTargets[e] = new RenderTexture(w, h, 24, format, RenderTextureReadWrite.Default)
                {
                    name = "Parallax Eye " + e,
                    // The D3D12 path reads the camera's own target, which must not be multisampled.
                    antiAliasing = D3D12 ? 1 : Mathf.Max(1, QualitySettings.antiAliasing),
                };
                eyeTargets[e].Create();
                eyePtrs[e] = eyeTargets[e].GetNativeTexturePtr();
                var go = new GameObject("Parallax Eye " + e);
                DontDestroyOnLoad(go);
                Eyes[e] = go.AddComponent<Camera>();
                Eyes[e].enabled = false;
            }
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
            Plugin.Log.LogInfo($"Eye targets {w}x{h} {format}, MSAA {eyeTargets[0].antiAliasing}");
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
            // Polled rather than SceneManager.sceneLoaded, whose accessor IL2CPP games often strip.
            int scene = SceneManager.GetActiveScene().handle;
            if (scene != sceneHandle)
            {
                sceneHandle = scene;
                ReleaseCamera();
            }

            if (MenuKeyPressed()) Menu.Toggle();
            Menu.Update();
            UpdatePointer();
            var cfg = new InputConfig
            {
                mode = (int)Settings.Input.Value,
                neutral = Menu.Open ? 1 : 0,
                pointerActive = PointerActive ? 1 : 0,
                pointerX = Mathf.RoundToInt(pointerUv.x * Screen.width),
                pointerY = Mathf.RoundToInt(pointerUv.y * Screen.height),
                mouseSpeed = Settings.MouseSpeed.Value,
            };
            uint events = Native.prx_input_update(ref cfg, Test?.ControllerOverride, out controllers);
            if ((events & Native.EvMenuToggle) != 0) Menu.Toggle();
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
            var cam = located && !Settings.Theater.Value && !forcedTheater ? EnsureCamera() : null;
            if (cam == null)
            {
                SetEyes(false);
                RestoreCamera();
                return;
            }
            HideGameCamera();
            // Pipeline settings rarely change; a refresh every second catches in-game graphics option changes.
            bool refreshExtras = extrasCamera != cam || Time.frameCount % 60 == 0;
            extrasCamera = cam;
            var camT = cam.transform;
            var yaw = Xr.Yaw(camT.rotation);
            var unYaw = Quaternion.Inverse(originYaw);
            float scale = Settings.WorldScale.Value;
            var lift = Vector3.up * Settings.Height.Value;
            for (int e = 0; e < 2; e++)
            {
                var eye = Eyes[e];
                eye.CopyFrom(cam);
                eye.cullingMask = savedMask;
                eye.clearFlags = savedClear;
                eye.backgroundColor = savedBackground;
                eye.targetTexture = eyeTargets[e];
                eye.rect = new Rect(0, 0, 1, 1);
#if !IL2CPP
                if (refreshExtras) CameraExtras.Copy(cam, eye);
#endif
                var rel = unYaw * (Xr.Position(Views[e]) - originPos);
                eye.transform.SetPositionAndRotation(camT.position + yaw * (rel * scale) + lift,
                    yaw * unYaw * Xr.Rotation(Views[e]));
                eye.projectionMatrix = Xr.Projection(Fovs[e], eye.nearClipPlane, eye.farClipPlane);
                eye.enabled = true;
            }
            eyesRendered = true;
        }

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
            // The game camera drew nothing, so the backbuffer now holds only overlay UI and IMGUI.
            bool native = D3D12 || captureIntoTexture == null || !managedCapture.Run(() =>
            {
                EnsureUiTarget();
                captureIntoTexture(uiTarget);
            });
            if (native && !D3D12) GL.IssuePluginEvent(renderEvent, Native.EvCaptureBackbuffer);
            bool stereo = eyesRendered;
            var f = new Frame { stereo = stereo ? 1 : 0, quad = 1, quadFromBackbuffer = native ? 1 : 0, quadAlpha = stereo ? 1 : 0 };
            if (stereo)
            {
                // A managed blit would land in the D3D12 UI grab; there, and where Graphics.Blit is stripped, Present draws it.
                f.mirror = D3D12 || !mirror.Run(() => Mirror(native)) ? 1 : 0;
                for (int e = 0; e < 2; e++)
                {
                    var rt = eyeTargets[e];
                    recreate.Run(() =>
                    {
                        if (rt.IsCreated()) return;
                        rt.Create();
                        eyePtrs[e] = rt.GetNativeTexturePtr();
                    });
                    // Unity resolves a camera's MSAA target itself; the explicit resolve is a safety net.
                    if (rt.antiAliasing > 1) resolve.Run(rt.ResolveAntiAliasedSurface);
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
            f.quadPose = Xr.ToPose(center, rot);
            Native.prx_set_frame(slot, ref f);
            GL.IssuePluginEvent(renderEvent, Native.EvSubmit | (slot << 8));
            slot = (slot + 1) & 3;
        }

        void Mirror(bool nativeCapture)
        {
            var eye = eyeTargets[0];
            float visible = Mathf.Min(1f, (float)eye.width / eye.height / ((float)Screen.width / Screen.height));
            Graphics.Blit(eye, (RenderTexture)null, new Vector2(1, visible), new Vector2(0, (1 - visible) / 2));
            if (nativeCapture) return;  // the UI only reaches the headset on this path
            RenderTexture.active = null;
            GL.PushMatrix();
            GL.LoadPixelMatrix(0, Screen.width, Screen.height, 0);
            Graphics.DrawTexture(new Rect(0, 0, Screen.width, Screen.height), uiTarget, new Rect(0, 1, 1, -1), 0, 0, 0, 0);
            GL.PopMatrix();
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

        void Placement(bool stereo, out Vector3 center, out Quaternion rot, out float width, out float height)
        {
            float dist = stereo ? Settings.UiDistance.Value : TheaterDistance;
            width = stereo ? Settings.UiWidth.Value : TheaterWidth;
            height = width * Screen.height / Mathf.Max(1, Screen.width);
            rot = originYaw;
            center = originPos + originYaw * new Vector3(0, stereo ? -UiDrop : 0, dist);
        }

        void UpdatePointer()
        {
            bool onQuad = false;
            Placement(!Settings.Theater.Value, out var c, out var r, out float w, out float h);
            if (Test?.PointerOverride is Vector2 forced)
            {
                pointerUv = forced;
                onQuad = true;
            }
            else if (controllers.aimValidR != 0)
            {
                var p = Xr.Position(controllers.aimR);
                var d = Xr.Rotation(controllers.aimR) * Vector3.forward;
                var n = r * Vector3.forward;
                float denom = Vector3.Dot(d, n);
                if (denom > 1e-4f)
                {
                    float t = Vector3.Dot(c - p, n) / denom;
                    var local = Quaternion.Inverse(r) * (p + d * t - c);
                    var uv = new Vector2(local.x / w + 0.5f, 0.5f - local.y / h);
                    onQuad = t > 0 && uv.x >= 0 && uv.x <= 1 && uv.y >= 0 && uv.y <= 1;
                    if (onQuad) pointerUv = uv;
                }
            }
            bool wanted = Menu.Open || (Settings.Pointer.Value == PointerMode.Auto && Cursor.visible && Cursor.lockState != CursorLockMode.Locked);
            bool active = onQuad && wanted;
            if (active && !PointerActive) Native.prx_haptic_pulse(1, 0.25f, 0.02f);
            PointerActive = active;
        }

        void OnGUI()
        {
            GUI.depth = -1000;
            menuDrawing.Run(Menu.Draw);
            if (PointerActive && Event.current.type == EventType.Repaint) pointerDot.Run(DrawPointer);
        }

        void DrawPointer()
        {
            {
                // Built-in white texture tinted: IL2CPP games often strip the Texture2D constructor.
                GUI.matrix = Matrix4x4.identity;
                float x = pointerUv.x * Screen.width, y = pointerUv.y * Screen.height, s = Screen.height / 60f;
                var tint = GUI.color;
                GUI.color = new Color(0.1f, 0.1f, 0.1f, 0.8f);
                GUI.DrawTexture(new Rect(x - s / 2, y - s / 2, s, s), Texture2D.whiteTexture);
                GUI.color = Color.white;
                GUI.DrawTexture(new Rect(x - s / 4, y - s / 4, s / 2, s / 2), Texture2D.whiteTexture);
                GUI.color = tint;
            }
        }

        internal void RequestRecenter() => recenterPending = true;

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
            if (GameCam != null && GameCam.isActiveAndEnabled) return GameCam;
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
                if (IsEye(cam) || cam.targetTexture != null) continue;
                if (wanted.Length > 0 && cam.name == wanted) return cam;
                int score = BitCount(cam.cullingMask) - (cam.orthographic ? 50 : 0) + (cam.CompareTag("MainCamera") ? 100 : 0);
                if (score > bestScore)
                {
                    best = cam;
                    bestScore = score;
                }
            }
            return best;
        }

        internal void ReleaseCamera()
        {
            RestoreCamera();
            GameCam = null;
        }

        // Re-read before hiding: a value that is not ours means the game changed it since last frame.
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

        void RestoreCamera()
        {
            if (camHidden && GameCam != null)
            {
                GameCam.cullingMask = savedMask;
                GameCam.clearFlags = savedClear;
                GameCam.backgroundColor = savedBackground;
            }
            camHidden = false;
        }

        void SetEyes(bool on)
        {
            foreach (var eye in Eyes)
                if (eye != null) eye.enabled = on;
        }

        internal bool IsEye(Camera cam) => cam == Eyes[0] || cam == Eyes[1];

        static int BitCount(int v)
        {
            int n = 0;
            for (uint u = (uint)v; u != 0; u &= u - 1) n++;
            return n;
        }

        bool MenuKeyPressed()
        {
            bool down = (Native.GetAsyncKeyState(Settings.MenuKey.Value) & 0x8000) != 0 && OwnsForeground();
            bool pressed = down && !menuKeyDown;
            menuKeyDown = down;
            return pressed;
        }

        static readonly int ProcessId = System.Diagnostics.Process.GetCurrentProcess().Id;

        static bool OwnsForeground()
        {
            Native.GetWindowThreadProcessId(Native.GetForegroundWindow(), out int pid);
            return pid == ProcessId;
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
}
