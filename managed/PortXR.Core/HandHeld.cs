// Copyright (C) 2026 EncidXVII
// SPDX-License-Identifier: GPL-3.0-only

using System.Collections.Generic;
using UnityEngine;

namespace PortXR
{
    // Hand aim already turns the game camera, and with it the weapon an FPS hangs under that camera, to where the right
    // controller points; the weapon still sits at the head. While the eyes render, the camera's children that draw
    // something (the viewmodel: arms and gun) are moved so their middle sits in the hand, then put back, so the game's
    // own logic never sees them moved. Viewmodels the game places some other way stay at the head.
    internal sealed class HandHeld
    {
        readonly List<Transform> roots = new List<Transform>();
        readonly List<Vector3> saved = new List<Vector3>();
        readonly Optional access = new Optional("Weapon in hand");
        Camera scanned;
        Vector3 middle;  // the viewmodel's middle in the camera's local space
        float nextScan;
        bool moved;

        internal int Held => moved ? roots.Count : 0;

        // Places the viewmodel's middle at `hand` (world space).
        public void Move(Camera head, Vector3 hand)
        {
            Restore();
            access.Run(() =>
            {
                if (head != scanned || Time.unscaledTime >= nextScan) Scan(head);
                if (roots.Count == 0) return;
                var shift = hand - head.transform.TransformPoint(middle);
                foreach (var t in roots)
                {
                    saved.Add(t != null ? t.position : Vector3.zero);
                    if (t != null) t.position += shift;
                }
                moved = true;
            });
        }

        public void Restore()
        {
            if (!moved) return;
            for (int i = 0; i < roots.Count; i++)
                if (roots[i] != null) roots[i].position = saved[i];
            saved.Clear();
            moved = false;
        }

        // The camera's direct children holding enabled renderers, leaving out full-screen quads held in front of it
        // (ScreenQuads), which must stay on the head.
        void Scan(Camera head)
        {
            nextScan = Time.unscaledTime + 0.5f;
            scanned = head;
            roots.Clear();
            var cam = head.transform;
            Bounds bounds = default;
            bool any = false;
            foreach (var r in Find.All<Renderer>())
            {
                if (!r.enabled || !r.gameObject.activeInHierarchy || !r.transform.IsChildOf(cam) || r.transform == cam) continue;
                var filter = r.GetComponent<MeshFilter>();
                if (filter != null && filter.sharedMesh != null && filter.sharedMesh.vertexCount == 4) continue;
                // Arm's length: sky domes and weather emitters that follow the camera are not the viewmodel.
                var b = r.bounds;
                if (b.extents.sqrMagnitude > 1f || (b.center - cam.position).sqrMagnitude > 2.25f) continue;
                var root = r.transform;
                while (root.parent != cam) root = root.parent;
                if (!roots.Contains(root)) roots.Add(root);
                if (any) bounds.Encapsulate(b);
                else bounds = b;
                any = true;
            }
            if (any) middle = cam.InverseTransformPoint(bounds.center);
        }
    }
}
