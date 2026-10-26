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
        readonly Dictionary<int, KeyValuePair<Canvas, Camera>> changed = new Dictionary<int, KeyValuePair<Canvas, Camera>>();
        readonly Optional access = new Optional("Canvas overlay switch");
        float nextScan;

        public void Apply(System.Func<Camera, bool> hidden)
        {
            if (Time.unscaledTime < nextScan) return;
            nextScan = Time.unscaledTime + 0.5f;
            access.Run(() =>
            {
                foreach (var canvas in Object.FindObjectsOfType<Canvas>())
                {
                    if (!canvas.isRootCanvas || canvas.renderMode != RenderMode.ScreenSpaceCamera) continue;
                    var cam = canvas.worldCamera;
                    if (cam == null || !hidden(cam)) continue;
                    changed[canvas.GetInstanceID()] = new KeyValuePair<Canvas, Camera>(canvas, cam);
                    canvas.renderMode = RenderMode.ScreenSpaceOverlay;
                }
            });
        }

        public void Restore()
        {
            if (changed.Count == 0) return;
            access.Run(() =>
            {
                foreach (var pair in changed.Values)
                {
                    if (pair.Key == null || pair.Key.renderMode != RenderMode.ScreenSpaceOverlay) continue;
                    pair.Key.renderMode = RenderMode.ScreenSpaceCamera;
                    pair.Key.worldCamera = pair.Value;
                }
            });
            changed.Clear();
            nextScan = 0;
        }
    }
}
