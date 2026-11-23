using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;
using UnityEngine;
using UnityEngine.Rendering;

namespace Pvd.Playback
{
    [ExecuteAlways]
    [DisallowMultipleComponent]
    [AddComponentMenu("PVD/PVD Playback")]
    public sealed class PvdPlayer : MonoBehaviour
    {
        [SerializeField] private string capturePath = "";
        [SerializeField] private bool loadOnStart;
        [SerializeField, Min(0.01f)] private float framesPerSecond = 60f;
        [SerializeField] private bool loop = true;
        [SerializeField] private Material materialOverride;
        [SerializeField] private bool mirrorZ = true;
        [SerializeField] private string listenAddress = "127.0.0.1";
        [SerializeField] private int listenPort = 5425;

        private static readonly VertexAttributeDescriptor[] MeshLayout =
        {
            new VertexAttributeDescriptor(VertexAttribute.Position, VertexAttributeFormat.Float32, 3),
            new VertexAttributeDescriptor(VertexAttribute.Normal, VertexAttributeFormat.Float32, 3)
        };

        private sealed class Visual
        {
            public GameObject Object;
            public MeshFilter Filter;
            public MeshRenderer Renderer;
            public Transform Transform;
            public Vector3 Position;
            public Quaternion Rotation;
            public uint MeshId;
            public ulong SeenRevision;
        }

        private readonly Dictionary<uint, Mesh> meshes = new Dictionary<uint, Mesh>();
        private readonly Dictionary<NativePlayback.Instance, Visual> visuals =
            new Dictionary<NativePlayback.Instance, Visual>();
        private readonly Stack<Visual> visualPool = new Stack<Visual>();
        private readonly List<NativePlayback.Instance> removed = new List<NativePlayback.Instance>();

        private PlaybackWorker worker;
        private Thread previousWorker;
        private GameObject visualRoot;
        private Material defaultMaterial;
        private Material appliedMaterialOverride;
        private string playbackError = "";
        private string listenerError = "";
        private bool activeMirrorZ;
        private ulong availablePreviewEventCount;
        private ulong requestedPreviewEventCount;
        private ulong visualRevision;
        private long previousTick;
        private double playbackCursor;

        public string CapturePath { get => capturePath; set => capturePath = value ?? ""; }
        public bool LoadOnStart { get => loadOnStart; set => loadOnStart = value; }
        public float FramesPerSecond { get => ValidFrameRate(framesPerSecond); set => framesPerSecond = ValidFrameRate(value); }
        public bool Loop { get => loop; set => loop = value; }
        public Material MaterialOverride { get => materialOverride; set => materialOverride = value; }
        public bool MirrorZ { get => mirrorZ; set => mirrorZ = value; }
        public string ListenAddress { get => listenAddress; set => listenAddress = value ?? ""; }
        public int ListenPort { get => listenPort; set => listenPort = value; }
        public bool IsLoading { get; private set; }
        public bool IsLoaded => CurrentFrame >= 0;
        public bool HasPreview => PreviewEventCount != 0;
        public bool HasPreviewGeometry => HasPreview && visuals.Count != 0;
        public ulong PreviewEventCount { get; private set; }
        public bool IsPlaying { get; private set; }
        public bool IsSeeking => FrameCount > 0 && RequestedFrame >= 0 && CurrentFrame != RequestedFrame;
        public bool IsLive { get; private set; }
        public bool IsFollowingLive { get; private set; }
        public PvdLiveState LiveState { get; private set; }
        public int BoundPort { get; private set; }
        public ulong ReceivedBytes { get; private set; }
        public int FrameCount { get; private set; }
        public int CurrentFrame { get; private set; } = -1;
        public int RequestedFrame { get; private set; } = -1;
        public string ErrorMessage => playbackError.Length == 0 ? listenerError : playbackError;
        public event Action StateChanged;

        private void OnEnable()
        {
            previousTick = Stopwatch.GetTimestamp();
#if UNITY_EDITOR
            UnityEditor.EditorApplication.update += TickInEditMode;
#endif
            if (Application.IsPlaying(gameObject) && loadOnStart && !string.IsNullOrWhiteSpace(capturePath))
                Open(capturePath);
        }

        private void OnDisable()
        {
#if UNITY_EDITOR
            UnityEditor.EditorApplication.update -= TickInEditMode;
#endif
            Close();
        }

        private void Update()
        {
#if UNITY_EDITOR
            if (UnityEditor.EditorApplication.isPaused)
                return;
#endif
            if (Application.IsPlaying(gameObject))
                Tick();
        }

#if UNITY_EDITOR
        private void TickInEditMode()
        {
            if (!isActiveAndEnabled)
                return;

            if (!Application.IsPlaying(gameObject))
                Tick();
            else if (UnityEditor.EditorApplication.isPaused)
                Tick(advancePlayback: false);
        }
#endif

        /// <summary>Asynchronously opens a raw PhysX PVD recording and displays its first frame.</summary>
        public void Open(string path)
        {
            CapturePath = path;
            OpenSource(() =>
            {
                if (string.IsNullOrWhiteSpace(path))
                    throw new ArgumentException("Select a raw PhysX 3.4 PVD recording.");
                return new PlaybackWorker(Path.GetFullPath(path), 0, activeMirrorZ, previousWorker);
            }, false);
        }

        /// <summary>Listens for one PhysX PVD TCP stream and follows its latest complete frames.</summary>
        public void StartListening(string address, int port)
        {
            ListenAddress = address;
            ListenPort = port;
            OpenSource(() =>
            {
                if (string.IsNullOrWhiteSpace(address) || address.IndexOf('\0') >= 0)
                    throw new ArgumentException("Enter a valid numeric IPv4 or IPv6 listening address.");
                if (port < 0 || port > ushort.MaxValue)
                    throw new ArgumentOutOfRangeException(nameof(port), "The listening port must be between 0 and 65535.");

                return new PlaybackWorker(address.Trim(), (ushort)port, activeMirrorZ, previousWorker);
            }, true);
        }

        public void StartListening() => StartListening(listenAddress, listenPort);

        /// <summary>Stops network capture. Complete received frames remain available for playback.</summary>
        public void StopListening()
        {
            if (IsLive && LiveState < PvdLiveState.Completed)
                worker?.StopListening();
        }

        /// <summary>Displays the newest complete frame and stays at the incoming end of the timeline.</summary>
        public void FollowLatest()
        {
            if (!IsLive || worker == null)
                return;

            IsFollowingLive = true;
            IsPlaying = false;
            if (FrameCount > 0 && RequestedFrame != FrameCount - 1)
                QueueFrame(FrameCount - 1);
            else
                Notify();
        }

        private void OpenSource(Func<PlaybackWorker> create, bool live)
        {
            ResetPlayback();
            IsLive = live;
            IsFollowingLive = live;
            try
            {
                if (!isActiveAndEnabled)
                    throw new InvalidOperationException("Enable the PVD Player before opening a source.");
                if ((Application.platform != RuntimePlatform.WindowsEditor &&
                     Application.platform != RuntimePlatform.WindowsPlayer) ||
                    RuntimeInformation.ProcessArchitecture != Architecture.X64)
                    throw new PlatformNotSupportedException(
                        $"PVD Playback requires a Windows x64 Unity process. Detected {RuntimeInformation.ProcessArchitecture}.");

                activeMirrorZ = mirrorZ;
                worker = create();
                previousWorker = null;
                RequestedFrame = live ? -1 : 0;
                IsLoading = true;
            }
            catch (Exception error)
            {
                playbackError = error.Message;
                if (live)
                {
                    LiveState = PvdLiveState.Failed;
                    IsFollowingLive = false;
                }
            }

            Notify();
        }

        /// <summary>Closes the current recording and releases its generated preview objects.</summary>
        public void Close()
        {
            ResetPlayback();
            Notify();
        }

        public void Play()
        {
            if (!IsLoaded || FrameCount < 2 || IsPlaying)
                return;

            IsFollowingLive = false;
            if (RequestedFrame == FrameCount - 1)
                QueueFrame(0, notify: false);
            playbackCursor = RequestedFrame;
            previousTick = Stopwatch.GetTimestamp();
            IsPlaying = true;
            Notify();
        }

        public void Pause()
        {
            if (!IsPlaying && !IsFollowingLive)
                return;

            IsPlaying = IsFollowingLive = false;
            Notify();
        }

        /// <summary>Requests a zero-based recorded frame. Consecutive requests keep the latest target.</summary>
        public void Seek(int frame)
        {
            if (FrameCount == 0 || worker == null)
                return;

            bool wasFollowing = IsFollowingLive;
            IsFollowingLive = false;
            int target = Mathf.Clamp(frame, 0, FrameCount - 1);
            playbackCursor = target;
            previousTick = Stopwatch.GetTimestamp();
            if (target != RequestedFrame)
                QueueFrame(target);
            else if (wasFollowing)
                Notify();
        }

        public bool TryGetWorldBounds(out Bounds bounds)
        {
            bounds = default;
            bool found = false;
            foreach (var visual in visuals.Values)
            {
                if (found)
                    bounds.Encapsulate(visual.Renderer.bounds);
                else
                {
                    bounds = visual.Renderer.bounds;
                    found = true;
                }
            }

            return found;
        }

        private void Tick(bool advancePlayback = true)
        {
            long now = Stopwatch.GetTimestamp();
            double elapsed = (now - previousTick) / (double)Stopwatch.Frequency;
            previousTick = now;
            if (worker == null)
                return;

            var currentWorker = worker;
            bool statusChanged = false;
            if (IsLive)
            {
                var status = currentWorker.TakeLiveStatus();
                if (status.HasValue)
                    statusChanged = AcceptLiveStatus(status.Value);
            }

            using (var result = currentWorker.Take())
            {
                if (result != null)
                {
                    Accept(result);
                    statusChanged = false;
                }
            }
            if (statusChanged)
                Notify();
            if (worker != currentWorker)
                return;

            if (activeMirrorZ != mirrorZ)
            {
                ReorientPreview();
                return;
            }

            if ((IsLoaded || HasPreview) && appliedMaterialOverride != materialOverride)
                RefreshMaterial();

            if (IsFollowingLive)
            {
                AdvanceLive();
                return;
            }

            if (!advancePlayback || !IsPlaying || !IsLoaded)
                return;

            playbackCursor += elapsed * FramesPerSecond;
            playbackCursor = loop ? playbackCursor % FrameCount : Math.Min(FrameCount - 1, playbackCursor);
            if (IsSeeking)
                return;

            int target = (int)playbackCursor;
            if (target != CurrentFrame)
                QueueFrame(target);
            else if (!loop && CurrentFrame == FrameCount - 1)
                Pause();
        }

        private void ReorientPreview()
        {
            activeMirrorZ = mirrorZ;
            int target = IsFollowingLive && FrameCount > 0 ? FrameCount - 1 : RequestedFrame;
            ulong previewTarget = 0;
            if (target < 0)
                previewTarget = IsFollowingLive ? availablePreviewEventCount : requestedPreviewEventCount;

            worker.ChangeOrientation(activeMirrorZ, target, previewTarget);
            ReleaseVisuals();
            CurrentFrame = -1;
            RequestedFrame = target;
            PreviewEventCount = 0;
            requestedPreviewEventCount = previewTarget;
            Notify();
        }

        private void AdvanceLive()
        {
            if (FrameCount > 0)
            {
                if (IsSeeking)
                    return;
                if (CurrentFrame != FrameCount - 1)
                {
                    QueueFrame(FrameCount - 1);
                    return;
                }
            }
            else
            {
                if (requestedPreviewEventCount != PreviewEventCount)
                    return;
                if (availablePreviewEventCount > PreviewEventCount)
                {
                    requestedPreviewEventCount = availablePreviewEventCount;
                    worker.RequestPreview(requestedPreviewEventCount);
                    return;
                }
            }

            if (LiveState >= PvdLiveState.Completed)
                Pause();
        }

        private bool AcceptLiveStatus(NativePlayback.LiveStatus status)
        {
            bool changed = IsLoading || LiveState != status.State || FrameCount != status.FrameCount ||
                           BoundPort != status.BoundPort || ReceivedBytes != status.ReceivedBytes ||
                           availablePreviewEventCount != status.PreviewEventCount;
            IsLoading = false;
            LiveState = status.State;
            FrameCount = status.FrameCount;
            BoundPort = status.BoundPort;
            ReceivedBytes = status.ReceivedBytes;
            availablePreviewEventCount = status.PreviewEventCount;
            if (LiveState == PvdLiveState.Failed || LiveState == PvdLiveState.Truncated)
                listenerError = string.IsNullOrEmpty(status.Error) ? "The PVD stream ended with an error." : status.Error;
            return changed;
        }

        private void RefreshMaterial()
        {
            appliedMaterialOverride = materialOverride;
            try
            {
                var material = PreviewMaterial();
                foreach (var visual in visuals.Values)
                    visual.Renderer.sharedMaterial = material;
                playbackError = "";
            }
            catch (Exception error)
            {
                playbackError = error.Message;
                IsPlaying = false;
            }

            Notify();
        }

        private void QueueFrame(int target, bool notify = true)
        {
            RequestedFrame = target;
            requestedPreviewEventCount = 0;
            playbackError = "";
            worker.Request(target);
            if (notify)
                Notify();
        }

        private void Accept(PlaybackWorker.Result result)
        {
            IsLoading = false;
            if (IsLive && result.Frame < 0 && result.PreviewEventCount == 0)
            {
                ResetPlayback();
                IsLive = true;
                LiveState = PvdLiveState.Failed;
                listenerError = result.Error;
                Notify();
                return;
            }

            try
            {
                if (!string.IsNullOrEmpty(result.Error))
                    throw new InvalidOperationException(result.Error);

                ApplyFrame(result);
                worker.AcknowledgeMeshes(result.Meshes);
                FrameCount = Math.Max(FrameCount, result.FrameCount);
                CurrentFrame = RequestedFrame = result.Frame;
                PreviewEventCount = requestedPreviewEventCount = result.PreviewEventCount;
                playbackError = "";
            }
            catch (Exception error)
            {
                if (!IsLive && !IsLoaded)
                    ResetPlayback();
                playbackError = error.Message;
                IsPlaying = IsFollowingLive = false;
                RequestedFrame = CurrentFrame;
                requestedPreviewEventCount = PreviewEventCount;
            }

            Notify();
        }

        private void ApplyFrame(PlaybackWorker.Result result)
        {
            foreach (var source in result.Meshes)
                if (!meshes.ContainsKey(source.Id))
                    meshes.Add(source.Id, CreateMesh(source));

            if (result.InstanceCount != 0 && visualRoot == null)
            {
                visualRoot = new GameObject("PVD Preview") { hideFlags = HideFlags.HideAndDontSave };
                visualRoot.transform.SetParent(transform, false);
            }

            Material material = result.InstanceCount != 0 ? PreviewMaterial() : null;
            bool materialChanged = appliedMaterialOverride != materialOverride;
            appliedMaterialOverride = materialOverride;
            ulong revision = ++visualRevision;
            for (int i = 0; i < result.InstanceCount; ++i)
            {
                var instance = result.Instances[i];
                bool created = !visuals.TryGetValue(instance, out var visual);
                if (created)
                {
                    visual = AcquireVisual();
                    visuals.Add(instance, visual);
                }

                visual.SeenRevision = revision;
                if (created || visual.MeshId != instance.MeshId)
                {
                    visual.Filter.sharedMesh = meshes[instance.MeshId];
                    visual.MeshId = instance.MeshId;
                }

                if (created || materialChanged)
                    visual.Renderer.sharedMaterial = material;

                var position = new Vector3(instance.Px, instance.Py, activeMirrorZ ? -instance.Pz : instance.Pz);
                var rotation = activeMirrorZ
                    ? new Quaternion(-instance.Qx, -instance.Qy, instance.Qz, instance.Qw)
                    : new Quaternion(instance.Qx, instance.Qy, instance.Qz, instance.Qw);
                if (created || !visual.Position.Equals(position) || !visual.Rotation.Equals(rotation))
                {
                    visual.Transform.SetLocalPositionAndRotation(position, rotation);
                    visual.Position = position;
                    visual.Rotation = rotation;
                }
            }

            removed.Clear();
            foreach (var entry in visuals)
                if (entry.Value.SeenRevision != revision)
                    removed.Add(entry.Key);

            foreach (var key in removed)
            {
                var visual = visuals[key];
                visuals.Remove(key);
                visual.Filter.sharedMesh = null;
                visual.Object.SetActive(false);
                if (visualPool.Count < 256)
                    visualPool.Push(visual);
                else
                    DestroyPreview(visual.Object);
            }
        }

        private static Mesh CreateMesh(NativePlayback.MeshData source)
        {
            var mesh = new Mesh
            {
                name = "PVD Mesh " + source.Id,
                hideFlags = HideFlags.HideAndDontSave,
                indexFormat = source.Vertices.Length > ushort.MaxValue ? IndexFormat.UInt32 : IndexFormat.UInt16
            };
            try
            {
                mesh.SetVertexBufferParams(source.Vertices.Length, MeshLayout);
                mesh.SetVertexBufferData(source.Vertices, 0, 0, source.Vertices.Length);
                mesh.subMeshCount = 1;
                mesh.SetIndices(source.Indices, MeshTopology.Triangles, 0, true);
                mesh.UploadMeshData(true);
                return mesh;
            }
            catch
            {
                DestroyPreview(mesh);
                throw;
            }
        }

        private Visual AcquireVisual()
        {
            if (visualPool.Count != 0)
            {
                var pooled = visualPool.Pop();
                pooled.Object.SetActive(true);
                return pooled;
            }

            var obj = new GameObject("PVD Shape") { hideFlags = HideFlags.HideAndDontSave };
            obj.transform.SetParent(visualRoot.transform, false);
            var filter = obj.AddComponent<MeshFilter>();
            var renderer = obj.AddComponent<MeshRenderer>();
            renderer.shadowCastingMode = ShadowCastingMode.Off;
            renderer.receiveShadows = false;
            renderer.lightProbeUsage = LightProbeUsage.Off;
            renderer.reflectionProbeUsage = ReflectionProbeUsage.Off;
            return new Visual { Object = obj, Filter = filter, Renderer = renderer, Transform = obj.transform };
        }

        private Material PreviewMaterial()
        {
            if (materialOverride != null)
                return materialOverride;
            if (defaultMaterial == null)
            {
                var shader = Resources.Load<Shader>("PvdPlaybackPreview");
                if (shader == null)
                    throw new InvalidOperationException("The PVD preview shader is missing. Assign a Preview Material or reinstall the package.");

                defaultMaterial = new Material(shader)
                {
                    name = "PVD Preview",
                    hideFlags = HideFlags.HideAndDontSave
                };
            }
            return defaultMaterial;
        }

        private void ResetPlayback()
        {
            if (worker != null)
            {
                previousWorker = worker.Stop();
                worker = null;
            }
            previousTick = Stopwatch.GetTimestamp();
            playbackError = listenerError = "";
            IsLive = IsFollowingLive = false;
            LiveState = PvdLiveState.Idle;
            BoundPort = 0;
            ReceivedBytes = 0;
            availablePreviewEventCount = requestedPreviewEventCount = PreviewEventCount = 0;
            IsPlaying = IsLoading = false;
            CurrentFrame = RequestedFrame = -1;
            FrameCount = 0;
            playbackCursor = 0;
            ReleaseVisuals();
        }

        private void ReleaseVisuals()
        {
            if (visualRoot != null)
            {
                visualRoot.SetActive(false);
                DestroyPreview(visualRoot);
                visualRoot = null;
            }

            visuals.Clear();
            visualPool.Clear();
            removed.Clear();
            foreach (var mesh in meshes.Values)
                DestroyPreview(mesh);
            meshes.Clear();
            DestroyPreview(defaultMaterial);
            defaultMaterial = null;
            appliedMaterialOverride = null;
        }

        private static float ValidFrameRate(float value)
        {
            return float.IsNaN(value) || float.IsInfinity(value) ? 60f : Mathf.Max(0.01f, value);
        }

        private static void DestroyPreview(UnityEngine.Object value)
        {
            if (value == null)
                return;
            if (Application.isPlaying)
                Destroy(value);
            else
                DestroyImmediate(value);
        }

        private void Notify()
        {
#if UNITY_EDITOR
            if (!Application.IsPlaying(gameObject) || UnityEditor.EditorApplication.isPaused)
            {
                UnityEditor.EditorApplication.QueuePlayerLoopUpdate();
                UnityEditor.SceneView.RepaintAll();
            }
#endif
            StateChanged?.Invoke();
        }
    }
}
