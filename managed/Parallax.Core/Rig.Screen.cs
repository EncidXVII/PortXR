using UnityEngine;

namespace Parallax
{
    // The UI panel, the Theater and Parallax screen, its handles and the laser pointer that works them are the core's
    // (native/src/rig.cpp); here, what tests and diagnostics read of them, and the pointer's dot on the game's UI.
    internal partial class Rig
    {
        internal Vector4 ScreenState => new Vector4(Out.screenYaw, Out.screenPitch, Out.screenDistance, Out.screenWidth);
        internal int Handle => Out.handle;
        internal int Grab => Out.grab;
        internal bool WatchShown => Out.watch != 0;
        internal bool Glancing => Out.glancing != 0;

        // The middle of the UI panel (Stereo) or the screen, in the play space.
        internal Vector3 PictureCenter => Out.mode == 0
            ? OriginPos + OriginYaw * new Vector3(0, -0.15f, Settings.UiDistance.Value)
            : OriginPos + OriginYaw * Quaternion.Euler(-Out.screenPitch, Out.screenYaw, 0) * new Vector3(0, 0, Out.screenDistance);

        void OnGUI()
        {
            GUI.depth = -1000;
            if (PointerActive && Event.current.type == EventType.Repaint) pointerDot.Run(DrawPointer);
        }

        // Where the laser meets the game's UI, for games that hide the OS cursor.
        void DrawPointer()
        {
            GUI.matrix = Matrix4x4.identity;
            float x = Out.pointerU * Screen.width, y = Out.pointerV * Screen.height, s = Screen.height / 60f;
            Menu.Fill(new Rect(x - s / 2, y - s / 2, s, s), new Color(0.1f, 0.1f, 0.1f, 0.8f));
            Menu.Fill(new Rect(x - s / 4, y - s / 4, s / 2, s / 2), Color.white);
        }
    }
}
