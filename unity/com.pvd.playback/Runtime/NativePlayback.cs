using System;
using System.Buffers;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

namespace Pvd.Playback
{
    public enum PvdLiveState
    {
        Idle = 0,
        Listening = 1,
        Receiving = 2,
        Completed = 3,
        Stopped = 4,
        Truncated = 5,
        Failed = 6
    }

    internal static class NativePlayback
    {
        private const string Library = "pvd_unity_v3";
        private const uint ExpectedAbiVersion = 3;

        [StructLayout(LayoutKind.Sequential)]
        internal struct Instance : IEquatable<Instance>
        {
            public ulong SceneId, SceneGeneration, ActorId, ActorGeneration, ShapeId, ShapeGeneration;
            public uint MeshId;
            public float Px, Py, Pz, Qx, Qy, Qz, Qw;

            public bool Equals(Instance other)
            {
                return SceneId == other.SceneId && SceneGeneration == other.SceneGeneration &&
                       ActorId == other.ActorId && ActorGeneration == other.ActorGeneration &&
                       ShapeId == other.ShapeId && ShapeGeneration == other.ShapeGeneration;
            }

            public override bool Equals(object obj) => obj is Instance other && Equals(other);

            public override int GetHashCode()
            {
                unchecked
                {
                    int hash = SceneId.GetHashCode();
                    hash = hash * 397 ^ SceneGeneration.GetHashCode();
                    hash = hash * 397 ^ ActorId.GetHashCode();
                    hash = hash * 397 ^ ActorGeneration.GetHashCode();
                    hash = hash * 397 ^ ShapeId.GetHashCode();
                    return hash * 397 ^ ShapeGeneration.GetHashCode();
                }
            }
        }

        [StructLayout(LayoutKind.Sequential)]
        internal struct Vertex
        {
            public float Px, Py, Pz, Nx, Ny, Nz;
        }

        internal sealed class MeshData
        {
            public uint Id;
            public Vertex[] Vertices;
            public int[] Indices;
        }

        internal struct LiveStatus
        {
            public PvdLiveState State;
            public int FrameCount;
            public int BoundPort;
            public ulong ReceivedBytes;
            public ulong PreviewEventCount;
            public string Error;

            public bool IsTerminal => State >= PvdLiveState.Completed;
        }

        [DllImport(Library, EntryPoint = "pu_abi_version", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
        private static extern uint GetAbiVersionNative();

        [DllImport(Library, EntryPoint = "pu_listen", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
        private static extern int ListenNative([In] byte[] address, ushort port, out IntPtr session);

        [DllImport(Library, EntryPoint = "pu_get_live_status", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
        private static extern int GetLiveStatusNative(IntPtr session, out uint state, out uint frameCount,
            out ushort boundPort, out ulong receivedBytes);

        [DllImport(Library, EntryPoint = "pu_get_live_preview_event_count", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
        private static extern int GetLivePreviewEventCountNative(IntPtr session, out ulong eventCount);

        [DllImport(Library, EntryPoint = "pu_prepare_live_preview", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
        private static extern int PrepareLivePreviewNative(IntPtr session, ulong eventCount, out uint instanceCount);

        [DllImport(Library, EntryPoint = "pu_stop_live", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
        private static extern int StopLiveNative(IntPtr session);

        [DllImport(Library, EntryPoint = "pu_live_error", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
        private static extern IntPtr LiveErrorNative(IntPtr session);

        [DllImport(Library, EntryPoint = "pu_open", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
        private static extern int OpenNative([In] byte[] path, out IntPtr session);

        [DllImport(Library, EntryPoint = "pu_close", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
        internal static extern void Close(IntPtr session);

        [DllImport(Library, EntryPoint = "pu_get_frame_count", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
        private static extern int GetFrameCountNative(IntPtr session, out uint count);

        [DllImport(Library, EntryPoint = "pu_prepare_frame", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
        private static extern int PrepareNative(IntPtr session, uint frame, out uint count);

        [DllImport(Library, EntryPoint = "pu_copy_instances", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
        private static extern int CopyInstancesNative(IntPtr session, [Out] Instance[] output, uint count);

        [DllImport(Library, EntryPoint = "pu_get_mesh_counts", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
        private static extern int GetMeshCountsNative(IntPtr session, uint mesh, out uint vertices, out uint indices);

        [DllImport(Library, EntryPoint = "pu_copy_mesh", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
        private static extern int CopyMeshNative(IntPtr session, uint mesh, [Out] Vertex[] vertices, uint vertexCount,
            [Out] int[] indices, uint indexCount);

        [DllImport(Library, EntryPoint = "pu_last_error", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)]
        private static extern IntPtr LastErrorNative();

        internal static IntPtr Open(string path)
        {
            VerifyAbi();
            Check(OpenNative(Encoding.UTF8.GetBytes(path + "\0"), out var session));
            return session;
        }

        internal static IntPtr Listen(string address, ushort port)
        {
            VerifyAbi();
            Check(ListenNative(Encoding.UTF8.GetBytes(address + "\0"), port, out var session));
            return session;
        }

        internal static LiveStatus GetLiveStatus(IntPtr session)
        {
            Check(GetLiveStatusNative(session, out var state, out var frameCount, out var boundPort, out var receivedBytes));
            var result = new LiveStatus
            {
                State = (PvdLiveState)state,
                FrameCount = checked((int)frameCount),
                BoundPort = boundPort,
                ReceivedBytes = receivedBytes,
                Error = ""
            };
            if (result.FrameCount == 0)
                Check(GetLivePreviewEventCountNative(session, out result.PreviewEventCount));
            if (result.State == PvdLiveState.Failed || result.State == PvdLiveState.Truncated)
                result.Error = ReadUtf8(LiveErrorNative(session));
            return result;
        }

        internal static void StopLive(IntPtr session) => Check(StopLiveNative(session));

        internal static int FrameCount(IntPtr session)
        {
            Check(GetFrameCountNative(session, out var count));
            return checked((int)count);
        }

        internal static int PrepareFrame(IntPtr session, int frame)
        {
            Check(PrepareNative(session, checked((uint)frame), out var count));
            return checked((int)count);
        }

        internal static int PrepareLivePreview(IntPtr session, ulong eventCount)
        {
            Check(PrepareLivePreviewNative(session, eventCount, out var count));
            return checked((int)count);
        }

        internal static void CopyInstances(IntPtr session, Instance[] instances, int count)
        {
            if (count != 0)
                Check(CopyInstancesNative(session, instances, checked((uint)count)));
        }

        internal static MeshData ReadMesh(IntPtr session, uint id, bool mirrorZ)
        {
            Check(GetMeshCountsNative(session, id, out var vertexCount, out var indexCount));
            var vertices = new Vertex[checked((int)vertexCount)];
            var indices = new int[checked((int)indexCount)];
            Check(CopyMeshNative(session, id, vertices, vertexCount, indices, indexCount));
            if (mirrorZ)
            {
                for (int i = 0; i < vertices.Length; ++i)
                {
                    vertices[i].Pz = -vertices[i].Pz;
                    vertices[i].Nz = -vertices[i].Nz;
                }

                for (int i = 0; i < indices.Length; i += 3)
                {
                    int second = indices[i + 1];
                    indices[i + 1] = indices[i + 2];
                    indices[i + 2] = second;
                }
            }

            return new MeshData { Id = id, Vertices = vertices, Indices = indices };
        }

        private static void VerifyAbi()
        {
            uint version = GetAbiVersionNative();
            if (version != ExpectedAbiVersion)
                throw new InvalidOperationException(
                    $"pvd_unity_v3.dll reports native ABI {version}. This package requires ABI {ExpectedAbiVersion}. " +
                    "Reinstall the built PVD Playback package and restart Unity.");
        }

        private static void Check(int status)
        {
            if (status == 0)
                return;

            string error = ReadUtf8(LastErrorNative());
            throw new InvalidOperationException(error.Length == 0 ? "The native PVD operation failed." : error);
        }

        private static string ReadUtf8(IntPtr pointer)
        {
            if (pointer == IntPtr.Zero)
                return "";

            int length = 0;
            while (Marshal.ReadByte(pointer, length) != 0)
                ++length;

            var bytes = new byte[length];
            Marshal.Copy(pointer, bytes, 0, length);
            return Encoding.UTF8.GetString(bytes);
        }
    }

    internal sealed class PlaybackWorker
    {
        internal sealed class Result : IDisposable
        {
            public int FrameCount;
            public int Frame = -1;
            public ulong PreviewEventCount;
            public int InstanceCount;
            public NativePlayback.Instance[] Instances;
            public readonly List<NativePlayback.MeshData> Meshes = new List<NativePlayback.MeshData>();
            public string Error;

            public void Dispose()
            {
                if (Instances != null && Instances.Length != 0)
                    ArrayPool<NativePlayback.Instance>.Shared.Return(Instances);
                Instances = null;
            }
        }

        private struct FrameRequest
        {
            public int Frame;
            public ulong PreviewEventCount;
            public long Revision;
            public bool MirrorZ;
            public bool StopListening;
        }

        private readonly object gate = new object();
        private readonly HashSet<uint> uploadedMeshes = new HashSet<uint>();
        private readonly string source;
        private readonly ushort? listenPort;
        private readonly Thread thread;
        private readonly Thread predecessor;
        private NativePlayback.LiveStatus? liveStatus;
        private Result ready;
        private bool closed;
        private bool pending;
        private bool stopListening;
        private bool mirrorZ;
        private int targetFrame;
        private ulong targetPreviewEventCount;
        private long requestRevision;

        internal PlaybackWorker(string path, int firstFrame, bool mirrorZ, Thread previous = null)
            : this(path, firstFrame, null, mirrorZ, previous) { }

        internal PlaybackWorker(string address, ushort port, bool mirrorZ, Thread previous = null)
            : this(address, -1, port, mirrorZ, previous) { }

        private PlaybackWorker(string source, int firstFrame, ushort? listenPort, bool mirrorZ, Thread previous)
        {
            this.source = source;
            this.listenPort = listenPort;
            targetFrame = firstFrame;
            pending = firstFrame >= 0;
            this.mirrorZ = mirrorZ;
            predecessor = previous;
            thread = new Thread(Run) { IsBackground = true, Name = "PVD playback" };
            thread.Start();
        }

        internal void Request(int frame)
        {
            lock (gate)
            {
                SetRequest(frame, 0);
            }
        }

        internal void RequestPreview(ulong eventCount)
        {
            lock (gate)
            {
                SetRequest(-1, eventCount);
            }
        }

        internal void ChangeOrientation(bool mirrored, int frame, ulong previewEventCount = 0)
        {
            lock (gate)
            {
                mirrorZ = mirrored;
                uploadedMeshes.Clear();
                SetRequest(frame, previewEventCount);
            }
        }

        private void SetRequest(int frame, ulong previewEventCount)
        {
            targetFrame = frame;
            targetPreviewEventCount = previewEventCount;
            ++requestRevision;
            pending = frame >= 0 || previewEventCount != 0;
            ready?.Dispose();
            ready = null;
            Monitor.Pulse(gate);
        }

        internal void StopListening()
        {
            lock (gate)
            {
                stopListening = true;
                Monitor.Pulse(gate);
            }
        }

        internal NativePlayback.LiveStatus? TakeLiveStatus()
        {
            lock (gate)
            {
                var status = liveStatus;
                liveStatus = null;
                return status;
            }
        }

        internal Result Take()
        {
            lock (gate)
            {
                var value = ready;
                ready = null;
                return value;
            }
        }

        internal void AcknowledgeMeshes(List<NativePlayback.MeshData> meshes)
        {
            lock (gate)
                foreach (var mesh in meshes)
                    uploadedMeshes.Add(mesh.Id);
        }

        internal Thread Stop()
        {
            lock (gate)
            {
                closed = true;
                ready?.Dispose();
                ready = null;
                Monitor.Pulse(gate);
            }
            return thread;
        }

        private void Run()
        {
            var session = IntPtr.Zero;
            try
            {
                // Closing an earlier source can still be releasing its socket.
                predecessor?.Join();
                lock (gate)
                    if (closed)
                        return;

                bool isLive = listenPort.HasValue;
                session = isLive ? NativePlayback.Listen(source, listenPort.Value) : NativePlayback.Open(source);
                int frameCount = isLive ? 0 : NativePlayback.FrameCount(session);
                if (!isLive && frameCount == 0)
                    throw new InvalidOperationException("The capture has no replay frames.");

                bool pollLive = isLive;
                if (pollLive)
                    pollLive = PollLive(session, out frameCount);

                var requestedMeshes = new HashSet<uint>();
                while (WaitForWork(pollLive) is FrameRequest request)
                {
                    if (request.StopListening && pollLive)
                        NativePlayback.StopLive(session);
                    if (pollLive)
                        pollLive = PollLive(session, out frameCount);
                    if ((request.Frame >= 0 && frameCount != 0) || request.PreviewEventCount != 0)
                        BuildFrame(session, frameCount, request, requestedMeshes);
                }
            }
            catch (Exception error)
            {
                lock (gate)
                {
                    if (!closed)
                    {
                        ready?.Dispose();
                        ready = new Result { Error = Describe(error) };
                    }
                }
            }
            finally
            {
                if (session != IntPtr.Zero)
                    NativePlayback.Close(session);
            }
        }

        private bool PollLive(IntPtr session, out int frameCount)
        {
            var status = NativePlayback.GetLiveStatus(session);
            frameCount = status.FrameCount;
            lock (gate)
            {
                if (!closed)
                    liveStatus = status;
            }

            return !status.IsTerminal;
        }

        private void BuildFrame(IntPtr session, int frameCount, FrameRequest request, HashSet<uint> requestedMeshes)
        {
            var result = new Result
            {
                Frame = Math.Min(frameCount - 1, request.Frame),
                FrameCount = frameCount,
                PreviewEventCount = request.PreviewEventCount
            };
            try
            {
                result.InstanceCount = request.PreviewEventCount == 0
                    ? NativePlayback.PrepareFrame(session, result.Frame)
                    : NativePlayback.PrepareLivePreview(session, request.PreviewEventCount);
                result.Instances = result.InstanceCount == 0
                    ? Array.Empty<NativePlayback.Instance>()
                    : ArrayPool<NativePlayback.Instance>.Shared.Rent(result.InstanceCount);
                NativePlayback.CopyInstances(session, result.Instances, result.InstanceCount);

                requestedMeshes.Clear();
                for (int i = 0; i < result.InstanceCount; ++i)
                    requestedMeshes.Add(result.Instances[i].MeshId);

                foreach (var meshId in requestedMeshes)
                {
                    lock (gate)
                    {
                        if (closed || requestRevision != request.Revision)
                            break;
                        if (uploadedMeshes.Contains(meshId))
                            continue;
                    }
                    result.Meshes.Add(NativePlayback.ReadMesh(session, meshId, request.MirrorZ));
                }
            }
            catch (Exception error)
            {
                result.Error = Describe(error);
            }

            lock (gate)
            {
                if (!closed && requestRevision == request.Revision)
                    ready = result;
                else
                    result.Dispose();
            }
        }

        private FrameRequest? WaitForWork(bool pollLive)
        {
            lock (gate)
            {
                while (!closed && !pending && !stopListening)
                {
                    if (pollLive)
                    {
                        Monitor.Wait(gate, 16);
                        break;
                    }

                    Monitor.Wait(gate);
                }

                if (closed)
                    return null;

                var request = new FrameRequest
                {
                    Frame = pending ? targetFrame : -1,
                    PreviewEventCount = pending ? targetPreviewEventCount : 0,
                    Revision = requestRevision,
                    MirrorZ = mirrorZ,
                    StopListening = stopListening
                };
                pending = stopListening = false;
                return request;
            }
        }

        private static string Describe(Exception error)
        {
            if (error is DllNotFoundException)
                return "Unity could not load pvd_unity_v3.dll. Install the built PVD Playback package for Windows x64 " +
                       "and verify that the DLL is enabled for this platform in the Plugin Inspector. " + error.Message;
            if (error is EntryPointNotFoundException)
                return "The loaded pvd_unity_v3.dll lacks a required native entry point. Reinstall the built " +
                       "PVD Playback package and restart Unity. " + error.Message;
            if (error is BadImageFormatException)
                return "Windows could not use pvd_unity_v3.dll as a Windows x64 library. " +
                       "Reinstall the built Windows x64 PVD Playback package. " + error.Message;
            return error.Message;
        }
    }
}
