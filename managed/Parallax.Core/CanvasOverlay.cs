using System.Collections.Generic;
using UnityEngine;

namespace Parallax
{
    // A "Screen Space - Camera" canvas is drawn only by the camera it names. Bound to the head camera (or to one whose
    // world the eyes took over), it would vanish: that camera draws nothing now and the eyes are other cameras
    // (Hollow Knight's menu). Such canvases become screen overlays while the eyes render, so they reach the UI panel,
    // and get their camera back afterwards. Checked twice a second, as games bind canvases on scene loads.
    internal sealed class CanvasOverlay
    {
        readonly Dictionary<int, Changed> changed = new Dictionary<int, Changed>();

        struct Changed
        {
            public Canvas Canvas;
            public Camera Camera;
            public int Order;
        }

        // A camera's canvas drew with that camera, under every overlay canvas; as an overlay it would tie with them on
        // sorting order and could cover a menu (Outer Wilds' title backdrop over its menu items).
        const int Below = 10000;
        readonly Optional access = new Optional("Canvas overlay switch");
        float nextScan;

        public void Apply(System.Func<Camera, bool> hidden)
        {
            if (Time.unscaledTime < nextScan) return;
            nextScan = Time.unscaledTime + 0.5f;
            access.Run(() =>
            {
                foreach (var canvas in Find.All<Canvas>())
                {
                    if (!canvas.isRootCanvas || canvas.renderMode != RenderMode.ScreenSpaceCamera) continue;
                    var cam = canvas.worldCamera;
                    if (cam == null || !hidden(cam)) continue;
                    changed[canvas.GetInstanceID()] = new Changed { Canvas = canvas, Camera = cam, Order = canvas.sortingOrder };
                    canvas.renderMode = RenderMode.ScreenSpaceOverlay;
                    canvas.sortingOrder = Mathf.Max(short.MinValue, canvas.sortingOrder - Below);
                }
            });
        }

        public void Restore()
        {
            if (changed.Count == 0) return;
            access.Run(() =>
            {
                foreach (var c in changed.Values)
                {
                    if (c.Canvas == null || c.Canvas.renderMode != RenderMode.ScreenSpaceOverlay) continue;
                    c.Canvas.renderMode = RenderMode.ScreenSpaceCamera;
                    c.Canvas.worldCamera = c.Camera;
                    c.Canvas.sortingOrder = c.Order;
                }
            });
            changed.Clear();
            nextScan = 0;
        }
    }
}
