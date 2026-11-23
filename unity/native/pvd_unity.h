#ifndef PVD_UNITY_H
#define PVD_UNITY_H

#include <stdint.h>

#if defined(_WIN32) && defined(PVD_UNITY_BUILD)
#define PU_API __declspec(dllexport)
#elif defined(_WIN32)
#define PU_API __declspec(dllimport)
#else
#define PU_API
#endif

#if defined(_WIN32)
#define PU_CALL __cdecl
#else
#define PU_CALL
#endif

#ifdef __cplusplus
#define PU_NOEXCEPT noexcept
extern "C" {
#else
#define PU_NOEXCEPT
#endif

typedef struct pu_instance {
    uint64_t scene_id, scene_generation;
    uint64_t actor_id, actor_generation;
    uint64_t shape_id, shape_generation;
    uint32_t mesh_id;
    float px, py, pz;
    float qx, qy, qz, qw;
} pu_instance;

typedef struct pu_vertex {
    float px, py, pz;
    float nx, ny, nz;
} pu_vertex;

/* Version 3 adds safe-prefix live previews to the TCP source API. */
PU_API uint32_t PU_CALL pu_abi_version(void) PU_NOEXCEPT;

/* Calls on a session must be serialized, including close. Counts passed to
   copy functions must match the corresponding queries. Mesh IDs start at 1
   and are valid for the session lifetime. Poses and vertices use native PhysX
   coordinates. Integer results are 0 on success and -1 on failure. */
PU_API int PU_CALL pu_open(const char *utf8_path, void **session) PU_NOEXCEPT;
/* Numeric IPv4/IPv6 address. Port 0 requests a system-assigned port. The
   listener accepts one connection. Use a new session to listen again. */
PU_API int PU_CALL pu_listen(const char *address, uint16_t port, void **session) PU_NOEXCEPT;
/* Import a bounded prefix and report its playable frame count. State values
   are idle (0), listening (1), receiving (2), completed (3), cancelled (4),
   truncated (5), failed (6). Terminal states are reported after import drains. */
PU_API int PU_CALL pu_get_live_status(void *session, uint32_t *state, uint32_t *frame_count,
                                      uint16_t *bound_port, uint64_t *wire_bytes) PU_NOEXCEPT;
/* Before the first completed live frame, return the latest imported safe
   event prefix, or 0 if none. Status polling updates this count. */
PU_API int PU_CALL pu_get_live_preview_event_count(void *session, uint64_t *event_count) PU_NOEXCEPT;
/* Prepare a previously reported preview prefix for pu_copy_instances without
   adding a frame to the timeline. A preview remains readable after disconnect. */
PU_API int PU_CALL pu_prepare_live_preview(void *session, uint64_t event_count,
                                            uint32_t *instance_count) PU_NOEXCEPT;
/* Stop receiving and release the socket. Poll until terminal to drain safe
   events already received. Imported frames remain available until close. */
PU_API int PU_CALL pu_stop_live(void *session) PU_NOEXCEPT;
/* Transport error, if available. Copy before the next status call or close. */
PU_API const char *PU_CALL pu_live_error(void *session) PU_NOEXCEPT;
PU_API void PU_CALL pu_close(void *session) PU_NOEXCEPT;
PU_API int PU_CALL pu_get_frame_count(void *session, uint32_t *count) PU_NOEXCEPT;
PU_API int PU_CALL pu_prepare_frame(void *session, uint32_t frame, uint32_t *instance_count) PU_NOEXCEPT;
PU_API int PU_CALL pu_copy_instances(void *session, pu_instance *instances, uint32_t count) PU_NOEXCEPT;
PU_API int PU_CALL pu_get_mesh_counts(void *session, uint32_t mesh_id,
                                     uint32_t *vertex_count, uint32_t *index_count) PU_NOEXCEPT;
PU_API int PU_CALL pu_copy_mesh(void *session, uint32_t mesh_id, pu_vertex *vertices,
                               uint32_t vertex_count, uint32_t *indices, uint32_t index_count) PU_NOEXCEPT;

/* Copy the thread-local message immediately after a failed call. */
PU_API const char *PU_CALL pu_last_error(void) PU_NOEXCEPT;

#ifdef __cplusplus
}
#endif

#endif
