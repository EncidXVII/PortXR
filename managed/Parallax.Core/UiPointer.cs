using System;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;
using UnityEngine;

namespace Parallax
{
    // The laser pointer on the game's own UI, through its EventSystem instead of the OS mouse: Unity reads the mouse
    // through raw input, so moved cursors and posted clicks reach it only while its window is in front, and an OS click
    // with the game behind other windows lands in whatever window is there. This does what uGUI's pointer input module
    // does for a mouse (hover, press, click, drag, scroll) at the pointer's spot, whatever the window's focus.
    // Reflection only: uGUI is a separate assembly whose version differs per game (an interop proxy on IL2CPP).
    internal static class UiPointer
    {
        static bool unavailable;
        static Type systemType, dataType;
        static PropertyInfo current;
        static MethodInfo raycastAll, setSelected;
        static PropertyInfo listCount, listItem, resultObject;
        static Type listType;
        static readonly Dictionary<string, Handler> handlers = new Dictionary<string, Handler>();

        sealed class Handler
        {
            public object Function;
            public MethodInfo Execute, Hierarchy, Find;
        }

        static object system, data, list;
        static GameObject hovered, pressed, dragged;
        static bool down, dragging, eligible, taken;
        static Vector2 pressAt, lastAt;

        // screen: pixels from the bottom-left. Returns whether the game's UI has the pointer (over one of its targets, or
        // a press that began on one): then the OS mouse button stays out of it.
        public static bool Update(bool active, Vector2 screen, float trigger, float scroll)
        {
            if (unavailable) return false;
            try
            {
                return Step(active, screen, trigger, scroll);
            }
            catch (Exception ex)
            {
                unavailable = true;
                Plugin.Log.LogWarning("Pointer on the game's UI is unavailable (" + (ex.InnerException ?? ex).Message + "); the OS mouse clicks instead.");
                return false;
            }
        }

        static bool Step(bool active, Vector2 screen, float trigger, float scroll)
        {
            if (systemType == null && !Bind()) return false;
            var es = current.GetValue(null, null);
            if (!active || es == null || !Alive(es))
            {
                Release();
                return false;
            }
            if (!ReferenceEquals(es, system) && !Same(es, system))
            {
                Release();
                system = es;
                data = Activator.CreateInstance(dataType, es);
            }
            Set("position", screen);
            Set("delta", screen - lastAt);
            Set("scrollDelta", Vector2.zero);
            lastAt = screen;
            var target = Raycast();
            // Decided before a press starts, so the OS click stays off from its first frame.
            if (!down) taken = target != null;

            bool want = down ? trigger > 0.4f : trigger > 0.6f;
            if (want && !down) Press(target, screen);
            else if (want && down) Hold(target, screen);
            else if (!want && down) Up(target);
            Hover(target);

            if (target != null && Mathf.Abs(scroll) > 0.2f)
            {
                var scroller = Find("scrollHandler", target);
                if (scroller != null)
                {
                    Set("scrollDelta", new Vector2(0, scroll * Mathf.Abs(scroll) * 30f * Time.unscaledDeltaTime));
                    Execute("scrollHandler", scroller);
                }
            }
            return taken;
        }

        static bool Bind()
        {
            systemType = FindType("UnityEngine.EventSystems.EventSystem");
            dataType = FindType("UnityEngine.EventSystems.PointerEventData");
            var exec = FindType("UnityEngine.EventSystems.ExecuteEvents");
            var resultType = FindType("UnityEngine.EventSystems.RaycastResult");
            if (systemType == null || dataType == null || exec == null || resultType == null)
            {
                unavailable = true;  // the game has no uGUI
                systemType = null;
                return false;
            }
            current = systemType.GetProperty("current", BindingFlags.Public | BindingFlags.Static);
            raycastAll = systemType.GetMethods().First(m => m.Name == "RaycastAll" && m.GetParameters().Length == 2);
            setSelected = systemType.GetMethods().First(m => m.Name == "SetSelectedGameObject" && m.GetParameters().Length == 2);
            listType = raycastAll.GetParameters()[1].ParameterType;
            listCount = listType.GetProperty("Count");
            listItem = listType.GetProperty("Item");
            resultObject = resultType.GetProperty("gameObject");
            list = Activator.CreateInstance(listType);
            var methods = exec.GetMethods(BindingFlags.Public | BindingFlags.Static);
            MethodInfo Generic(string name, int args) => methods.First(m => m.Name == name && m.IsGenericMethodDefinition && m.GetParameters().Length == args);
            MethodInfo execute = Generic("Execute", 3), hierarchy = Generic("ExecuteHierarchy", 3), find = Generic("GetEventHandler", 1);
            foreach (var name in new[] { "pointerEnterHandler", "pointerExitHandler", "pointerDownHandler", "pointerUpHandler", "pointerClickHandler",
                         "initializePotentialDrag", "beginDragHandler", "dragHandler", "endDragHandler", "dropHandler", "scrollHandler", "selectHandler" })
            {
                var p = exec.GetProperty(name, BindingFlags.Public | BindingFlags.Static);
                var t = p.PropertyType.GetGenericArguments()[0];
                handlers[name] = new Handler
                {
                    Function = p.GetValue(null, null),
                    Execute = execute.MakeGenericMethod(t),
                    Hierarchy = hierarchy.MakeGenericMethod(t),
                    Find = find.MakeGenericMethod(t),
                };
            }
            Plugin.Log.LogInfo("Pointer drives the game's UI through its EventSystem");
            return true;
        }

        static Type FindType(string name)
        {
            foreach (var a in AppDomain.CurrentDomain.GetAssemblies())
            {
                var t = a.GetType(name, false);
                if (t != null) return t;
            }
#if IL2CPP
            // Interop assemblies load on first use; nothing may have touched uGUI's yet.
            try { return Assembly.Load("UnityEngine.UI").GetType(name, false); }
            catch (Exception) { return null; }
#else
            return null;
#endif
        }

        static GameObject Raycast()
        {
            listType.GetMethod("Clear").Invoke(list, null);
            raycastAll.Invoke(system, new[] { data, list });
            int n = (int)listCount.GetValue(list, null);
            for (int i = 0; i < n; i++)
            {
                var result = listItem.GetValue(list, new object[] { i });
                var go = (GameObject)resultObject.GetValue(result, null);
                if (go == null) continue;
                Set("pointerCurrentRaycast", result);
                return go;
            }
            return null;
        }

        static void Press(GameObject target, Vector2 at)
        {
            down = true;
            dragging = false;
            eligible = true;
            pressAt = at;
            Set("eligibleForClick", true);
            Set("dragging", false);
            Set("useDragThreshold", true);
            Set("pressPosition", at);
            Set("delta", Vector2.zero);
            Set("pointerPressRaycast", Get("pointerCurrentRaycast"));
            if (target == null) return;
            // Pressing outside the selected control deselects it, as a mouse click does.
            var selectable = Find("selectHandler", target);
            var selected = systemType.GetProperty("currentSelectedGameObject").GetValue(system, null) as GameObject;
            if (selectable != selected) setSelected.Invoke(system, new object[] { null, data });
            var press = Hierarchy("pointerDownHandler", target) ?? Find("pointerClickHandler", target);
            Set("clickCount", 1);
            Set("clickTime", Time.unscaledTime);
            Set("pointerPress", press);
            Set("rawPointerPress", target);
            pressed = press;
            dragged = Find("dragHandler", target);
            Set("pointerDrag", dragged);
            if (dragged != null) Execute("initializePotentialDrag", dragged);
        }

        static void Hold(GameObject target, Vector2 at)
        {
            if (dragged == null) return;
            if (!dragging && (at - pressAt).sqrMagnitude > Sq(10f * Screen.height / 1080f))
            {
                Execute("beginDragHandler", dragged);
                dragging = true;
                Set("dragging", true);
                // A drag that started on a button is no click (scroll lists, sliders).
                if (pressed != null && pressed != dragged)
                {
                    Execute("pointerUpHandler", pressed);
                    pressed = null;
                    Set("pointerPress", null);
                }
                eligible = false;
                Set("eligibleForClick", false);
            }
            if (dragging) Execute("dragHandler", dragged);
        }

        static void Up(GameObject target)
        {
            down = false;
            if (pressed != null) Execute("pointerUpHandler", pressed);
            if (target != null)
            {
                var click = Find("pointerClickHandler", target);
                if (pressed != null && pressed == click && eligible) Execute("pointerClickHandler", pressed);
                else if (dragging) Hierarchy("dropHandler", target);
            }
            if (dragging && dragged != null) Execute("endDragHandler", dragged);
            pressed = dragged = null;
            dragging = false;
            Set("pointerPress", null);
            Set("rawPointerPress", null);
            Set("pointerDrag", null);
            Set("dragging", false);
            Set("eligibleForClick", false);
        }

        // Exit to what the pointer left, enter to what it reached, up to their common parent (as uGUI does).
        static void Hover(GameObject target)
        {
            if (target == hovered) return;
            var common = CommonRoot(hovered, target);
            for (var t = hovered != null ? hovered.transform : null; t != null && t != common; t = t.parent) Execute("pointerExitHandler", t.gameObject);
            Set("pointerEnter", target);
            for (var t = target != null ? target.transform : null; t != null && t != common; t = t.parent) Execute("pointerEnterHandler", t.gameObject);
            hovered = target;
        }

        static Transform CommonRoot(GameObject a, GameObject b)
        {
            if (a == null || b == null) return null;
            for (var x = a.transform; x != null; x = x.parent)
                for (var y = b.transform; y != null; y = y.parent)
                    if (x == y) return x;
            return null;
        }

        // The pointer left or the EventSystem went: let go of whatever it held, without a click.
        static void Release()
        {
            if (system == null) return;
            try
            {
                if (down)
                {
                    eligible = false;
                    Up(null);
                }
                Hover(null);
            }
            catch (Exception)
            {
                // The objects may already be destroyed with their scene.
            }
            hovered = pressed = dragged = null;
            down = dragging = taken = false;
            system = data = null;
        }

        static bool Alive(object es) => (UnityEngine.Object)es != null && ((Behaviour)es).isActiveAndEnabled;

        static bool Same(object a, object b) => a != null && b != null && ((UnityEngine.Object)a).GetInstanceID() == ((UnityEngine.Object)b).GetInstanceID();

        static float Sq(float v) => v * v;

        static void Execute(string name, GameObject target)
        {
            var h = handlers[name];
            h.Execute.Invoke(null, new[] { target, data, h.Function });
        }

        static GameObject Hierarchy(string name, GameObject target)
        {
            var h = handlers[name];
            return h.Hierarchy.Invoke(null, new[] { target, data, h.Function }) as GameObject;
        }

        static GameObject Find(string name, GameObject target)
        {
            var go = handlers[name].Find.Invoke(null, new object[] { target }) as GameObject;
            return go != null ? go : null;
        }

        static object Get(string name)
        {
            var p = dataType.GetProperty(name);
            return p != null ? p.GetValue(data, null) : dataType.GetField(name).GetValue(data);
        }

        static void Set(string name, object value)
        {
            var p = dataType.GetProperty(name);
            if (p != null && p.CanWrite) p.SetValue(data, value, null);
            else dataType.GetField(name)?.SetValue(data, value);
        }
    }
}
