using System.Collections.Generic;
using UnityEngine;

namespace Parallax
{
    // Some games draw full-screen effects (fog, vignettes, damage flashes) on a quad held just in front of the camera
    // and sized to its view (PEAK's "Post Fog"). The eyes see wider and sit 3 cm to each side, so they would look past
    // its edges: unfogged mountains in one eye only. While the eyes render, such quads are enlarged to cover both eyes,
    // then put back. A quad is a renderer with a four-vertex mesh within half a metre of the head camera.
    internal sealed class ScreenQuads
    {
        const float Factor = 4f;
        readonly List<Transform> quads = new List<Transform>();
        readonly List<Vector3> scales = new List<Vector3>();
        readonly Optional access = new Optional("Screen quad fitting");
        float nextScan;
        bool grown;

        public void Grow(Camera head)
        {
            Restore();
            access.Run(() =>
            {
                if (Time.unscaledTime >= nextScan) Scan(head);
                foreach (var t in quads)
                {
                    scales.Add(t != null ? t.localScale : Vector3.one);
                    if (t != null) t.localScale = t.localScale * Factor;
                }
                grown = true;
            });
        }

        public void Restore()
        {
            if (!grown) return;
            for (int i = 0; i < quads.Count; i++)
                if (quads[i] != null) quads[i].localScale = scales[i];
            scales.Clear();
            grown = false;
        }

        void Scan(Camera head)
        {
            nextScan = Time.unscaledTime + 0.5f;
            quads.Clear();
            var at = head.transform.position;
            foreach (var r in Object.FindObjectsOfType<MeshRenderer>())
            {
                // Distance to the bounds by hand: IL2CPP games often strip Bounds.SqrDistance.
                var b = r.bounds;
                var d = Vector3.Max(Vector3.zero, Vector3.Max(b.min - at, at - b.max));
                if (!r.enabled || d.sqrMagnitude > 0.25f) continue;
                var filter = r.GetComponent<MeshFilter>();
                var mesh = filter != null ? filter.sharedMesh : null;
                if (mesh != null && mesh.vertexCount == 4) quads.Add(r.transform);
            }
        }
    }
}
