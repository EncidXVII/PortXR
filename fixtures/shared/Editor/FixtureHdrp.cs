using System.Linq;
using UnityEditor;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.Rendering.HighDefinition;

// HDRP fixture only. Fixed exposure and no tonemapping keep the unlit marker colours exact (HDRP exposes unlit
// surfaces too); a Bloom override plus the HDR glow sphere shows whether the eye cameras kept the game's HDRP
// camera settings and volumes.
public static partial class FixtureBuild
{
    const string PipelineAsset = "Assets/Materials/Pipeline.asset";

    static partial void ConfigurePipeline()
    {
        unlitShader = "HDRP/Unlit";
        colorProperty = "_UnlitColor";
        System.IO.Directory.CreateDirectory(Materials);
        var asset = AssetDatabase.LoadAssetAtPath<HDRenderPipelineAsset>(PipelineAsset);
        if (asset == null)
        {
            asset = ScriptableObject.CreateInstance<HDRenderPipelineAsset>();
            AssetDatabase.CreateAsset(asset, PipelineAsset);
        }
        GraphicsSettings.renderPipelineAsset = asset;
        for (int i = 0; i < QualitySettings.names.Length; i++)
        {
            QualitySettings.SetQualityLevel(i, false);
            QualitySettings.renderPipeline = asset;
        }
        // The wizard normally creates the global settings asset (default volume, frame settings); batchmode does not.
        var settings = typeof(HDRenderPipelineAsset).Assembly.GetType("UnityEngine.Rendering.HighDefinition.HDRenderPipelineGlobalSettings");
        var ensure = settings == null ? null : settings.GetMethod("Ensure",
            System.Reflection.BindingFlags.Static | System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.NonPublic);
        if (ensure != null) ensure.Invoke(null, new object[ensure.GetParameters().Length].Select((_, i) => (object)true).ToArray());
    }

    static partial void DecorateScene(Camera camera)
    {
        var hd = camera.gameObject.AddComponent<HDAdditionalCameraData>();
        hd.clearColorMode = HDAdditionalCameraData.ClearColorMode.Color;
        hd.backgroundColorHDR = camera.backgroundColor;
        var profilePath = Materials + "/Post.asset";
        var profile = AssetDatabase.LoadAssetAtPath<VolumeProfile>(profilePath);
        if (profile == null)
        {
            profile = ScriptableObject.CreateInstance<VolumeProfile>();
            AssetDatabase.CreateAsset(profile, profilePath);
            var exposure = profile.Add<Exposure>();
            exposure.mode.Override(ExposureMode.Fixed);
            exposure.fixedExposure.Override(0f);
            var tonemapping = profile.Add<Tonemapping>();
            tonemapping.mode.Override(TonemappingMode.None);
            var bloom = profile.Add<Bloom>();
            bloom.threshold.Override(1.5f);
            bloom.intensity.Override(0.6f);
            foreach (var c in profile.components) AssetDatabase.AddObjectToAsset(c, profile);
            AssetDatabase.SaveAssets();
        }
        var volume = new GameObject("Post").AddComponent<Volume>();
        volume.isGlobal = true;
        volume.priority = 10;
        volume.sharedProfile = profile;
        Box("Glow", new Vector3(2f, 3.5f, 6f), Vector3.one * 0.3f, new Color(8f, 8f, 8f), PrimitiveType.Sphere);
    }
}
