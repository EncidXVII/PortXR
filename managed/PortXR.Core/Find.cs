// Copyright (C) 2026 EncidXVII
// SPDX-License-Identifier: GPL-3.0-only

using UnityEngine;

namespace PortXR
{
    internal static class Find
    {
        // Every loaded object of a type. IL2CPP games strip the generic lookup; the type-object one stays.
        public static T[] All<T>() where T : Object
        {
#if IL2CPP
            var found = Object.FindObjectsOfType(Il2CppInterop.Runtime.Il2CppType.Of<T>());
            var all = new T[found.Length];
            for (int i = 0; i < all.Length; i++) all[i] = found[i].Cast<T>();
            return all;
#else
            return Object.FindObjectsOfType<T>();
#endif
        }
    }
}
