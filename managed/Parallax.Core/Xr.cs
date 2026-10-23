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
            float l = Mathf.Tan(fov.left) * near, r = Mathf.Tan(fov.right) * near;
            float b = Mathf.Tan(fov.down) * near, t = Mathf.Tan(fov.up) * near;
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
    }
}
