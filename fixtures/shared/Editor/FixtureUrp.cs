// Copyright (C) 2026 EncidXVII
// SPDX-License-Identifier: GPL-3.0-only

using UnityEditor;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.Rendering.Universal;

// URP fixtures only. A Bloom volume plus an HDR glow sphere: the halo appears in the eyes only if the plugin
// carried the game camera's post-processing settings over.
public static partial class FixtureBuild
{
    const string PipelineAsset = "Assets/Materials/Pipeline.asset";

    static partial void ConfigurePipeline()
    {
        unlitShader = "Universal Render Pipeline/Unlit";
        colorProperty = "_BaseColor";
        System.IO.Directory.CreateDirectory(Materials);
        var rendererPath = Materials + "/Renderer.asset";
        var renderer = AssetDatabase.LoadAssetAtPath<UniversalRendererData>(rendererPath);
        if (renderer == null)
        {
            renderer = ScriptableObject.CreateInstance<UniversalRendererData>();
            AssetDatabase.CreateAsset(renderer, rendererPath);
        }
        // A renderer created from script has no post-process resources, and URP then silently skips post-processing.
        if (renderer.postProcessData == null)
        {
            renderer.postProcessData = AssetDatabase.LoadAssetAtPath<PostProcessData>(
                "Packages/com.unity.render-pipelines.universal/Runtime/Data/PostProcessData.asset");
            EditorUtility.SetDirty(renderer);
        }
        var asset = AssetDatabase.LoadAssetAtPath<UniversalRenderPipelineAsset>(PipelineAsset);
        if (asset == null)
        {
            asset = UniversalRenderPipelineAsset.Create(renderer);
            asset.supportsHDR = true;
            AssetDatabase.CreateAsset(asset, PipelineAsset);
        }
        GraphicsSettings.renderPipelineAsset = asset;
        for (int i = 0; i < QualitySettings.names.Length; i++)
        {
            QualitySettings.SetQualityLevel(i, false);
            QualitySettings.renderPipeline = asset;
        }
    }

    static partial void DecorateScene(Camera camera)
    {
        camera.GetUniversalAdditionalCameraData().renderPostProcessing = true;
        var profilePath = Materials + "/Post.asset";
        var profile = AssetDatabase.LoadAssetAtPath<VolumeProfile>(profilePath);
        if (profile == null)
        {
            profile = ScriptableObject.CreateInstance<VolumeProfile>();
            var bloom = profile.Add<Bloom>();
            bloom.threshold.Override(1.5f);
            bloom.intensity.Override(3f);
            AssetDatabase.CreateAsset(profile, profilePath);
            AssetDatabase.AddObjectToAsset(bloom, profile);
        }
        var volume = new GameObject("Post").AddComponent<Volume>();
        volume.isGlobal = true;
        volume.sharedProfile = profile;
        // Upper right, against the sky; HDR white far above the bloom threshold.
        Box("Glow", new Vector3(2f, 3.5f, 6f), Vector3.one * 0.3f, new Color(8f, 8f, 8f), PrimitiveType.Sphere);
    }
}
