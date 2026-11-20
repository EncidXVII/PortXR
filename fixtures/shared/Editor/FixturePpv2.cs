// Copyright (C) 2026 EncidXVII
// SPDX-License-Identifier: GPL-3.0-only

using UnityEditor;
using UnityEngine;
using UnityEngine.Rendering.PostProcessing;

// Built-in pipeline with the Post Processing v2 package: a PostProcessLayer on the camera and a global Bloom volume.
// The halo around the HDR glow sphere appears in the eyes only if the plugin carried the layer over.
public static partial class FixtureBuild
{
    static partial void ConfigurePipeline() { }

    static partial void DecorateScene(Camera camera)
    {
        camera.allowHDR = true;
        var layer = camera.gameObject.AddComponent<PostProcessLayer>();
        // Added from script, the layer has no shader resources until given them.
        layer.Init(AssetDatabase.LoadAssetAtPath<PostProcessResources>("Packages/com.unity.postprocessing/PostProcessing/PostProcessResources.asset"));
        layer.volumeLayer = ~0;
        layer.antialiasingMode = PostProcessLayer.Antialiasing.None;
        var profilePath = Materials + "/Post.asset";
        var profile = AssetDatabase.LoadAssetAtPath<PostProcessProfile>(profilePath);
        if (profile == null)
        {
            profile = ScriptableObject.CreateInstance<PostProcessProfile>();
            AssetDatabase.CreateAsset(profile, profilePath);
            var bloom = profile.AddSettings<Bloom>();
            bloom.enabled.Override(true);
            bloom.threshold.Override(1.5f);
            bloom.intensity.Override(3f);
            AssetDatabase.AddObjectToAsset(bloom, profile);
            AssetDatabase.SaveAssets();
        }
        var volume = new GameObject("Post").AddComponent<PostProcessVolume>();
        volume.isGlobal = true;
        volume.sharedProfile = profile;
        Box("Glow", new Vector3(2f, 3.5f, 6f), Vector3.one * 0.3f, new Color(8f, 8f, 8f), PrimitiveType.Sphere);
    }
}
