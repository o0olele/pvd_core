// Compile-only ABI oracle against original SDK declarations; no pvd_core
// include and no SDK implementation/library is needed. Build x64 and Win32.
#include "PsMutex.h"
#include "NpPvdSceneQueryCollector.h"
#include <cstddef>
using namespace physx;
using namespace physx::Vd;
constexpr std::size_t width = sizeof(void *);
static_assert(width == 4 || width == 8, "source pointer width");
static_assert(sizeof(PxFilterData) == 16 && sizeof(PxTransform) == 28,
              "SDK auxiliary native layouts");
static_assert(sizeof(PvdReference) == (width == 8 ? 16 : 12),
              "PvdReference stride");
static_assert(offsetof(PvdReference, mBaseIndex) == width &&
                  offsetof(PvdReference, mCount) == width + 4,
              "PvdReference offsets");
static_assert(sizeof(PvdRaycast) == (width == 8 ? 72 : 64),
              "raycast native stride");
static_assert(offsetof(PvdRaycast, mType) == 0 &&
                  offsetof(PvdRaycast, mFilterData) == 4 &&
                  offsetof(PvdRaycast, mFilterFlags) == 20 &&
                  offsetof(PvdRaycast, mOrigin) == 24 &&
                  offsetof(PvdRaycast, mUnitDir) == 36 &&
                  offsetof(PvdRaycast, mDistance) == 48 &&
                  offsetof(PvdRaycast, mHits) == (width == 8 ? 56 : 52),
              "raycast native offsets");
static_assert(sizeof(PvdOverlap) == (width == 8 ? 88 : 76),
              "overlap native stride");
static_assert(offsetof(PvdOverlap, mType) == 0 &&
                  offsetof(PvdOverlap, mFilterData) == 4 &&
                  offsetof(PvdOverlap, mFilterFlags) == 20 &&
                  offsetof(PvdOverlap, mPose) == 24 &&
                  offsetof(PvdOverlap, mGeometries) == (width == 8 ? 56 : 52) &&
                  offsetof(PvdOverlap, mHits) == (width == 8 ? 72 : 64),
              "overlap native offsets");
static_assert(sizeof(PvdSweep) == (width == 8 ? 88 : 72),
              "sweep native stride");
static_assert(offsetof(PvdSweep, mType) == 0 &&
                  offsetof(PvdSweep, mFilterFlags) == 4 &&
                  offsetof(PvdSweep, mUnitDir) == 8 &&
                  offsetof(PvdSweep, mDistance) == 20 &&
                  offsetof(PvdSweep, mGeometries) == 24 &&
                  offsetof(PvdSweep, mPoses) == (width == 8 ? 40 : 36) &&
                  offsetof(PvdSweep, mFilterData) == (width == 8 ? 56 : 48) &&
                  offsetof(PvdSweep, mHits) == (width == 8 ? 72 : 60),
              "sweep native offsets");
static_assert(sizeof(PvdSqHit) == (width == 8 ? 64 : 52), "hit native stride");
static_assert(offsetof(PvdSqHit, mShape) == 0 &&
                  offsetof(PvdSqHit, mActor) == width &&
                  offsetof(PvdSqHit, mFaceIndex) == 2 * width &&
                  offsetof(PvdSqHit, mFlags) == 2 * width + 4 &&
                  offsetof(PvdSqHit, mImpact) == 2 * width + 8 &&
                  offsetof(PvdSqHit, mNormal) == 2 * width + 20 &&
                  offsetof(PvdSqHit, mDistance) == 2 * width + 32 &&
                  offsetof(PvdSqHit, mU) == 2 * width + 36 &&
                  offsetof(PvdSqHit, mV) == 2 * width + 40,
              "hit native offsets");
