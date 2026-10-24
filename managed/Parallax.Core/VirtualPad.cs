using System;
using System.Linq;
using System.Reflection;
using UnityEngine;

namespace Parallax
{
    // Games on Unity's Input System package do not read the hooked XInput functions (2021+ players go through
    // Windows.Gaming.Input), so Gamepad mode also adds a virtual Input System gamepad fed with the same state.
    // Reflection only: the package is optional and its version differs per game.
    internal static class VirtualPad
    {
        // XInput button bit -> GamepadButton bit (DpadUp 0 .. North 4, East 5, South 6, West 7 .. Start 12, Select 13).
        static readonly int[,] ButtonMap =
        {
            { 0x0001, 0 }, { 0x0002, 1 }, { 0x0004, 2 }, { 0x0008, 3 }, { 0x0010, 12 }, { 0x0020, 13 },
            { 0x0040, 8 }, { 0x0080, 9 }, { 0x0100, 10 }, { 0x0200, 11 },
            { 0x1000, 6 }, { 0x2000, 5 }, { 0x4000, 7 }, { 0x8000, 4 },
        };

        static bool unavailable;
        static object device;
        static Type stateType;
        static MethodInfo queue, remove;
        static FieldInfo buttons, leftStick, rightStick, leftTrigger, rightTrigger;

        public static void Update()
        {
            if (unavailable) return;
            bool active = Native.prx_pad_state(out var pad) != 0;
            if (!active)
            {
                if (device != null) remove.Invoke(null, new[] { device });
                device = null;
                return;
            }
            try
            {
                if (device == null && !Create()) return;
                var state = Activator.CreateInstance(stateType);
                uint bits = 0;
                for (int i = 0; i < ButtonMap.GetLength(0); i++)
                    if ((pad.buttons & ButtonMap[i, 0]) != 0) bits |= 1u << ButtonMap[i, 1];
                buttons.SetValue(state, bits);
                leftStick.SetValue(state, new Vector2(pad.lx / 32767f, pad.ly / 32767f));
                rightStick.SetValue(state, new Vector2(pad.rx / 32767f, pad.ry / 32767f));
                leftTrigger.SetValue(state, pad.leftTrigger / 255f);
                rightTrigger.SetValue(state, pad.rightTrigger / 255f);
                queue.Invoke(null, new[] { device, state, -1.0 });
            }
            catch (Exception ex)
            {
                unavailable = true;
                Plugin.Log.LogWarning("Input System gamepad unavailable: " + (ex.InnerException ?? ex).Message);
            }
        }

        static bool Create()
        {
            var input = Type.GetType("UnityEngine.InputSystem.InputSystem, Unity.InputSystem");
            stateType = Type.GetType("UnityEngine.InputSystem.LowLevel.GamepadState, Unity.InputSystem");
            if (input == null || stateType == null)
            {
                unavailable = true;  // the game does not use the Input System
                return false;
            }
            var methods = input.GetMethods(BindingFlags.Public | BindingFlags.Static);
            var add = methods.First(m => m.Name == "AddDevice" && !m.IsGenericMethod && m.GetParameters().Length == 3 &&
                                         m.GetParameters()[0].ParameterType == typeof(string));
            remove = methods.First(m => m.Name == "RemoveDevice" && m.GetParameters().Length == 1);
            queue = methods.First(m => m.Name == "QueueStateEvent" && m.IsGenericMethodDefinition).MakeGenericMethod(stateType);
            buttons = stateType.GetField("buttons");
            leftStick = stateType.GetField("leftStick");
            rightStick = stateType.GetField("rightStick");
            leftTrigger = stateType.GetField("leftTrigger");
            rightTrigger = stateType.GetField("rightTrigger");
            device = add.Invoke(null, new object[] { "Gamepad", "ParallaxPad", null });
            Plugin.Log.LogInfo("Input System gamepad added");
            return true;
        }
    }
}
