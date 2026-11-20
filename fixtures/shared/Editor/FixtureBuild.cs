// Copyright (C) 2026 EncidXVII
// SPDX-License-Identifier: GPL-3.0-only

using System;
using System.IO;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.EventSystems;
using UnityEngine.Rendering;
using UnityEngine.UI;

// Generates the fixture scenes and builds the player. Driven by fixtures/build.ps1, which runs Configure and Build
// as two editor launches (a scripting-runtime change only applies after a restart).
// Written for C# 4: Unity 2018.4 on the .NET 3.5 runtime compiles editor scripts with its old compiler too.
public static partial class FixtureBuild
{
    const string Scenes = "Assets/Scenes";
    const string Materials = "Assets/Materials";
    static string unlitShader = "Unlit/Color", colorProperty = "_Color";

    // Implemented in FixtureUrp.cs, which build.ps1 copies only into URP fixtures.
    static partial void ConfigurePipeline();
    static partial void DecorateScene(Camera camera);

    static string Name { get { return Arg("-fixture") ?? "F1"; } }

    static string Arg(string name)
    {
        var args = Environment.GetCommandLineArgs();
        int i = Array.IndexOf(args, name);
        return i >= 0 && i + 1 < args.Length ? args[i + 1] : null;
    }

    public static void Configure()
    {
        AddRightStickAxis();
        PlayerSettings.companyName = "PortXR";
        PlayerSettings.productName = Name;
        PlayerSettings.fullScreenMode = FullScreenMode.Windowed;
        PlayerSettings.defaultScreenWidth = 1280;
        PlayerSettings.defaultScreenHeight = 720;
        PlayerSettings.resizableWindow = true;
        PlayerSettings.runInBackground = true;
        PlayerSettings.colorSpace = ColorSpace.Linear;
        bool il2cpp = Arg("-backend") == "il2cpp";
        PlayerSettings.SetScriptingBackend(BuildTargetGroup.Standalone, il2cpp ? ScriptingImplementation.IL2CPP : ScriptingImplementation.Mono2x);
        // Keep whole engine modules (IMGUI, ScreenCapture) so the plugin's compile surface exists; real games may strip
        // them and the plugin falls back at runtime.
        PlayerSettings.stripEngineCode = false;
        // Mono players keep their full class libraries (the 2022 default) unless -strip low asks for a trimmed one.
#if UNITY_2020_1_OR_NEWER
        var il2cppStripping = ManagedStrippingLevel.Minimal;
#else
        var il2cppStripping = ManagedStrippingLevel.Low;  // Minimal arrived in 2020.1
#endif
        var monoStripping = Arg("-strip") == "low" ? ManagedStrippingLevel.Low : ManagedStrippingLevel.Disabled;
        PlayerSettings.SetManagedStrippingLevel(BuildTargetGroup.Standalone, il2cpp ? il2cppStripping : monoStripping);
#if !UNITY_2019_3_OR_NEWER
        // Pre-2019.3 players open a resolution dialog with a Play button; test runs must start unattended.
        PlayerSettings.displayResolutionDialog = ResolutionDialogSetting.Disabled;
        PlayerSettings.scriptingRuntimeVersion = Arg("-runtime") == "legacy" ? ScriptingRuntimeVersion.Legacy : ScriptingRuntimeVersion.Latest;
#endif
        if (Arg("-input") == "both")
        {
            // No public API for Active Input Handling; 2 = both the old Input Manager and the Input System package.
            var player = new SerializedObject(Unsupported.GetSerializedAssetInterfaceSingleton("PlayerSettings"));
            player.FindProperty("activeInputHandler").intValue = 2;
            player.ApplyModifiedProperties();
        }
        PlayerSettings.SetUseDefaultGraphicsAPIs(BuildTarget.StandaloneWindows64, false);
        PlayerSettings.SetGraphicsAPIs(BuildTarget.StandaloneWindows64, new[] { Arg("-gfx") == "d3d12" ? GraphicsDeviceType.Direct3D12 : GraphicsDeviceType.Direct3D11 });
        for (int i = 0; i < QualitySettings.names.Length; i++)
        {
            QualitySettings.SetQualityLevel(i, false);
            QualitySettings.antiAliasing = 4;
            QualitySettings.vSyncCount = 1;
        }
        ConfigurePipeline();
        AssetDatabase.SaveAssets();
    }

    public static void Build()
    {
        ConfigurePipeline();
        Directory.CreateDirectory(Scenes);
        Directory.CreateDirectory(Materials);
        var scenes = new[] { MakeScene("Main", new Color(0.10f, 0.15f, 0.30f)), MakeScene("Second", new Color(0.25f, 0.12f, 0.10f)) };
        var report = BuildPipeline.BuildPlayer(scenes, "Build/" + Name + ".exe", BuildTarget.StandaloneWindows64, BuildOptions.None);
        if (report.summary.result != UnityEditor.Build.Reporting.BuildResult.Succeeded) EditorApplication.Exit(1);
    }

    static string MakeScene(string name, Color sky)
    {
        var scene = EditorSceneManager.NewScene(NewSceneSetup.EmptyScene, NewSceneMode.Single);
        new GameObject("Sun").AddComponent<Light>().type = LightType.Directional;

        Box("Ground", new Vector3(0, -0.05f, 0), new Vector3(40, 0.1f, 40), new Color(0.5f, 0.5f, 0.5f));
        // 2.0 m tall door straight ahead at 5 m: the scale assertion measures its pixel height.
        Box("Door", new Vector3(0, 1f, 5f), new Vector3(1f, 2f, 0.1f), Color.magenta);
        // High and to the left: must land in the top-left quadrant of both eyes.
        Box("MarkerTopLeft", new Vector3(-3f, 4f, 6f), new Vector3(0.6f, 0.6f, 0.6f), Color.red);
        // Close to the eyes so it shifts strongly between them.
        Box("MarkerNear", new Vector3(0.35f, 1.3f, 0.8f), new Vector3(0.1f, 0.1f, 0.1f), Color.yellow);
        Box("Cube", new Vector3(2.5f, 0.5f, 4f), Vector3.one, Color.green);

        var player = new GameObject("Player");
        var cc = player.AddComponent<CharacterController>();
        cc.height = 1.8f;
        cc.center = new Vector3(0, 0.9f, 0);
        cc.radius = 0.3f;
        var camGo = new GameObject("Main Camera") { tag = "MainCamera" };
        camGo.transform.SetParent(player.transform, false);
        camGo.transform.localPosition = new Vector3(0, 1.6f, 0);
        var cam = camGo.AddComponent<Camera>();
        cam.clearFlags = CameraClearFlags.SolidColor;
        cam.backgroundColor = sky;
        cam.nearClipPlane = 0.05f;
        camGo.AddComponent<AudioListener>();
        player.AddComponent<FixturePlayer>().head = camGo.transform;
        DecorateScene(cam);

        MakeUi(name);
        var path = Scenes + "/" + name + ".unity";
        EditorSceneManager.SaveScene(scene, path);
        return path;
    }

    static void MakeUi(string sceneName)
    {
        var canvasGo = new GameObject("Canvas");
        var canvas = canvasGo.AddComponent<Canvas>();
        canvas.renderMode = RenderMode.ScreenSpaceOverlay;
        var scaler = canvasGo.AddComponent<CanvasScaler>();
        scaler.uiScaleMode = CanvasScaler.ScaleMode.ScaleWithScreenSize;
        scaler.referenceResolution = new Vector2(1280, 720);
        scaler.matchWidthOrHeight = 1f;
        canvasGo.AddComponent<GraphicRaycaster>();
#if UNITY_2022_2_OR_NEWER
        var font = Resources.GetBuiltinResource<Font>("LegacyRuntime.ttf");
#else
        var font = Resources.GetBuiltinResource<Font>("Arial.ttf");
#endif

        var title = Rect("Title", canvasGo.transform, new Vector2(0.5f, 1f), new Vector2(0, -40), new Vector2(800, 80));
        var text = title.gameObject.AddComponent<Text>();
        text.text = "PORTXR FIXTURE " + sceneName.ToUpperInvariant();
        text.font = font;
        text.fontSize = 48;
        text.alignment = TextAnchor.MiddleCenter;
        text.color = Color.white;

        // Centre at 120 px above the bottom of a 720 px reference: uv (0.5, 0.8333).
        var buttonRect = Rect("Button", canvasGo.transform, new Vector2(0.5f, 0f), new Vector2(0, 120), new Vector2(400, 120));
        buttonRect.gameObject.AddComponent<Image>().color = new Color(0.2f, 0.6f, 1f);
        buttonRect.gameObject.AddComponent<Button>();
        buttonRect.gameObject.AddComponent<FixtureButton>();
        var label = Rect("Label", buttonRect, new Vector2(0.5f, 0.5f), Vector2.zero, new Vector2(400, 120));
        var labelText = label.gameObject.AddComponent<Text>();
        labelText.text = "CLICK";
        labelText.font = font;
        labelText.fontSize = 40;
        labelText.alignment = TextAnchor.MiddleCenter;
        labelText.color = Color.black;

        // A 50% white square, left of middle: the VR panel must keep the opacity it has on the monitor (harness gate 6c).
        var veil = Rect("Translucent", canvasGo.transform, new Vector2(0f, 0.5f), new Vector2(100, 0), new Vector2(120, 120));
        veil.gameObject.AddComponent<Image>().color = new Color(1f, 1f, 1f, 0.5f);

        var es = new GameObject("EventSystem");
        es.AddComponent<EventSystem>();
        es.AddComponent<StandaloneInputModule>();
    }

    static RectTransform Rect(string name, Transform parent, Vector2 anchor, Vector2 pos, Vector2 size)
    {
        var rt = new GameObject(name).AddComponent<RectTransform>();
        rt.SetParent(parent, false);
        rt.anchorMin = rt.anchorMax = anchor;
        rt.anchoredPosition = pos;
        rt.sizeDelta = size;
        return rt;
    }

    static GameObject Box(string name, Vector3 pos, Vector3 size, Color color, PrimitiveType shape = PrimitiveType.Cube)
    {
        var go = GameObject.CreatePrimitive(shape);
        go.name = name;
        go.transform.position = pos;
        go.transform.localScale = size;
        UnityEngine.Object.DestroyImmediate(go.GetComponent<Collider>());
        if (name == "Ground") go.AddComponent<BoxCollider>();
        var matPath = Materials + "/" + name + ".mat";
        var mat = AssetDatabase.LoadAssetAtPath<Material>(matPath);
        if (mat == null)
        {
            mat = new Material(Shader.Find(unlitShader));
            mat.SetColor(colorProperty, color);
            AssetDatabase.CreateAsset(mat, matPath);
        }
        go.GetComponent<Renderer>().sharedMaterial = mat;
        return go;
    }

    static void AddRightStickAxis()
    {
        var so = new SerializedObject(AssetDatabase.LoadAllAssetsAtPath("ProjectSettings/InputManager.asset")[0]);
        var axes = so.FindProperty("m_Axes");
        for (int i = 0; i < axes.arraySize; i++)
            if (axes.GetArrayElementAtIndex(i).FindPropertyRelative("m_Name").stringValue == "RightX") return;
        axes.arraySize++;
        var a = axes.GetArrayElementAtIndex(axes.arraySize - 1);
        a.FindPropertyRelative("m_Name").stringValue = "RightX";
        a.FindPropertyRelative("descriptiveName").stringValue = "";
        a.FindPropertyRelative("negativeButton").stringValue = "";
        a.FindPropertyRelative("positiveButton").stringValue = "";
        a.FindPropertyRelative("altNegativeButton").stringValue = "";
        a.FindPropertyRelative("altPositiveButton").stringValue = "";
        a.FindPropertyRelative("gravity").floatValue = 0;
        a.FindPropertyRelative("dead").floatValue = 0.19f;
        a.FindPropertyRelative("sensitivity").floatValue = 1;
        a.FindPropertyRelative("snap").boolValue = false;
        a.FindPropertyRelative("invert").boolValue = false;
        a.FindPropertyRelative("type").intValue = 2;  // joystick axis
        a.FindPropertyRelative("axis").intValue = 3;  // 4th axis = XInput right stick X
        a.FindPropertyRelative("joyNum").intValue = 0;
        so.ApplyModifiedProperties();
    }
}
