using System.IO;
using System.Runtime.InteropServices;
using UnityEditor;
using UnityEngine;

namespace Pvd.Playback
{
    [CustomEditor(typeof(PvdPlayer))]
    internal sealed class PvdPlayerEditor : Editor
    {
        private const string NativePluginPath = "Runtime/Plugins/x86_64/pvd_unity_v3.dll";

        private static readonly GUIContent BindAddressLabel = new GUIContent(
            "Bind Address", "Numeric IPv4 or IPv6 address. Use 127.0.0.1 for a local sender.");
        private static readonly GUIContent TcpPortLabel = new GUIContent(
            "TCP Port", "Port for the PVD sender to connect to. Use 0 to select an available port automatically.");

        private PvdPlayer player;
        private SerializedProperty capturePath;
        private SerializedProperty listenAddress;
        private SerializedProperty listenPort;
        private SerializedProperty loadOnStart;
        private SerializedProperty framesPerSecond;
        private SerializedProperty loop;
        private SerializedProperty materialOverride;
        private SerializedProperty mirrorZ;
        private string nativePluginIssue;

        private void OnEnable()
        {
            player = (PvdPlayer)target;
            capturePath = serializedObject.FindProperty("capturePath");
            listenAddress = serializedObject.FindProperty("listenAddress");
            listenPort = serializedObject.FindProperty("listenPort");
            loadOnStart = serializedObject.FindProperty("loadOnStart");
            framesPerSecond = serializedObject.FindProperty("framesPerSecond");
            loop = serializedObject.FindProperty("loop");
            materialOverride = serializedObject.FindProperty("materialOverride");
            mirrorZ = serializedObject.FindProperty("mirrorZ");
            player.StateChanged += OnPlaybackStateChanged;
            EditorApplication.projectChanged += RefreshNativePluginIssue;
            RefreshNativePluginIssue();
        }

        private void OnDisable()
        {
            EditorApplication.projectChanged -= RefreshNativePluginIssue;
            if (player != null)
                player.StateChanged -= OnPlaybackStateChanged;
        }

        public override void OnInspectorGUI()
        {
            serializedObject.Update();
            if (nativePluginIssue != null)
                EditorGUILayout.HelpBox(nativePluginIssue, MessageType.Error);

            bool listening = player.IsLive && (player.IsLoading || player.LiveState == PvdLiveState.Listening ||
                                               player.LiveState == PvdLiveState.Receiving);
            DrawCaptureControls(listening);
            DrawListenerControls(listening);

            EditorGUILayout.Space();
            EditorGUILayout.PropertyField(loadOnStart);
            EditorGUILayout.PropertyField(framesPerSecond);
            EditorGUILayout.PropertyField(loop);
            EditorGUILayout.PropertyField(materialOverride);
            EditorGUILayout.PropertyField(mirrorZ);
            serializedObject.ApplyModifiedProperties();

            if (nativePluginIssue == null && !string.IsNullOrEmpty(player.ErrorMessage))
                EditorGUILayout.HelpBox(player.ErrorMessage, MessageType.Error);

            if (player.IsLoading && !player.IsLive)
                EditorGUILayout.LabelField("Opening PVD capture…");

            if (player.IsLoaded && player.FrameCount > 0)
                DrawPlaybackControls();
            else if (player.HasPreviewGeometry)
            {
                EditorGUILayout.Space();
                using (new EditorGUI.DisabledScope(!player.isActiveAndEnabled))
                {
                    if (GUILayout.Button("Frame in Scene View"))
                        FrameInSceneView();
                }
            }
        }

        private void DrawCaptureControls(bool listening)
        {
            EditorGUILayout.LabelField("Recorded Capture", EditorStyles.boldLabel);
            EditorGUILayout.PropertyField(capturePath);

            using (new EditorGUILayout.HorizontalScope())
            {
                using (new EditorGUI.DisabledScope(!player.isActiveAndEnabled || listening || nativePluginIssue != null))
                {
                    if (GUILayout.Button("Browse & Open…"))
                        BrowseForCapture();

                    using (new EditorGUI.DisabledScope(string.IsNullOrWhiteSpace(capturePath.stringValue)))
                    {
                        if (GUILayout.Button("Open Capture"))
                            OpenCapture(capturePath.stringValue);
                    }
                }

                if (!player.IsLive)
                {
                    using (new EditorGUI.DisabledScope(!player.IsLoaded && !player.IsLoading))
                    {
                        if (GUILayout.Button("Close"))
                            player.Close();
                    }
                }
            }
        }

        private void DrawListenerControls(bool listening)
        {
            EditorGUILayout.Space();
            EditorGUILayout.LabelField("Live PVD", EditorStyles.boldLabel);
            using (new EditorGUI.DisabledScope(listening))
            {
                EditorGUILayout.PropertyField(listenAddress, BindAddressLabel);
                EditorGUILayout.PropertyField(listenPort, TcpPortLabel);
            }

            using (new EditorGUILayout.HorizontalScope())
            {
                string address = listenAddress.stringValue;
                int port = listenPort.intValue;
                bool canStart = player.isActiveAndEnabled && !listening && nativePluginIssue == null &&
                                !string.IsNullOrWhiteSpace(address) && port >= 0 && port <= 65535;
                using (new EditorGUI.DisabledScope(!canStart))
                {
                    if (GUILayout.Button("Start Listening"))
                    {
                        serializedObject.ApplyModifiedProperties();
                        player.StartListening(address.Trim(), port);
                    }
                }

                using (new EditorGUI.DisabledScope(!listening))
                {
                    if (GUILayout.Button("Stop Listening"))
                        player.StopListening();
                }

                if (player.IsLive && !listening && GUILayout.Button("Close Session"))
                    player.Close();
            }

            if (!player.IsLive)
                return;

            EditorGUILayout.LabelField(LiveStatus());
            EditorGUILayout.LabelField("Received", $"{player.ReceivedBytes:N0} bytes");
            EditorGUILayout.LabelField("Complete Frames", $"{player.FrameCount:N0}");
            if (player.LiveState == PvdLiveState.Receiving && player.FrameCount == 0)
            {
                if (player.ReceivedBytes == 0)
                    EditorGUILayout.LabelField("Waiting for the sender to write PVD data…");
                else if (player.HasPreviewGeometry)
                    EditorGUILayout.LabelField("Showing live geometry before the first complete frame.");
                else if (player.HasPreview)
                    EditorGUILayout.LabelField("Waiting for geometry or the first complete frame…");
                else
                    EditorGUILayout.LabelField("Waiting for a complete PVD update…");
            }

            if (listening && !player.IsLoading)
            {
                bool follow = EditorGUILayout.Toggle("Follow Latest", player.IsFollowingLive);
                if (follow != player.IsFollowingLive)
                {
                    if (follow)
                        player.FollowLatest();
                    else
                        player.Pause();
                }
            }
        }

        private string LiveStatus()
        {
            if (player.IsLoading)
                return "Starting TCP listener…";

            switch (player.LiveState)
            {
                case PvdLiveState.Listening:
                    return $"Listening on {player.ListenAddress} (TCP {player.BoundPort})";
                case PvdLiveState.Receiving:
                    return $"TCP connected on port {player.BoundPort}";
                case PvdLiveState.Completed:
                    return "Sender disconnected.";
                case PvdLiveState.Stopped:
                    return "Listening stopped.";
                case PvdLiveState.Truncated:
                    return "Sender disconnected before completing the capture.";
                case PvdLiveState.Failed:
                    return "Listener failed.";
                default:
                    return "Listener idle.";
            }
        }

        private void BrowseForCapture()
        {
            string path = EditorUtility.OpenFilePanel("Open PVD capture", string.Empty, "pxd2");
            if (!string.IsNullOrEmpty(path))
                OpenCapture(path);
        }

        private void OpenCapture(string path)
        {
            capturePath.stringValue = path;
            serializedObject.ApplyModifiedProperties();
            player.Open(path);
        }

        private void DrawPlaybackControls()
        {
            EditorGUILayout.Space();
            using (new EditorGUI.DisabledScope(player.IsLoading || !player.isActiveAndEnabled))
            {
                using (new EditorGUILayout.HorizontalScope())
                {
                    string playLabel = player.IsLive ? "Play History" : "Play";
                    if (GUILayout.Button(player.IsPlaying ? "Pause" : playLabel))
                    {
                        if (player.IsPlaying)
                            player.Pause();
                        else
                            player.Play();
                    }

                    using (new EditorGUI.DisabledScope(player.CurrentFrame < 0))
                    {
                        if (GUILayout.Button("Frame in Scene View"))
                            FrameInSceneView();
                    }
                }

                int displayFrame = player.IsSeeking ? player.RequestedFrame : player.CurrentFrame;
                int frameNumber = Mathf.Clamp(displayFrame + 1, 1, player.FrameCount);
                using (var changed = new EditorGUI.ChangeCheckScope())
                {
                    int nextFrame = EditorGUILayout.IntSlider("Frame", frameNumber, 1, player.FrameCount);
                    if (changed.changed)
                    {
                        player.Pause();
                        player.Seek(nextFrame - 1);
                    }
                }
            }

            string frameStatus = player.IsSeeking
                ? $"Seeking to frame {player.RequestedFrame + 1} of {player.FrameCount}…"
                : $"{player.FrameCount} recorded frames";
            EditorGUILayout.LabelField(frameStatus, EditorStyles.miniLabel);
        }

        private void FrameInSceneView()
        {
            if (!player.TryGetWorldBounds(out Bounds bounds))
                bounds = new Bounds(player.transform.position, Vector3.one);

            SceneView view = SceneView.lastActiveSceneView;
            if (view == null)
                view = EditorWindow.GetWindow<SceneView>();
            view.Focus();
            view.Frame(bounds, false);
        }

        private void OnPlaybackStateChanged()
        {
            Repaint();
        }

        private void RefreshNativePluginIssue()
        {
            nativePluginIssue = CheckNativePlugin();
            Repaint();
        }

        private static string CheckNativePlugin()
        {
            var package = UnityEditor.PackageManager.PackageInfo.FindForAssembly(typeof(PvdPlayer).Assembly);
            if (package == null)
                return null;

            if (Application.platform != RuntimePlatform.WindowsEditor || RuntimeInformation.ProcessArchitecture != Architecture.X64)
                return $"PVD Playback requires a Windows x64 Unity Editor. This Editor is {Application.platform} ({RuntimeInformation.ProcessArchitecture}).";

            string installedPath = Path.Combine(package.resolvedPath, NativePluginPath);
            if (!File.Exists(installedPath))
            {
                return "The installed PVD Playback package is missing its native DLL. In Package Manager, use " +
                       $"Add package from tarball to install the built .tgz from out/unity.\nExpected DLL: {installedPath}";
            }

            string assetPath = $"{package.assetPath}/{NativePluginPath}";
            if (!(AssetImporter.GetAtPath(assetPath) is PluginImporter importer) || !importer.isNativePlugin)
                return $"Unity did not import the DLL as a native plugin. Reimport the built package.\nAsset: {assetPath}\nDLL: {installedPath}";

            bool editorEnabled = importer.GetCompatibleWithAnyPlatform()
                ? !importer.GetExcludeEditorFromAnyPlatform()
                : importer.GetCompatibleWithEditor();
            string cpu = importer.GetEditorData("CPU");
            string os = importer.GetEditorData("OS");
            bool supportsX64 = string.IsNullOrEmpty(cpu) || cpu == "x86_64" || cpu == "AnyCPU";
            bool supportsWindows = string.IsNullOrEmpty(os) || os == "Windows" || os == "AnyOS";
            if (!editorEnabled || !supportsX64 || !supportsWindows)
            {
                return "The native plugin is not enabled for this Windows x64 Editor. " +
                       $"Editor enabled: {editorEnabled}, CPU: '{cpu}', OS: '{os}'.\nAsset: {assetPath}\nDLL: {installedPath}";
            }

            return null;
        }

        [MenuItem("GameObject/PVD Playback", false, 10)]
        private static void CreatePlayback(MenuCommand command)
        {
            var replay = new GameObject("PVD Playback", typeof(PvdPlayer));
            GameObjectUtility.SetParentAndAlign(replay, command.context as GameObject);
            Undo.RegisterCreatedObjectUndo(replay, "Create PVD Playback");
            Selection.activeGameObject = replay;
        }
    }
}
