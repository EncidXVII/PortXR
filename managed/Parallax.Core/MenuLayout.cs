using System;
using System.Collections.Generic;
using UnityEngine;

namespace Parallax
{
    // The VR menu's own layout, in place of GUILayout: IL2CPP games that never used IMGUI strip its layout engine
    // (GUILayoutGroup.GetNext, BeginArea, Space) along with GUI.Label and GUI.DrawTexture. Only GUIStyle.Draw and
    // CalcSize, which call straight into the engine, are needed to draw.
    //
    // Rows (horizontal) and columns (vertical) nest; each item takes a rect along the group's axis. The menu body runs
    // twice per event: a measuring pass records every group's fixed size, flexible count and cross size, then the
    // drawing pass places items with those (flexible items in a row share what the fixed ones leave).
    internal sealed class MenuLayout
    {
        sealed class Group
        {
            public bool Row;
            public float X, Y, W, Cursor, Cross, Fixed;
            public int Index, Flex;
        }

        struct Measure
        {
            public float Fixed, Cross, Main;
            public int Flex;
        }

        readonly List<Measure> measures = new List<Measure>();
        readonly Stack<Group> stack = new Stack<Group>();
        int nextIndex;

        public bool Measuring { get; private set; }
        public static MenuLayout Current { get; private set; }  // the layout running now, if any
        public Rect Last { get; private set; }
        public bool InRow => stack.Peek().Row;

        public void Run(float width, Action body)
        {
            Current = this;
            try
            {
                Measuring = true;
                Pass(width, body);
                Measuring = false;
                Pass(width, body);
            }
            finally
            {
                Current = null;
                Measuring = false;
            }
        }

        void Pass(float width, Action body)
        {
            nextIndex = 0;
            stack.Clear();
            Push(false, 0, 0, width);
            body();
            stack.Clear();
        }

        Group Push(bool row, float x, float y, float w)
        {
            var g = new Group { Row = row, X = x, Y = y, W = w, Index = nextIndex++ };
            if (Measuring)
            {
                while (measures.Count <= g.Index) measures.Add(default);
                measures[g.Index] = default;
            }
            stack.Push(g);
            return g;
        }

        Measure MeasureOf(int index) => index < measures.Count ? measures[index] : default;

        // Width 0: in a row a flexible share, in a column the column's width.
        public Rect Take(float w, float h)
        {
            var g = stack.Peek();
            Rect r;
            if (g.Row)
            {
                var m = MeasureOf(g.Index);
                float width = w > 0 ? w : m.Flex > 0 ? Mathf.Max(0, (g.W - m.Fixed) / m.Flex) : 0;
                if (w > 0) g.Fixed += w;
                else g.Flex++;
                r = new Rect(g.X + g.Cursor, g.Y, width, h);
                g.Cursor += width;
                g.Cross = Mathf.Max(g.Cross, h);
            }
            else
            {
                float width = w > 0 ? w : g.W;
                r = new Rect(g.X, g.Y + g.Cursor, width, h);
                g.Cursor += h;
                g.Fixed += h;
                g.Cross = Mathf.Max(g.Cross, width);
            }
            Last = r;
            return r;
        }

        public void Gap(float px)
        {
            var g = stack.Peek();
            g.Cursor += px;
            g.Fixed += px;
        }

        // Pushes what follows in the row to its right end.
        public void Flex()
        {
            if (InRow) Take(0, 0);
        }

        public void BeginRow()
        {
            var parent = stack.Peek();
            int index = nextIndex;
            float w = parent.Row ? MeasureOf(index).Main : parent.W;
            Push(true, parent.Row ? parent.X + parent.Cursor : parent.X, parent.Row ? parent.Y : parent.Y + parent.Cursor, w);
        }

        public void EndRow() => End();

        // Width 0: as wide as the parent column, or (in a row) as its widest item.
        public void BeginColumn(float width = 0)
        {
            var parent = stack.Peek();
            int index = nextIndex;
            float w = width > 0 ? width : parent.Row ? MeasureOf(index).Cross : parent.W;
            Push(false, parent.Row ? parent.X + parent.Cursor : parent.X, parent.Row ? parent.Y : parent.Y + parent.Cursor, w);
        }

        public void EndColumn() => End();

        void End()
        {
            var g = stack.Pop();
            if (Measuring) measures[g.Index] = new Measure { Fixed = g.Fixed, Flex = g.Flex, Cross = g.Cross, Main = g.Cursor };
            var parent = stack.Peek();
            // The group takes its place in the parent: a row is as tall as its tallest item (and, in a column still being
            // measured, as wide as its items), a column as tall as its items.
            var r = g.Row
                ? (parent.Row ? Take(g.Cursor, g.Cross) : Take(Mathf.Max(g.W, g.Cursor), g.Cross))
                : Take(g.W, g.Cursor);
            Last = r;
        }
    }
}
