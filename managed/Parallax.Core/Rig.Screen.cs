using UnityEngine;

namespace Parallax
{
    // The theater and Parallax screen, the UI panel, and the laser pointer that works them.
    internal partial class Rig
    {
        void Placement(bool stereo, out Vector3 center, out Quaternion rot, out float width, out float height)
        {
            if (stereo)
            {
                width = Settings.UiWidth.Value;
                rot = originYaw;
                center = originPos + originYaw * new Vector3(0, -UiDrop, Settings.UiDistance.Value);
            }
            else
            {
                ScreenFromSettings();
                width = screenWidth;
                // Turned to face the recenter point, so the Parallax window's views stay centred on the player.
                rot = originYaw * Quaternion.Euler(CurveFits ? 0 : -screenPitch, screenYaw, 0);
                center = CurveFits ? CurveCenter + rot * Vector3.forward * CurveRadius : originPos + rot * Vector3.forward * screenDistance;
            }
            height = width * Screen.height / Mathf.Max(1, Screen.width);
        }

        // The theater and Parallax screen: a direction from the recenter point, a distance and a width, kept per game.
        float screenYaw, screenPitch, screenDistance = TheaterDistance, screenWidth = TheaterWidth;
        bool screenLive;  // being adjusted: these fields lead, the settings follow when a drag or stick move ends
        const float MinScreen = 0.8f, MaxScreen = 20f;

        internal Vector4 ScreenState => new Vector4(screenYaw, screenPitch, screenDistance, screenWidth);

        float CurveRadius => screenDistance * Xr.Cos(screenPitch * Mathf.Deg2Rad);
        Vector3 CurveCenter => originPos + Vector3.up * (screenDistance * Xr.Sin(screenPitch * Mathf.Deg2Rad));

        void ScreenFromSettings()
        {
            if (screenLive) return;
            screenYaw = Settings.ScreenYaw.Value;
            screenPitch = Mathf.Clamp(Settings.ScreenPitch.Value, -60f, 75f);
            screenDistance = Mathf.Clamp(Settings.ScreenDistance.Value, MinScreen, MaxScreen);
            screenWidth = Mathf.Clamp(Settings.ScreenWidth.Value, MinScreen, MaxScreen);
        }

        void SaveScreen()
        {
            Settings.ScreenYaw.Value = Mathf.Round(screenYaw * 10) / 10;
            Settings.ScreenPitch.Value = Mathf.Round(screenPitch * 10) / 10;
            Settings.ScreenDistance.Value = Mathf.Round(screenDistance * 100) / 100;
            Settings.ScreenWidth.Value = Mathf.Round(screenWidth * 100) / 100;
        }

        internal void ResetScreen()
        {
            foreach (var e in new[] { Settings.ScreenYaw, Settings.ScreenPitch, Settings.ScreenDistance, Settings.ScreenWidth }) e.BoxedValue = e.DefaultValue;
            screenLive = false;
            ScreenFromSettings();
        }

        // Screen handles (Theater and Parallax), shown while the VR menu is open:
        // a bar under the middle of the screen drags it anywhere around the player, the corners resize it, and RESET
        // beside the bar brings back 3 m ahead. While a handle is dragged the right stick pushes the screen away or pulls
        // it in (up, down) and sizes it (right, left). Game input pauses over a handle; each game keeps its screen.
        int handle, adjustGrab;  // handle under the laser: 1 bar, 2 corner, 3 reset; adjustGrab: 1 moving, 2 resizing
        bool handlesShown, handlePull;
        Quaternion grabFrom;
        float grabWidth, grabReach;
        readonly Optional handleDraw = new Optional("Screen handles");
        GUIStyle handleText;

        internal int Handle => handle;
        internal int Grab => adjustGrab;
        bool OnHandles => handle != 0 || adjustGrab != 0;
        internal bool WatchShown => watchShown;
        internal bool Glancing => glancing;

        static Rect Bar => new Rect(Screen.width / 2f - Screen.height * 0.16f, Screen.height * 0.935f, Screen.height * 0.32f, Screen.height * 0.065f);
        static Rect ResetRect => new Rect(Bar.xMax + Screen.height * 0.02f, Bar.y, Screen.height * 0.16f, Bar.height);
        static float CornerSize => Screen.height * 0.12f;

        static int HandleAt(Vector2 uv)
        {
            var p = new Vector2(uv.x * Screen.width, uv.y * Screen.height);
            if (Bar.Contains(p)) return 1;
            if (ResetRect.Contains(p)) return 3;
            float c = CornerSize;
            return (p.x < c || p.x > Screen.width - c) && (p.y < c || p.y > Screen.height - c) ? 2 : 0;
        }

        // Takes the laser while it is on a handle or dragging one; false leaves it to the menu and the game.
        bool UpdateHandles(bool shown, bool screen, bool onQuad, Vector2 uv, Vector3 c, Quaternion r, float w, float h)
        {
            bool pull = controllers.rTrigger > 0.6f;
            bool fresh = pull && !handlePull;
            handlePull = pull;
            handlesShown = shown || (screen && adjustGrab != 0);
            if (!handlesShown || controllers.aimValidR == 0)
            {
                if (adjustGrab != 0) SaveScreen();
                handle = adjustGrab = 0;
                screenLive = false;
                return false;
            }
            int at = onQuad ? HandleAt(uv) : 0;
            if (adjustGrab == 0)
            {
                if (at != handle && at != 0) Native.prx_haptic_pulse(1, 0.2f, 0.015f);
                handle = at;
            }
            if (handle == 0) return false;

            PointerActive = false;
            beamOn = true;
            beamFrom = Xr.Position(controllers.aimR);
            var dir = Xr.Rotation(controllers.aimR) * Vector3.forward;
            if (fresh && adjustGrab == 0)
            {
                Native.prx_haptic_pulse(1, 0.3f, 0.03f);
                if (handle == 3) ResetScreen();
                else
                {
                    adjustGrab = handle;
                    screenLive = true;
                    // Moving keeps the screen where it sat relative to the ray; resizing scales with the corner's reach.
                    grabFrom = Quaternion.Inverse(Quaternion.LookRotation(dir)) * (originYaw * Quaternion.Euler(-screenPitch, screenYaw, 0));
                    grabWidth = screenWidth;
                    grabReach = Reach(beamFrom, dir, c, r);
                }
            }
            else if (!pull && adjustGrab != 0)
            {
                adjustGrab = 0;
                screenLive = false;
                SaveScreen();
            }
            if (adjustGrab != 0)
            {
                float dt = Time.unscaledDeltaTime;
                if (Mathf.Abs(controllers.ry) > 0.2f) screenDistance = Mathf.Clamp(screenDistance * (float)System.Math.Exp(controllers.ry * dt * 1.5f), MinScreen, MaxScreen);
                if (Mathf.Abs(controllers.rx) > 0.2f) grabWidth = Mathf.Clamp(grabWidth * (float)System.Math.Exp(controllers.rx * dt * 1.5f), MinScreen, MaxScreen);
                if (adjustGrab == 1)
                {
                    var facing = Quaternion.Inverse(originYaw) * (Quaternion.LookRotation(dir) * grabFrom) * Vector3.forward;
                    screenYaw = Xr.Atan2(facing.x, facing.z) * Mathf.Rad2Deg;
                    screenPitch = Mathf.Clamp(Xr.Asin(Mathf.Clamp(facing.y, -1f, 1f)) * Mathf.Rad2Deg, -60f, 75f);
                    screenWidth = grabWidth;
                }
                else if (grabReach > 0.05f)
                {
                    float reach = Reach(beamFrom, dir, c, r);
                    if (reach > 0) screenWidth = Mathf.Clamp(grabWidth * reach / grabReach, MinScreen, MaxScreen);
                }
            }
            beamTo = onQuad ? PanelPoint(uv, c, r, w, h) : beamFrom + dir;
            return true;
        }

        // How far from the screen's centre the ray crosses the screen's plane; -1 when it does not.
        static float Reach(Vector3 from, Vector3 dir, Vector3 center, Quaternion rot)
        {
            var inv = Quaternion.Inverse(rot);
            Vector3 p = inv * (from - center), d = inv * dir;
            if (d.z < 1e-4f) return -1;
            var hit = p + d * (-p.z / d.z);
            return new Vector2(hit.x, hit.y).magnitude;
        }

        // Faint white marks over the game, lime under the laser: corner brackets, the bar, RESET; the stick's controls
        // and the size show above the bar while dragging. Drawn as the menu draws (GUI.Label and rich text are stripped
        // from some IL2CPP games).
        void DrawHandles()
        {
            GUI.matrix = Matrix4x4.identity;
            float W = Screen.width, H = Screen.height, t = Mathf.Max(2, H / 200), arm = CornerSize * 0.6f, inset = H * 0.02f;
            var faint = new Color(1f, 1f, 1f, 0.45f);
            var shade = new Color(0f, 0f, 0f, 0.35f);
            bool sizing = handle == 2 || adjustGrab == 2;
            float k = sizing ? 2 : 1;
            foreach (var x in new[] { inset, W - inset })
                foreach (var y in new[] { inset, H - inset })
                {
                    Menu.Fill(new Rect(x < W / 2 ? x : x - arm, y < H / 2 ? y : y - t * k, arm, t * k), sizing ? Menu.Lime : faint);
                    Menu.Fill(new Rect(x < W / 2 ? x : x - t * k, y < H / 2 ? y : y - arm, t * k, arm), sizing ? Menu.Lime : faint);
                }
            var bar = Bar;
            Menu.Fill(bar, shade);
            Menu.Fill(new Rect(bar.x + bar.width * 0.2f, bar.center.y - t * 1.5f, bar.width * 0.6f, t * 3), handle == 1 || adjustGrab == 1 ? Menu.Lime : faint);
            int size = Mathf.RoundToInt(H / 40);
            if (handleText == null || handleText.fontSize != size) handleText = Menu.Style(TextAnchor.MiddleCenter, Color.white, size, null, false);
            var reset = ResetRect;
            Menu.Fill(reset, handle == 3 ? Menu.Lime : shade);
            handleText.normal.textColor = handle == 3 ? Color.black : faint;
            handleText.Draw(reset, new GUIContent("RESET"), false, false, false, false);
            if (adjustGrab == 0) return;
            var tip = new Rect(W / 2 - H * 0.5f, bar.y - H * 0.1f, H, H * 0.08f);
            Menu.Fill(tip, new Color(0.04f, 0.04f, 0.04f, 0.8f));
            handleText.normal.textColor = Color.white;
            handleText.Draw(new Rect(tip.x, tip.y, tip.width, tip.height / 2), new GUIContent($"{screenDistance:0.0} m away, {screenWidth:0.0} m wide"), false, false, false, false);
            handleText.Draw(new Rect(tip.x, tip.center.y, tip.width, tip.height / 2), new GUIContent("Right stick: up/down distance, left/right size"), false, false, false, false);
        }

        void UpdatePointer()
        {
            bool onQuad = false;
            Placement(!theaterShown, out var c, out var r, out float w, out float h);
            if (Test?.AimOffset is Vector3 offset)
            {
                var from = originPos + originYaw * offset;
                var to = Test.AimTarget is Vector3 t ? originPos + originYaw * t : c;
                var target = Test.AimObject != null ? GameObject.Find(Test.AimObject) : null;
                if (target != null && GameCam != null)
                {
                    // Game world back to tracking space: the inverse of how the eyes are placed.
                    var cam = GameCam.transform;
                    var rel = target.transform.position - cam.position - Vector3.up * Settings.Height.Value;
                    to = originPos + originYaw * (Quaternion.Inverse(Body) * rel / Scale);
                }
                controllers.aimR = Xr.ToPose(from, Quaternion.LookRotation(to - from));
                controllers.aimValidR = 1;
            }
            var uv = new Vector2(-1, -1);
            if (Test?.PointerOverride is Vector2 forced)
            {
                pointerUv = forced;
                onQuad = true;
            }
            else if (controllers.aimValidR != 0)
            {
                var inv = Quaternion.Inverse(r);
                var p = Xr.Position(controllers.aimR);
                var d = inv * (Xr.Rotation(controllers.aimR) * Vector3.forward);
                uv = CurvedShown ? HitCylinder(inv * (p - CurveCenter), d, w, h, CurveRadius) : HitPlane(inv * (p - c), d, w, h);
                onQuad = uv.x >= 0 && uv.x <= 1 && uv.y >= 0 && uv.y <= 1;
                // Hand tremor would shake the game's cursor (and the real mouse with it): small moves are eased, large
                // ones followed at once, and a trigger being pulled (which tips the controller down) holds it steadier.
                if (onQuad && PointerActive)
                {
                    float follow = Mathf.Clamp((uv - pointerUv).magnitude * 60f, controllers.rTrigger > 0.15f ? 0.04f : 0.15f, 1f);
                    pointerUv = Vector2.Lerp(pointerUv, uv, follow);
                }
                else if (onQuad) pointerUv = uv;
            }
            bool wanted = Menu.Open || (Settings.Pointer.Value == PointerMode.Auto && Cursor.visible && Cursor.lockState != CursorLockMode.Locked);
            bool screen = theaterShown && Settings.Display.Value != DisplayMode.Stereo && !Hider.Active && Test?.PointerOverride == null;
            // Only with the menu open: game buttons often sit in the corners or at the bottom middle.
            if (UpdateHandles(screen && Menu.Open, screen, onQuad, uv, c, r, w, h)) return;
            bool active = onQuad && wanted;
            if (Hider.Active)
            {
                UpdateHider(active);
                return;
            }
            beamOn = wanted && controllers.aimValidR != 0 && Test?.PointerOverride == null;
            if (beamOn)
            {
                beamFrom = Xr.Position(controllers.aimR);
                beamTo = active ? PanelPoint(pointerUv, c, r, w, h) : beamFrom + Xr.Rotation(controllers.aimR) * Vector3.forward;
            }
            if (active && !PointerActive) Native.prx_haptic_pulse(1, 0.25f, 0.02f);
            PointerActive = active;
        }

        // Hide-object tool: the beam goes out into the game world and ends on the object it would hide. The menu
        // button (or opening the menu) ends the tool; in the game world, so the panel pointer stays off meanwhile.
        void UpdateHider(bool overPanel)
        {
            PointerActive = false;
            var cam = GameCam;
            beamOn = cam != null && controllers.aimValidR != 0;
            if (!beamOn || Menu.Open)
            {
                if (Menu.Open) Hider.Stop();
                return;
            }
            float scale = Scale;
            var yaw = Body;
            var toWorld = yaw * Quaternion.Inverse(originYaw);
            var lift = Vector3.up * Settings.Height.Value;
            beamFrom = Xr.Position(controllers.aimR);
            var dir = Xr.Rotation(controllers.aimR) * Vector3.forward;
            var origin = cam.transform.position + toWorld * ((beamFrom - originPos) * scale) + lift;
            Hider.Aim(new Ray(origin, toWorld * dir), controllers.rTrigger > 0.6f, (controllers.buttons & Buttons.B) != 0);
            float reach = Hider.Target != null ? Hider.TargetDistance / scale : 3f;
            beamTo = beamFrom + dir * Mathf.Min(reach, 20f);
        }

        // Panel UV back to tracking space: on the flat panel, or on the curved theater screen around the origin.
        Vector3 PanelPoint(Vector2 uv, Vector3 center, Quaternion rot, float w, float h)
        {
            if (!CurvedShown) return center + rot * new Vector3((uv.x - 0.5f) * w, (0.5f - uv.y) * h, 0);
            float R = CurveRadius, a = (uv.x - 0.5f) * w / R;
            return CurveCenter + rot * new Vector3(Xr.Sin(a) * R, (0.5f - uv.y) * h, Xr.Cos(a) * R);
        }

        // Ray (in the panel's frame, panel centre at the origin, facing -Z) to panel UV; (-1, -1) for a miss.
        static Vector2 HitPlane(Vector3 p, Vector3 d, float w, float h)
        {
            if (d.z < 1e-4f) return new Vector2(-1, -1);
            float t = -p.z / d.z;
            var hit = p + d * t;
            return t > 0 ? new Vector2(hit.x / w + 0.5f, 0.5f - hit.y / h) : new Vector2(-1, -1);
        }

        // Ray (in the screen's frame, cylinder axis vertical through the origin) to UV on the curved theater screen,
        // whose arc length is w. The player stands inside, so the hit is where the ray leaves the cylinder.
        static Vector2 HitCylinder(Vector3 p, Vector3 d, float w, float h, float radius)
        {
            float a = d.x * d.x + d.z * d.z, b = 2 * (p.x * d.x + p.z * d.z), c = p.x * p.x + p.z * p.z - radius * radius;
            float disc = b * b - 4 * a * c;
            if (a < 1e-6f || disc < 0) return new Vector2(-1, -1);
            float t = (-b + Xr.Sqrt(disc)) / (2 * a);
            var hit = p + d * t;
            return t > 0 ? new Vector2(Xr.Atan2(hit.x, hit.z) * radius / w + 0.5f, 0.5f - hit.y / h) : new Vector2(-1, -1);
        }

        void OnGUI()
        {
            GUI.depth = -1000;
            menuDrawing.Run(Menu.Draw);
            if (PointerActive && Event.current.type == EventType.Repaint) pointerDot.Run(DrawPointer);
            if (handlesShown && Event.current.type == EventType.Repaint) handleDraw.Run(DrawHandles);
        }

        void DrawPointer()
        {
            GUI.matrix = Matrix4x4.identity;
            float x = pointerUv.x * Screen.width, y = pointerUv.y * Screen.height, s = Screen.height / 60f;
            Menu.Fill(new Rect(x - s / 2, y - s / 2, s, s), new Color(0.1f, 0.1f, 0.1f, 0.8f));
            Menu.Fill(new Rect(x - s / 4, y - s / 4, s / 2, s / 2), Color.white);
        }
    }
}
