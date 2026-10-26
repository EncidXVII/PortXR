#if !IL2CPP
using UnityEngine;

namespace Parallax
{
    // Post-processing v2 resets its camera's projection before culling (Lorn's Lure, Superliminal, Risk of Rain 2),
    // which would give both eyes a centred frustum at the game's FOV. Unity sends OnPreCull in component order, so this
    // one, added after the eye's PostProcessLayer, puts the eye's off-centre frustum back.
    internal sealed class EyeProjection : MonoBehaviour
    {
        internal Matrix4x4 Matrix;
        Camera cam;

        void OnPreCull()
        {
            if (cam == null) cam = GetComponent<Camera>();
            if (Matrix.m32 != 0) cam.projectionMatrix = Matrix;
        }
    }
}
#endif
