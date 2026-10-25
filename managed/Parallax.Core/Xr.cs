using UnityEngine;

namespace Parallax
{
    // OpenXR is right-handed (-Z forward), Unity left-handed (+Z forward): mirror Z.
    internal static class Xr
    {
        public static Vector3 Position(Pose p) => new Vector3(p.px, p.py, -p.pz);
        public static Quaternion Rotation(Pose p) => new Quaternion(-p.qx, -p.qy, p.qz, p.qw);

        public static Pose ToPose(Vector3 pos, Quaternion rot) => new Pose
        {
            px = pos.x, py = pos.y, pz = -pos.z,
            qx = -rot.x, qy = -rot.y, qz = rot.z, qw = rot.w,
        };

        // Off-center OpenGL-style frustum, which is what Camera.projectionMatrix expects.
        public static Matrix4x4 Projection(Fov fov, float near, float far)
        {
            float l = Tan(fov.left) * near, r = Tan(fov.right) * near;
            float b = Tan(fov.down) * near, t = Tan(fov.up) * near;
            var m = new Matrix4x4();
            m[0, 0] = 2 * near / (r - l);
            m[0, 2] = (r + l) / (r - l);
            m[1, 1] = 2 * near / (t - b);
            m[1, 2] = (t + b) / (t - b);
            m[2, 2] = -(far + near) / (far - near);
            m[2, 3] = -2 * far * near / (far - near);
            m[3, 2] = -1;
            return m;
        }

        public static Quaternion Yaw(Quaternion q) => Quaternion.Euler(0, q.eulerAngles.y, 0);

        // Trigonometry through .NET, not Mathf: Mathf forwards to the game's own Math class, and IL2CPP games strip
        // the members they never call (Atan2 and Asin are often gone).
        public static float Sin(float a) => (float)System.Math.Sin(a);
        public static float Cos(float a) => (float)System.Math.Cos(a);
        public static float Tan(float a) => (float)System.Math.Tan(a);
        public static float Asin(float v) => (float)System.Math.Asin(v);
        public static float Atan2(float y, float x) => (float)System.Math.Atan2(y, x);
        public static float Sqrt(float v) => (float)System.Math.Sqrt(v);
    }
}
