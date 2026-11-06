/*
 * SGK Grass 0.9 - stable, prewarmed world-space vegetation, SoH 9.3.1.
 * TriAevum is the architecture reference; no Vulkan source is copied.
 * Stable high-resolution world-space blades, conservative camera frustum,
 * overlapping distance LOD and smoothed actor bending.
 * SGK 0.7 keeps elevation-independent sampling and adds conservative material checks.
 * Vertical camera culling now uses true cached terrain height and conservative
 * chunk bounds; foliage no longer resets just because Link changes elevation.
 * Grass height alone is reduced by default; width/density/spacing unchanged.
 * Eight-way bucketed collision cache, fixed world-space thinning, low-amplitude
 * wind, pre-warmed offscreen collision cells, stable tips at LOD boundaries.
 * Does not replace textures, OTR/O2R mods or character models.
 * Only collision surfaces tagged grass are eligible, checked at three points.
 * This DOES NOT guarantee pixel-perfect discrimination of grass textures vs paths
 * when the original map uses the same collision polygon/sound for both.
 */
#include "global.h"
#include "soh/Enhancements/SGK/SGKGrass.h"
#include <math.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <libultraship/bridge/consolevariablebridge.h>

#define SGK_CACHE_CAPACITY 65536u
#define SGK_CACHE_WAYS 8u
#define SGK_CACHE_BUCKETS (SGK_CACHE_CAPACITY / SGK_CACHE_WAYS)
#define SGK_CACHE_BUCKET_MASK (SGK_CACHE_BUCKETS - 1u)
#define SGK_BATCH_BLADES 10u /* 10 triangles = 30 vertices; N64 Fast3D limit is 32 */
#define SGK_FLOOR_GRASS_SFX (NA_SE_PL_WALK_GRASS - SFX_FLAG)
#define SGK_PI 3.14159265358979323846f
#define SGK_MAX_INFLUENCES 24
#define SGK_CHUNK_SIDE 6 /* group nearby cells before collision queries */
#define SGK_VERTEX_PRECISION 4.0f /* 0.25-world-unit precision in camera-local coordinates */
#define SGK_VERTEX_SCALE (1.0f / SGK_VERTEX_PRECISION)
#define SGK_NEW_CELL_FADE_FRAMES 12u
#define SGK_PREFETCH_SAMPLES_PER_TIER 24u
#define SGK_PREFETCH_PERIOD 8192u

typedef struct SGKCell {
    s32 gridX, gridZ, layer;
    u32 seed;
    f32 y;
    s16 normalX, normalY, normalZ;
    u8 tier, valid, occupied;
    f32 pushX, pushZ; /* smoothed bend from actors */
    u32 lastBendFrame;
    u32 bornFrame;   /* age-based smooth reveal: no instantaneous cache insertion */
    u32 lastUsedFrame; /* LRU for associative collision cache */
    f32 bendVelX, bendVelZ; /* damped spring, persists with the cell */
} SGKCell;

typedef struct SGKTier {
    s32 spacing;
    s32 innerRadius;
    s32 outerRadius;
    u8 bladesPerCell;
} SGKTier;

/* Quality 0 = economy, 1 = balanced (default), 2 = dense.
 * The distances are N64 world units; each ring uses a stable world grid.
 */
static const SGKTier sTierSettings[3][3] = {
    { { 15,    0,  375, 7 }, { 32,  265,  845, 4 }, { 62,  680, 1500, 2 } },
    { { 10,    0,  485,11 }, { 23,  340, 1250, 6 }, { 56,  970, 2050, 3 } },
    { {  9,    0,  550,13 }, { 20,  395, 1550, 7 }, { 52, 1150, 2450, 3 } },
};

/* Long-range grass is intentionally sparse at the horizon for Intel UHD. */
static const SGKTier sFieldSettings[3][3] = {
    { { 15, 0, 425, 8 }, { 37, 310, 1250, 4 }, { 135, 1000, 3400, 2 } },
    { { 10, 0, 510, 13 }, { 28, 365, 1750, 7 }, { 115, 1400, 4800, 3 } },
    { {  9, 0, 590, 16 }, { 25, 425, 2120, 9 }, { 115, 1700, 5600, 4 } },
};
static SGKTier SGK_TierForScene(const PlayState* play, s32 quality, s32 tier) {
    return play->sceneNum == SCENE_HYRULE_FIELD ? sFieldSettings[quality][tier] : sTierSettings[quality][tier];
}
static f32 SGK_Clamp(f32 x, f32 lo, f32 hi);

static SGKCell sCells[SGK_CACHE_CAPACITY];
static s32 sCachedScene = -1;
static s32 sCachedQuality = -1;
/* This also detects a scene re-entered with the same numeric scene ID. */
static const CollisionHeader* sCachedCollisionHeader = NULL;
static u32 sPreviousGameplayFrame = 0u;
static u32 sFrameNumber = 0;
static u32 sCacheMisses = 0;
static u32 sDrawnBlades = 0;
static u32 sDrawnCells = 0;
static u32 sRaycastsThisFrame = 0;
static u32 sCulledChunks = 0;
static u32 sCulledCells = 0;
static u32 sConsideredCells = 0;
static u32 sNewCells = 0;
static u32 sFrameCachedCells = 0;
static u32 sCacheEvictions = 0;
static u32 sPrecacheRequests = 0;
static s32 sVersionLogged = 0;
static u32 sPrefetchCursor[3] = { 0u, 0u, 0u };
static u32 sTransitionWarmFrames = 0u;
/* A conservative scene-wide material preflight avoids scanning barren scenes
 * every frame (temples/stone/deserts without grass footstep polygons). */
static s32 sSceneHasGrass = -1;
/* CVar material policy is read once per frame, not for every cached cell. */
static s32 sEdgeCheck = 1;
static s32 sCachedEdgeCheck = -1;
/* Stable world origin for int16 Vtx coordinates, otherwise large maps overflow
 * the 0.25-unit precision model when |world position| exceeds 8190 units. */
static f32 sVertexOriginX = 0.0f;
static f32 sVertexOriginY = 0.0f;
static f32 sVertexOriginZ = 0.0f;

typedef struct SGKInfluence {
    f32 x, y, z;
    f32 vx, vz;
    f32 radius;
} SGKInfluence;
static SGKInfluence sInfluences[SGK_MAX_INFLUENCES];
static u32 sInfluenceCount = 0;

typedef struct SGKViewCone {
    f32 ex, ey, ez;
    f32 fx, fy, fz;
    f32 rx, rz;
    f32 ux, uy, uz;
    f32 tanHorizontal, tanVertical;
    s32 valid;
} SGKViewCone;
static SGKViewCone sView;

static void SGK_InitView(const View* view) {
    f32 fx = view->lookAt.x - view->eye.x;
    f32 fy = view->lookAt.y - view->eye.y;
    f32 fz = view->lookAt.z - view->eye.z;
    f32 len = sqrtf(fx * fx + fy * fy + fz * fz);
    f32 horiz, aspect, fovy;
    sView.valid = 0;
    if (len < 0.001f) return; /* do not accidentally cull everything */
    fx /= len; fy /= len; fz /= len;
    horiz = sqrtf(fx * fx + fz * fz);
    if (horiz < 0.04f) return; /* vertical camera: conservative fallback */
    sView.rx = fz / horiz;
    sView.rz = -fx / horiz;
    sView.uy = horiz;
    /* Cross(right, forward): upX = rightZ*forwardY, upZ = -rightX*forwardY. */
    sView.ux = sView.rz * fy;
    sView.uz = -sView.rx * fy;
    sView.ex = view->eye.x; sView.ey = view->eye.y; sView.ez = view->eye.z;
    sView.fx = fx; sView.fy = fy; sView.fz = fz;
    fovy = view->fovy;
    if (fovy < 20.0f || fovy > 120.0f) fovy = 60.0f;
    aspect = (f32)(view->viewport.rightX - view->viewport.leftX) /
             (f32)(view->viewport.bottomY - view->viewport.topY > 0 ?
                   view->viewport.bottomY - view->viewport.topY : 240);
    if (aspect < 1.0f) aspect = 4.0f / 3.0f;
    sView.tanVertical = tanf(fovy * (SGK_PI / 360.0f));
    /* 1.30 guards interpolation / actual widescreen aspect ratios. */
    sView.tanHorizontal = sView.tanVertical * aspect * 1.30f;
    sView.valid = 1;
}

/* A conservative sphere against the camera's four side planes.
 * When out of view, blades are NOT drawn, but cache and position remain intact.
 * Border adds safety against culling pop, slopes, wind and camera interpolation.
 */
static s32 SGK_Visible(f32 x, f32 y, f32 z, f32 radius) {
    f32 dx, dy, dz, forward, side, up;
    if (!sView.valid) return 1;
    radius += 85.0f; /* guard for camera motion and interpolation */
    dx = x - sView.ex; dy = y - sView.ey; dz = z - sView.ez;
    forward = dx * sView.fx + dy * sView.fy + dz * sView.fz;
    if (forward < -radius) return 0;
    side = fabsf(dx * sView.rx + dz * sView.rz);
    if (side > (forward > 0 ? forward : 0) * sView.tanHorizontal + radius) return 0;
    up = fabsf(dx * sView.ux + dy * sView.uy + dz * sView.uz);
    if (up > (forward > 0 ? forward : 0) * sView.tanVertical + radius) return 0;
    return 1;
}

static void SGK_AddInfluence(const Actor* a, f32 radius) {
    SGKInfluence* v;
    if (sInfluenceCount >= SGK_MAX_INFLUENCES || a == NULL) return;
    v = &sInfluences[sInfluenceCount++];
    v->x = a->world.pos.x; v->y = a->world.pos.y; v->z = a->world.pos.z;
    v->vx = a->world.pos.x - a->prevPos.x;
    v->vz = a->world.pos.z - a->prevPos.z;
    v->radius = radius;
}

/* Includes Link, Kokiri NPCs, enemies and bosses if present. Skips non-characters. */
static void SGK_CollectInfluences(PlayState* play) {
    const s32 cats[3] = { ACTORCAT_NPC, ACTORCAT_ENEMY, ACTORCAT_BOSS };
    s32 i;
    sInfluenceCount = 0;
    SGK_AddInfluence(&GET_PLAYER(play)->actor, 32.0f);
    for (i = 0; i < 3; ++i) {
        Actor* a = play->actorCtx.actorLists[cats[i]].head;
        s32 limit = play->actorCtx.actorLists[cats[i]].length;
        while (a != NULL && limit-- > 0 && sInfluenceCount < SGK_MAX_INFLUENCES) {
            if (a->update != NULL) SGK_AddInfluence(a, cats[i] == ACTORCAT_NPC ? 23.0f : 30.0f);
            a = a->next;
        }
    }
}

/* Persistent, smoothly relaxing tip displacement. No permanent geometry changes. */
static void SGK_UpdateBend(SGKCell* cell, f32 x, f32 y, f32 z, s32 enabled) {
    f32 tx = 0.0f, tz = 0.0f;
    u32 k, frames = cell->lastBendFrame ? (sFrameNumber - cell->lastBendFrame) : 1u;
    if (enabled) {
        for (k = 0; k < sInfluenceCount; ++k) {
            const SGKInfluence* a = &sInfluences[k];
            f32 dx = x - a->x, dz = z - a->z;
            f32 dist2 = dx * dx + dz * dz;
            if (fabsf(y - a->y) < 40.0f && dist2 < a->radius * a->radius) {
                f32 dist = sqrtf(dist2);
                f32 weight = 1.0f - dist / a->radius;
                f32 vx = dx * 0.65f + a->vx * 2.0f;
                f32 vz = dz * 0.65f + a->vz * 2.0f;
                f32 dirLen = sqrtf(vx * vx + vz * vz);
                if (dirLen < 0.001f) { vx = 0.0f; vz = 1.0f; dirLen = 1.0f; }
                tx += weight * weight * 10.0f * vx / dirLen;
                tz += weight * weight * 10.0f * vz / dirLen;
            }
        }
    }
    tx = SGK_Clamp(tx, -9.0f, 9.0f);
    tz = SGK_Clamp(tz, -9.0f, 9.0f);
    /* Damped spring. Stable when draw is skipped due to frustum culling.
     * A cell not touched for many frames relaxes instead of snapping back.
     */
    {
        u32 steps = frames < 6u ? frames : 6u;
        u32 t;
        if (frames > 6u && frames < 4096u) {
            f32 relax = powf(0.84f, (f32)(frames - 6u));
            cell->pushX *= relax; cell->pushZ *= relax;
            cell->bendVelX *= relax; cell->bendVelZ *= relax;
        }
        if (frames >= 4096u) {
            cell->pushX = cell->pushZ = 0.0f;
            cell->bendVelX = cell->bendVelZ = 0.0f;
        }
        if (steps == 0u) steps = 1u;
        for (t = 0u; t < steps; ++t) {
            cell->bendVelX = (cell->bendVelX + (tx - cell->pushX) * 0.14f) * 0.78f;
            cell->bendVelZ = (cell->bendVelZ + (tz - cell->pushZ) * 0.14f) * 0.78f;
            cell->pushX += cell->bendVelX;
            cell->pushZ += cell->bendVelZ;
        }
    }
    cell->lastBendFrame = sFrameNumber;
}


static u32 SGK_Hash(u32 n) {
    n ^= n >> 16;
    n *= 0x7feb352dU;
    n ^= n >> 15;
    n *= 0x846ca68bU;
    return n ^ (n >> 16);
}

static u32 SGK_Seed(s32 gx, s32 gz, s32 tier) {
    return SGK_Hash((u32)gx * 0x9e3779b9U ^ (u32)gz * 0x85ebca6bU ^
                    (u32)tier * 0xc2b2ae35U ^ 0x53a71f2bU);
}

static f32 SGK_Unit(u32 n) {
    return (f32)(n & 65535u) / 65535.0f;
}

static f32 SGK_Clamp(f32 x, f32 lo, f32 hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

/* 0 at the beginning, 1 at the end; zero derivative at both endpoints.
 * Useful for soft distance LOD and first-cache reveal without translucent overdraw.
 */
static f32 SGK_Smooth(f32 x) {
    x = SGK_Clamp(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

static s32 SGK_GridFloor(f32 x, s32 spacing) {
    return (s32)floorf(x / (f32)spacing);
}

static void SGK_ResetCache(s32 scene, s32 quality) {
    memset(sCells, 0, sizeof(sCells));
    sCachedScene = scene;
    sCachedQuality = quality;
    sCacheMisses = 0;
    sCacheEvictions = 0;
    memset(sPrefetchCursor, 0, sizeof(sPrefetchCursor));
    sTransitionWarmFrames = 0u;
    sSceneHasGrass = -1;
    sCachedEdgeCheck = sEdgeCheck;
}

static s32 SGK_IsUnderwater(PlayState* play, f32 x, f32 y, f32 z) {
    CollisionHeader* head = play->colCtx.colHeader;
    u32 i;
    if (head == NULL || head->waterBoxes == NULL) return 0;
    for (i = 0; i < head->numWaterBoxes; ++i) {
        WaterBox* w = &head->waterBoxes[i];
        if (x >= w->xMin && x < w->xMin + w->xLength &&
            z >= w->zMin && z < w->zMin + w->zLength &&
            y <= (f32)w->ySurface + 2.0f) return 1;
    }
    return 0;
}

/* A static polygon tagged with the grass footstep sound is a necessary
 * condition for SGK vegetation. No scene-name whitelist: this covers ANY
 * game region containing such polygons, without planting on flagged rock.
 * This scan runs only on scene reset and short-circuits on first match. */
static s32 SGK_SceneContainsGrass(PlayState* play) {
    CollisionHeader* head = play->colCtx.colHeader;
    u32 i;
    if (head == NULL || head->polyList == NULL) return 0;
    for (i = 0; i < (u32)head->numPolygons; ++i) {
        CollisionPoly* poly = &head->polyList[i];
        if (COLPOLY_GET_NORMAL(poly->normal.y) >= 0.72f &&
            SurfaceType_GetSfx(&play->colCtx, poly, BGCHECK_SCENE) == SGK_FLOOR_GRASS_SFX) {
            return 1;
        }
    }
    return 0;
}

/* Optional conservative edge verification: a path/rock bordering grass
 * should not inherit leaves merely because the center point lands on grass.
 * This still cannot split one visual polygon that shares the same grass
 * collision tag across grassy and path textures. */
static s32 SGK_GrassProbe(PlayState* play, f32 x, f32 z, f32 sourceY, f32 heightLimit) {
    Vec3f pos;
    CollisionPoly* poly = NULL;
    s32 bgId = BG_ACTOR_MAX;
    f32 y;
    pos.x = x;
    pos.y = SGK_Clamp(play->colCtx.maxBounds.y + 12.0f, -32000.0f, 32700.0f);
    pos.z = z;
    y = BgCheck_EntityRaycastFloor3(&play->colCtx, &poly, &bgId, &pos);
    ++sRaycastsThisFrame;
    return poly != NULL && bgId == BGCHECK_SCENE && y > BGCHECK_Y_MIN + 1.0f &&
           fabsf(y - sourceY) <= heightLimit &&
           COLPOLY_GET_NORMAL(poly->normal.y) >= 0.72f &&
           SurfaceType_GetSfx(&play->colCtx, poly, bgId) == SGK_FLOOR_GRASS_SFX &&
           !SGK_IsUnderwater(play, x, y, z);
}

/* Collision queries are spread over frames to avoid a long first-frame pause.
 * Null means a query has been deferred; occupied=0 is a completed, empty query.
 */
static SGKCell* SGK_GetCell(PlayState* play, s32 gx, s32 gz, s32 tier,
                            s32 layer, s32 spacing, f32 playerY, u32 raycastBudget) {
    /* Kept as a parameter for callers, but never used for geometry classification.
     * Otherwise a player climbing a ladder invalidates the distant field.
     */
    (void)playerY;
    u32 seed = SGK_Seed(gx, gz, tier);
    s32 useEdgeCheck = sEdgeCheck;
    u32 raycastCost = useEdgeCheck ? 3u : 1u;
    /* One slot per hash in v0.4 could replace visible plants every frame.
     * 8-way bucket search retains nearby colliding keys simultaneously.
     * Choose an unused slot, otherwise evict least recently used.
     */
    u32 bucket = (SGK_Hash(seed ^ (u32)layer * 0x27d4eb2dU) &
                  SGK_CACHE_BUCKET_MASK) * SGK_CACHE_WAYS;
    SGKCell* cell = NULL;
    SGKCell* oldest = NULL;
    u32 oldestAge = 0u;
    u32 way;
    for (way = 0; way < SGK_CACHE_WAYS; ++way) {
        SGKCell* entry = &sCells[bucket + way];
        if (entry->valid && entry->gridX == gx && entry->gridZ == gz &&
            entry->tier == tier && entry->layer == layer) {
            entry->lastUsedFrame = sFrameNumber;
            return entry;
        }
        if (!entry->valid) { if (cell == NULL) cell = entry; continue; }
        {
            u32 age = sFrameNumber - entry->lastUsedFrame;
            if (oldest == NULL || age > oldestAge) { oldest = entry; oldestAge = age; }
        }
    }
    if (cell == NULL) cell = oldest;
    f32 x, z, floorY;
    Vec3f pos;
    CollisionPoly* poly = NULL;
    s32 bgId = BG_ACTOR_MAX;

    if (sRaycastsThisFrame + raycastCost > raycastBudget) return NULL;
    if (cell->valid) ++sCacheEvictions;

    cell->valid = 1;
    cell->lastUsedFrame = sFrameNumber;
    cell->occupied = 0;
    cell->gridX = gx;
    cell->gridZ = gz;
    cell->tier = (u8)tier;
    cell->layer = layer;
    cell->seed = seed;
    cell->pushX = 0.0f; cell->pushZ = 0.0f;
    cell->bendVelX = 0.0f; cell->bendVelZ = 0.0f;
    cell->lastBendFrame = 0;
    cell->bornFrame = sFrameNumber;
    ++sNewCells;
    ++sCacheMisses;
    ++sRaycastsThisFrame;

    /* The same grid cell always selects the same position. */
    x = ((f32)gx + 0.15f + 0.70f * SGK_Unit(SGK_Hash(seed ^ 0xa77u))) * spacing;
    z = ((f32)gz + 0.15f + 0.70f * SGK_Unit(SGK_Hash(seed ^ 0xb91u))) * spacing;
    pos.x = x;
    /* A fixed scene-relative top-down ray means the SAME world cell always
     * produces the SAME ground, regardless of Link's height. Previously a
     * ray starting at playerY+210 and a |floorY-playerY| test poisoned
     * cached cells when climbing ladders or stairs.
     */
    pos.y = SGK_Clamp(play->colCtx.maxBounds.y + 12.0f, -32000.0f, 32700.0f);
    pos.z = z;
    floorY = BgCheck_EntityRaycastFloor3(&play->colCtx, &poly, &bgId, &pos);
    if (poly == NULL || bgId != BGCHECK_SCENE || floorY <= BGCHECK_Y_MIN + 1.0f) return cell;
    if (COLPOLY_GET_NORMAL(poly->normal.y) < 0.72f) return cell;
    if (SurfaceType_GetSfx(&play->colCtx, poly, bgId) != SGK_FLOOR_GRASS_SFX) return cell;
    if (SGK_IsUnderwater(play, x, floorY, z)) return cell;
    if (useEdgeCheck) {
        /* Stable opposite diagonals, independent of camera or player direction.
         * Probe offsets remain inside the cell in all quality tiers. */
        f32 offset = (f32)spacing * 0.20f;
        f32 sx = (seed & 1u) ? offset : -offset;
        f32 sz = (seed & 2u) ? offset : -offset;
        f32 maxHeightStep = SGK_Clamp((f32)spacing * 0.70f, 9.0f, 40.0f);
        if (!SGK_GrassProbe(play, x + sx, z + sz, floorY, maxHeightStep) ||
            !SGK_GrassProbe(play, x - sx, z - sz, floorY, maxHeightStep)) {
            return cell;
        }
    }

    cell->y = floorY + 1.10f;
    cell->normalX = poly->normal.x;
    cell->normalY = poly->normal.y;
    cell->normalZ = poly->normal.z;
    cell->occupied = 1;
    return cell;
}


/* Populate collision data outside the camera using spare, bounded CPU budget.
 * Rendering is still strictly frustum-culled. After several stationary frames,
 * turning around usually finds a ready cache instead of bare ground.
 * Deterministic golden-angle samples distribute work across all directions.
 */
/* Van der Corput sequence distributes prewarming across the entire ring. */
static f32 SGK_RadicalInverse(u32 x) {
    x = (x << 16u) | (x >> 16u);
    x = ((x & 0x00ff00ffu) << 8u) | ((x & 0xff00ff00u) >> 8u);
    x = ((x & 0x0f0f0f0fu) << 4u) | ((x & 0xf0f0f0f0u) >> 4u);
    x = ((x & 0x33333333u) << 2u) | ((x & 0xccccccccu) >> 2u);
    x = ((x & 0x55555555u) << 1u) | ((x & 0xaaaaaaaau) >> 1u);
    return (f32)x * 2.3283064365386963e-10f;
}
/* Camera-independent reserve inspired by TriAevum's source placement cache.
 * Alternate LOD rings to avoid exhausting transition raycasts on near cells.
 * Mature the cache during fade-in instead of replaying a full reveal afterward.
 */
static void SGK_Prewarm(PlayState* play, f32 px, f32 py, f32 pz,
                        s32 quality, s32 layer, u32 budget, s32 distPct,
                        u32 samplesPerTier) {
    u32 sample, tier;
    if (!CVarGetInteger("SGK.GrassPrecache", 1)) return;
    for (sample = 0u; sample < samplesPerTier && sRaycastsThisFrame < budget; ++sample) {
        for (tier = 0u; tier < 3u && sRaycastsThisFrame < budget; ++tier) {
            SGKTier cfg = SGK_TierForScene(play, quality, (s32)tier);
            u32 n = sPrefetchCursor[tier]++;
            f32 nearR = (f32)(cfg.innerRadius * distPct) / 100.0f;
            f32 farR = (f32)(cfg.outerRadius * distPct) / 100.0f;
            f32 ratio = SGK_RadicalInverse(n + 1u);
            f32 rad = sqrtf(nearR * nearR + (farR * farR - nearR * nearR) * ratio);
            f32 ang = (f32)n * 2.39996322973f;
            s32 gx = SGK_GridFloor(px + cosf(ang) * rad, cfg.spacing);
            s32 gz = SGK_GridFloor(pz + sinf(ang) * rad, cfg.spacing);
            SGKCell* cell = SGK_GetCell(play, gx, gz, (s32)tier, layer, cfg.spacing, py, budget);
            if (cell != NULL && cell->occupied) {
                /* Unsigned wrap is valid during first fade before the draw counter starts. */
                cell->bornFrame = sFrameNumber - SGK_NEW_CELL_FADE_FRAMES;
            }
            ++sPrecacheRequests;
        }
    }
}

static void SGK_Vertex(Vtx* v, f32 x, f32 y, f32 z, u8 r, u8 g, u8 b) {
    /* N64 Vtx uses signed integer positions. The model matrix scales the
     * doubled coordinates back to world space, reducing sub-unit snapping.
     * Prior releases used sub-unit blades whose endpoints often collapsed to
     * the SAME integer coordinate, causing intermittent zero-area triangles.
     */
    v->v.ob[0] = (s16)SGK_Clamp(roundf((x - sVertexOriginX) * SGK_VERTEX_PRECISION), -32760.0f, 32760.0f);
    v->v.ob[1] = (s16)SGK_Clamp(roundf((y - sVertexOriginY) * SGK_VERTEX_PRECISION), -32760.0f, 32760.0f);
    v->v.ob[2] = (s16)SGK_Clamp(roundf((z - sVertexOriginZ) * SGK_VERTEX_PRECISION), -32760.0f, 32760.0f);
    v->v.flag = 0;
    v->v.tc[0] = 0;
    v->v.tc[1] = 0;
    v->v.cn[0] = r;
    v->v.cn[1] = g;
    v->v.cn[2] = b;
    v->v.cn[3] = 255; /* opaque, avoid expensive alpha sorting on Intel UHD */
}

/* A blade is a much narrower triangle, not a broad triangular tuft.
 * Each blade is separately scattered and animated, yielding a "filament" look.
 * The collision plane normal approximates the Y offset for each blade.
 */
static void SGK_MakeBlade(Vtx* v, const SGKCell* cell, s32 gx, s32 gz,
                          s32 spacing, u32 bladeIndex, s32 tier, f32 phase, s32 wind,
                          f32 scale, f32 cellGust, f32 cellGust2, f32 heightScale, f32 windPower) {
    u32 n = SGK_Hash(cell->seed ^ (bladeIndex + 1u) * 0x165667b1U);
    f32 cellX = ((f32)gx + 0.15f + 0.70f * SGK_Unit(SGK_Hash(cell->seed ^ 0xa77u))) * spacing;
    f32 cellZ = ((f32)gz + 0.15f + 0.70f * SGK_Unit(SGK_Hash(cell->seed ^ 0xb91u))) * spacing;
    f32 offsetX = (SGK_Unit(SGK_Hash(n ^ 0xc33u)) - 0.5f) * (f32)spacing * 0.68f;
    f32 offsetZ = (SGK_Unit(SGK_Hash(n ^ 0xd44u)) - 0.5f) * (f32)spacing * 0.68f;
    f32 x = cellX + offsetX;
    f32 z = cellZ + offsetZ;
    f32 y = cell->y;
    f32 ang = SGK_Unit(SGK_Hash(n ^ 0xe55u)) * 2.0f * SGK_PI;
    f32 dx = cosf(ang);
    f32 dz = sinf(ang);
    u32 kind = (n >> 17) & 3u; /* short, upright, tall, wispy */
    f32 heightBase = kind == 0 ? 5.0f : kind == 1 ? 9.5f : kind == 2 ? 14.0f : 11.0f;
    /* Shrinking width to zero at a LOD border generated subpixel, flickering
     * triangles. Keep a stable silhouette and fade only the blade height.
     * A faint root disappears after the ring boundary without sparkle.
     */
    f32 heightFade = 0.06f + 0.94f * scale;
    f32 widthFade = 0.80f + 0.20f * scale;
    /* 15 percent shorter by default, ONLY the vertical length changes. */
    f32 h = (heightBase + 6.0f * SGK_Unit(SGK_Hash(n ^ 0xf66u))) * heightFade * heightScale;
    f32 w = ((tier == 0 ? 1.05f : tier == 1 ? 1.08f : 1.15f) +
             0.32f * SGK_Unit(SGK_Hash(n ^ 0x117u))) * widthFade;
    /* Shared slow gust per cell + individual ripples: fewer trigonometry calls
     * than computing two full waves anew for every leaf at every frame.
     */
    /* Two long, world-space wind fronts plus a small independent ripple.
     * Far LOD skips per-blade trig to protect integrated graphics. */
    f32 ripple = tier != 2 ? sinf(phase * 0.52f - z * 0.012f +
                                   (f32)(n & 255u) * 0.023f) : 0.0f;
    f32 high = tier == 0 ? sinf(phase * 1.01f +
                                 (f32)((n >> 8) & 255u) * 0.017f) * 0.075f : 0.0f;
    /* The second broad wave also modulates gust intensity: slow, irregular
     * wind swell without another expensive sine per individual leaf. */
    f32 gustEnvelope = 0.80f + 0.20f * cellGust2;
    f32 windX = wind ? (cellGust * 0.59f + cellGust2 * 0.22f +
                         ripple * 0.22f + high) * windPower * heightFade *
                         gustEnvelope : 0.0f;
    f32 windZ = wind ? (cellGust2 * 0.52f - cellGust * 0.14f +
                         ripple * 0.17f - high * 0.45f) * windPower * heightFade *
                         gustEnvelope : 0.0f;
    f32 bendX = windX * (0.59f + 0.15f * (f32)(n & 3u)) + cell->pushX * heightFade;
    f32 bendZ = windZ * (0.40f + 0.12f * (f32)((n >> 3) & 3u)) + cell->pushZ * heightFade;
    u32 tint = SGK_Hash(n ^ 0x3185u) % 25u;
    /* Muted BOTW-like greens, less stark tip/base contrast and sparkle. */
    u8 r0 = (u8)(kind == 3 ? 54u + tint : 45u + tint);
    u8 g0 = (u8)(kind == 3 ? 76u + tint : 67u + tint);
    u8 b0 = (u8)(29u + tint / 3u);
    u8 r1 = (u8)(kind == 3 ? 75u + tint : 65u + tint);
    u8 g1 = (u8)(kind == 3 ? 105u + tint : 94u + tint);
    u8 b1 = (u8)(41u + tint / 2u);

    if (cell->normalY > 0) {
        y -= (cell->normalX * offsetX + cell->normalZ * offsetZ) / (f32)cell->normalY;
    }

    SGK_Vertex(&v[0], x - dx * w, y, z - dz * w, r0, g0, b0);
    SGK_Vertex(&v[1], x + dx * w, y, z + dz * w, r0, g0, b0);
    /* Gently lower the tip while bending to mimic a flexible stalk.
     * Root and width stay fixed, preventing flickering subpixel geometry. */
    SGK_Vertex(&v[2], x + bendX + dx * (w * 0.25f),
               y + h - (fabsf(windX) + fabsf(windZ)) * 0.16f,
               z + bendZ + dz * (w * 0.25f), r1, g1, b1);
}

void SGKGrass_Draw(PlayState* play) {
    s32 quality, tier, distancePercent, layer, wind, budget, interaction, viewCull;
    f32 heightScale, sceneMidY, sceneHalfY, windPower;
    f32 px, py, pz;
    f32 phase;
    u32 vertsInBatch = 0;
    Vtx* batch = NULL;
    u32 totalBlades = 0, totalCells = 0;

    ++sFrameNumber;
    if (play == NULL || !CVarGetInteger("SGK.GrassEnabled", 1) ||
        play->colCtx.colHeader == NULL) return;
    if (!sVersionLogged) {
        osSyncPrintf("[SGK Grass 0.9] balanced prewarm, stable LOD and independent density\n");
        sVersionLogged = 1;
    }

    quality = CVarGetInteger("SGK.GrassQuality", 1);
    if (quality < 0) quality = 0;
    if (quality > 2) quality = 2;
    wind = CVarGetInteger("SGK.GrassWind", 1);
    sEdgeCheck = CVarGetInteger("SGK.GrassEdgeCheck", 1) != 0;
    windPower = SGK_Clamp((f32)CVarGetInteger("SGK.GrassWindStrength", 100),
                           0.0f, 180.0f) * 0.01f;
    distancePercent = CVarGetInteger("SGK.GrassDistance", 100);
    distancePercent = (s32)SGK_Clamp((f32)distancePercent, 50.0f,
                              play->sceneNum == SCENE_HYRULE_FIELD ? 135.0f : 180.0f);
    /* Lower CPU raycast pressure; cache is now stable across stair travel. */
    budget = CVarGetInteger("SGK.GrassRaycastBudget", 300);
    if (budget < 80) budget = 80;
    if (budget > 700) budget = 700;
    /* Height only. Does not change width, number or distribution of blades. */
    heightScale = SGK_Clamp((f32)CVarGetInteger("SGK.GrassHeightPercent", 85), 55.0f, 120.0f) * 0.01f;
    sceneMidY = (play->colCtx.minBounds.y + play->colCtx.maxBounds.y) * 0.5f;
    sceneHalfY = (play->colCtx.maxBounds.y - play->colCtx.minBounds.y) * 0.5f;
    if (sceneHalfY < 0.0f) sceneHalfY = 0.0f;
    viewCull = CVarGetInteger("SGK.GrassViewCulling", 1);
    interaction = CVarGetInteger("SGK.GrassInteraction", 1);
    SGK_InitView(&play->view);
    if (!viewCull) sView.valid = 0;
    SGK_CollectInfluences(play);
    sCulledChunks = 0;
    sCulledCells = 0;
    sConsideredCells = 0;
    px = GET_PLAYER(play)->actor.world.pos.x;
    py = GET_PLAYER(play)->actor.world.pos.y;
    pz = GET_PLAYER(play)->actor.world.pos.z;
    /* Global Kokiri ground cache, not a Link-height-dependent slice.
     * It is safe because the raycast above always starts at scene max Y.
     */
    layer = 0;
    phase = (f32)sFrameNumber * 0.0215f; /* gradual, spatial wind waves */
    sRaycastsThisFrame = 0;

    /* A reset is only allowed after scene reload, quality change, or a new
     * PlayState lifetime; NEVER when Link merely changes elevation.
     */
    if (sCachedScene != play->sceneNum || sCachedQuality != quality ||
        sCachedEdgeCheck != sEdgeCheck ||
        sCachedCollisionHeader != play->colCtx.colHeader ||
        play->gameplayFrames < sPreviousGameplayFrame) {
        SGK_ResetCache(play->sceneNum, quality);
        sCachedCollisionHeader = play->colCtx.colHeader;
    }
    sPreviousGameplayFrame = play->gameplayFrames;
    sNewCells = 0;
    sFrameCachedCells = 0;
    if (sSceneHasGrass < 0) sSceneHasGrass = SGK_SceneContainsGrass(play);
    if (!sSceneHasGrass) return; /* never spend per-frame work on non-grass scenes */

    /* Fixed local origin per frame: preserve 0.25-unit precision without
     * clamping vertices on large world maps or inside distant areas. */
    sVertexOriginX = floorf(px / 32.0f) * 32.0f;
    sVertexOriginY = floorf(py / 32.0f) * 32.0f;
    sVertexOriginZ = floorf(pz / 32.0f) * 32.0f;

    OPEN_DISPS(play->state.gfxCtx);
    POLY_OPA_DISP = Gfx_SetupDL(POLY_OPA_DISP, 25);
    gDPPipeSync(POLY_OPA_DISP++);
    gDPSetCycleType(POLY_OPA_DISP++, G_CYC_1CYCLE);
    gDPSetCombineMode(POLY_OPA_DISP++, G_CC_SHADE, G_CC_SHADE);
    gDPSetRenderMode(POLY_OPA_DISP++, G_RM_AA_ZB_OPA_SURF, G_RM_AA_ZB_OPA_SURF2);
    gSPTexture(POLY_OPA_DISP++, 0, 0, 0, G_TX_RENDERTILE, G_OFF);
    gSPClearGeometryMode(POLY_OPA_DISP++, G_LIGHTING | G_CULL_BOTH | G_TEXTURE_GEN | G_TEXTURE_GEN_LINEAR);
    gSPSetGeometryMode(POLY_OPA_DISP++, G_ZBUFFER | G_SHADE | G_SHADING_SMOOTH);
    Matrix_Push();
    Matrix_Translate(sVertexOriginX, sVertexOriginY, sVertexOriginZ, MTXMODE_NEW);
    Matrix_Scale(SGK_VERTEX_SCALE, SGK_VERTEX_SCALE, SGK_VERTEX_SCALE, MTXMODE_APPLY);
    gSPMatrix(POLY_OPA_DISP++, MATRIX_NEWMTX(play->state.gfxCtx),
              G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    Matrix_Pop();

    /* Macro lives inside this function and only emits complete batches.
     * One gSPVertex for up to 10 blades greatly reduces draw-state overhead.
     */
#define SGK_FLUSH_BATCH()                                                      \
    do {                                                                       \
        if (vertsInBatch != 0 && batch != NULL) {                              \
            u32 ti;                                                           \
            gSPVertex(POLY_OPA_DISP++, (uintptr_t)batch, vertsInBatch, 0);    \
            for (ti = 0; ti + 1u < vertsInBatch / 3u; ti += 2u) {            \
                gSP2Triangles(POLY_OPA_DISP++, 3u * ti, 3u * ti + 1u,         \
                              3u * ti + 2u, 0, 3u * (ti + 1u),               \
                              3u * (ti + 1u) + 1u, 3u * (ti + 1u) + 2u, 0); \
            }                                                                  \
            if (ti < vertsInBatch / 3u) {                                      \
                gSP1Triangle(POLY_OPA_DISP++, 3u * ti, 3u * ti + 1u,           \
                             3u * ti + 2u, 0);                               \
            }                                                                  \
            vertsInBatch = 0;                                                  \
            batch = NULL;                                                      \
        }                                                                      \
    } while (0)

    /* Chunk-frustum culling happens before expensive cell tests/raycasts.
     * All sample positions, fade, cache entries remain fixed in world-space.
     * Stable hashed thinning distributes far details evenly without stripe artifacts.
     */
    for (tier = 0; tier < 3; ++tier) {
        SGKTier cfg = SGK_TierForScene(play, quality, tier);
        s32 minR = cfg.innerRadius * distancePercent / 100;
        s32 maxR = cfg.outerRadius * distancePercent / 100;
        s32 spacing = cfg.spacing;
        /* World-aligned 6x6 chunks traversed CENTER OUT.
         * v0.3 walked raster rows from the far edge, spending the raycast
         * budget on distant terrain while close cells popped in later.
         */
        s32 playerChunkX = SGK_GridFloor(px, spacing * SGK_CHUNK_SIDE);
        s32 playerChunkZ = SGK_GridFloor(pz, spacing * SGK_CHUNK_SIDE);
        s32 maxBand = maxR / (spacing * SGK_CHUNK_SIDE) + 2;
        s32 band, chunkX, chunkZ;
        /* IMPORTANT: no frame-dependent maxBlades cut. In v0.4 the first
         * visible cells used all capacity and the others abruptly disappeared
         * whenever the camera turned. Density is now a stable hash decision
         * per world-space cell, unaffected by view direction or frame order.
         */
        f32 density = (quality == 0 ? (tier == 0 ? 0.82f : tier == 1 ? 0.75f : 0.70f) :
                       quality == 1 ? (tier == 0 ? 0.86f : tier == 1 ? 0.82f : 0.78f) :
                                      (tier == 0 ? 0.76f : tier == 1 ? 0.77f : 0.79f));
        s32 densityPercent = CVarGetInteger("SGK.GrassDensity", 100);
        densityPercent = (s32)SGK_Clamp((f32)densityPercent, 30.0f, 125.0f);
        density *= (f32)densityPercent * 0.01f;
        if (density > 1.0f) density = 1.0f;
        /* More draw distance does not reduce the density of near grass. */
        if (play->sceneNum == SCENE_HYRULE_FIELD && tier == 2)
            density *= quality == 0 ? 0.48f : (quality == 1 ? 0.67f : 0.72f);
        for (band = 0; band <= maxBand; ++band) {
            s32 offsetZ, offsetX;
            for (offsetZ = -band; offsetZ <= band; ++offsetZ) {
                for (offsetX = -band; offsetX <= band; ++offsetX) {
                s32 iz, ix;
                s32 countX = SGK_CHUNK_SIDE;
                s32 countZ = SGK_CHUNK_SIDE;
                if (abs(offsetX) != band && abs(offsetZ) != band) continue;
                chunkX = (playerChunkX + offsetX) * SGK_CHUNK_SIDE;
                chunkZ = (playerChunkZ + offsetZ) * SGK_CHUNK_SIDE;
                f32 centerX = ((f32)chunkX + 0.5f * (f32)countX) * spacing;
                f32 centerZ = ((f32)chunkZ + 0.5f * (f32)countZ) * spacing;
                /* Sphere encloses both XZ chunk size and the *full* static
                 * scene height range. No stairs-induced culling of low grass.
                 * Fine cull below uses the cached ground Y for precision.
                 */
                f32 chunkRadius = (f32)SGK_CHUNK_SIDE * (f32)spacing * 0.75f +
                                  sceneHalfY + 45.0f;
                f32 cdX = centerX - px, cdZ = centerZ - pz;
                if (cdX * cdX + cdZ * cdZ > (maxR + chunkRadius) * (maxR + chunkRadius)) continue;
                if (!SGK_Visible(centerX, sceneMidY, centerZ, chunkRadius)) { ++sCulledChunks; continue; }
                for (iz = chunkZ; iz < chunkZ + countZ; ++iz) {
                    for (ix = chunkX; ix < chunkX + countX; ++ix) {
                        f32 x = ((f32)ix + 0.5f) * spacing;
                        f32 z = ((f32)iz + 0.5f) * spacing;
                        f32 dx = x - px, dz = z - pz;
                        f32 d2 = dx * dx + dz * dz;
                        f32 distance, fade, ageFade, cellGust, cellGust2;
                        SGKCell* cell;
                        u32 blade, seed;
                        if (d2 < (f32)(minR * minR) || d2 > (f32)(maxR * maxR)) continue;
                        ++sConsideredCells;
                        /* Conservative broad phase before raycast. The actual
                         * world-space floor Y is not yet known here.
                         */
                        if (!SGK_Visible(x, sceneMidY, z,
                                         (f32)spacing * 1.3f + sceneHalfY + 65.0f)) {
                            ++sCulledCells;
                            continue;
                        }
                        seed = SGK_Seed(ix, iz, tier);
                        if (SGK_Unit(SGK_Hash(seed ^ 0x3bau)) > density) continue;
                        cell = SGK_GetCell(play, ix, iz, tier, layer, spacing, py, (u32)budget);
                        if (cell == NULL || !cell->occupied) continue;
                        /* Exact narrow phase: visible cells use the true floor
                         * height, even if Link is above them on a ladder.
                         * Data remains cached when drawing is skipped.
                         */
                        if (!SGK_Visible(x, cell->y, z, (f32)spacing * 1.3f + 65.0f)) {
                            ++sCulledCells;
                            continue;
                        }
                        distance = sqrtf(d2);
                        /* Continuous smooth overlap of neighboring LOD rings.
                         * No hard 24% cutoff: the geometry eases almost to zero.
                         */
                        fade = SGK_Smooth((maxR - distance) / (f32)(spacing * 12));
                        if (tier != 0) {
                            f32 nearFade = SGK_Smooth((distance - minR) / (f32)(spacing * 10));
                            if (nearFade < fade) fade = nearFade;
                        }
                        ageFade = SGK_Smooth((f32)(sFrameNumber - cell->bornFrame) / (f32)SGK_NEW_CELL_FADE_FRAMES);
                        if (ageFade < fade) fade = ageFade;
                        if (fade < 0.025f) continue; /* stable dither handles ring edges */
                        if (tier == 0) SGK_UpdateBend(cell, x, cell->y, z, interaction);
                        cellGust = wind ? sinf(phase * 0.33f + x * 0.010f + z * 0.008f) : 0.0f;
                        cellGust2 = wind ? sinf(phase * 0.24f - x * 0.006f +
                                                  z * 0.011f + 1.41f) : 0.0f;
                        ++sFrameCachedCells;
                        for (blade = 0; blade < cfg.bladesPerCell; ++blade) {
                            /* Stable world-space dithering avoids moving subpixel triangles. */
                            if (SGK_Unit(SGK_Hash(cell->seed ^
                                ((blade + 1u) * 0x9e3779b9u) ^ 0xf013u)) >= fade) continue;
                            if (batch == NULL) {
                                batch = Graph_Alloc(play->state.gfxCtx, sizeof(Vtx) * 3u * SGK_BATCH_BLADES);
                                if (batch == NULL) goto SGK_Finish;
                            }
                            SGK_MakeBlade(&batch[vertsInBatch], cell, ix, iz, spacing,
                                          blade, tier, phase, wind, 1.0f, cellGust,
                                          cellGust2, heightScale, windPower);
                            vertsInBatch += 3u;
                            ++totalBlades;
                            if (vertsInBatch == SGK_BATCH_BLADES * 3u) SGK_FLUSH_BATCH();
                        }
                        ++totalCells;
                    }
                }
                }
            }
        }
    }
SGK_Finish:
    SGK_FLUSH_BATCH();
#undef SGK_FLUSH_BATCH

    sDrawnBlades = totalBlades;
    sDrawnCells = totalCells;
    gDPPipeSync(POLY_OPA_DISP++);
    POLY_OPA_DISP = Gfx_SetupDL(POLY_OPA_DISP, 25);
    CLOSE_DISPS(play->state.gfxCtx);
    /* Prefetch offscreen grass only when visible rendering is finished. */
    sPrecacheRequests = 0;
    SGK_Prewarm(play, px, py, pz, quality, layer, (u32)budget, distancePercent, SGK_PREFETCH_SAMPLES_PER_TIER);

    if (CVarGetInteger("SGK.GrassDebug", 0) && (sFrameNumber % 300u) == 0u) {
        osSyncPrintf("[SGK 0.9] scene=%d quality=%d dist=%d%% cells=%u blades=%u rays=%u new=%u cache_evictions=%u precache=%u visible_cells=%u cache=%u culled_chunks=%u culled_cells=%u actors=%u\n",
                     play->sceneNum, quality, distancePercent, sDrawnCells, sDrawnBlades, sRaycastsThisFrame,
                     sNewCells, sCacheEvictions, sPrecacheRequests, sFrameCachedCells,
                     sCacheMisses, sCulledChunks, sCulledCells, sInfluenceCount);
    }
}
/* Execute during scene fade-in (Play_Update), before any vegetation is drawn. */
void SGKGrass_PrewarmTransition(PlayState* play) {
    Player* player;
    s32 quality, distancePercent, budget;
    if (play == NULL || play->colCtx.colHeader == NULL ||
        play->transitionMode == TRANS_MODE_OFF ||
        play->transitionTrigger != TRANS_TRIGGER_END ||
        !CVarGetInteger("SGK.GrassEnabled", 1) ||
        !CVarGetInteger("SGK.GrassFadePrewarm", 1)) return;
    player = GET_PLAYER(play);
    if (player == NULL) return;
    quality = CVarGetInteger("SGK.GrassQuality", 1);
    if (quality < 0) quality = 0;
    if (quality > 2) quality = 2;
    sEdgeCheck = CVarGetInteger("SGK.GrassEdgeCheck", 1) != 0;
    if (sCachedScene != play->sceneNum || sCachedQuality != quality ||
        sCachedEdgeCheck != sEdgeCheck ||
        sCachedCollisionHeader != play->colCtx.colHeader ||
        play->gameplayFrames < sPreviousGameplayFrame) {
        SGK_ResetCache(play->sceneNum, quality);
        sCachedCollisionHeader = play->colCtx.colHeader;
    }
    if (sSceneHasGrass < 0) sSceneHasGrass = SGK_SceneContainsGrass(play);
    if (!sSceneHasGrass || sTransitionWarmFrames++ > 50u) return;
    distancePercent = CVarGetInteger("SGK.GrassDistance", 100);
    distancePercent = (s32)SGK_Clamp((f32)distancePercent, 50.0f,
                 play->sceneNum == SCENE_HYRULE_FIELD ? 135.0f : 180.0f);
    budget = CVarGetInteger("SGK.GrassFadeWarmBudget", 360);
    if (budget < 60) budget = 60;
    if (budget > 480) budget = 480;
    sRaycastsThisFrame = 0;
    SGK_Prewarm(play, player->actor.world.pos.x, player->actor.world.pos.y,
                player->actor.world.pos.z, quality, 0, (u32)budget,
                distancePercent, 140u);
}
