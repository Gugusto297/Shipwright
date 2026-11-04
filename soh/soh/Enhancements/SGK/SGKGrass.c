/*
 * SGK Grass 0.3 - Kokiri, Ship of Harkinian 9.3.1.
 * World-space blades, camera frustum culling, three LOD rings, physical actor bending.
 * Does not replace textures, OTR/O2R mods or character models.
 * The mask is based on collision surface sound, NOT visual texture/material.
 */
#include "global.h"
#include "soh/Enhancements/SGK/SGKGrass.h"
#include <math.h>
#include <limits.h>
#include <string.h>
#include <libultraship/bridge/consolevariablebridge.h>

#define SGK_CACHE_CAPACITY 65536u
#define SGK_CACHE_MASK (SGK_CACHE_CAPACITY - 1u)
#define SGK_BATCH_BLADES 10u /* 10 triangles = 30 vertices; N64 Fast3D limit is 32 */
#define SGK_FLOOR_GRASS_SFX (NA_SE_PL_WALK_GRASS - SFX_FLAG)
#define SGK_PI 3.14159265358979323846f
#define SGK_MAX_INFLUENCES 24
#define SGK_CHUNK_SIDE 6 /* group nearby cells before collision queries */

typedef struct SGKCell {
    s32 gridX, gridZ, layer;
    u32 seed;
    f32 y;
    s16 normalX, normalY, normalZ;
    u8 tier, valid, occupied;
    f32 pushX, pushZ; /* smoothed bend from actors */
    u32 lastBendFrame;
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
    { { 15,    0,  300, 6 }, { 32,  265,  730, 3 }, { 62,  680, 1300, 2 } },
    { { 10,    0,  370, 9 }, { 23,  340, 1050, 5 }, { 56,  970, 1850, 2 } },
    { {  9,    0,  430, 11 }, { 20,  395, 1240, 6 }, { 52, 1150, 2200, 3 } },
};

static f32 SGK_Clamp(f32 x, f32 lo, f32 hi);

static SGKCell sCells[SGK_CACHE_CAPACITY];
static s32 sCachedScene = -1;
static s32 sCachedQuality = -1;
static s32 sCachedLayer = INT_MIN;
static u32 sFrameNumber = 0;
static u32 sCacheMisses = 0;
static u32 sDrawnBlades = 0;
static u32 sDrawnCells = 0;
static u32 sRaycastsThisFrame = 0;
static u32 sCulledChunks = 0;
static u32 sCulledCells = 0;
static u32 sConsideredCells = 0;

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
    u32 k, frames = cell->lastBendFrame ? (sFrameNumber - cell->lastBendFrame) : 999u;
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
    if (frames > 16u) { cell->pushX = 0.0f; cell->pushZ = 0.0f; }
    else if (frames > 1u) {
        f32 decay = powf(0.80f, (f32)(frames - 1u));
        cell->pushX *= decay; cell->pushZ *= decay;
    }
    cell->pushX += (tx - cell->pushX) * (fabsf(tx) > 0.1f ? 0.48f : 0.17f);
    cell->pushZ += (tz - cell->pushZ) * (fabsf(tz) > 0.1f ? 0.48f : 0.17f);
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

static s32 SGK_GridFloor(f32 x, s32 spacing) {
    return (s32)floorf(x / (f32)spacing);
}

static void SGK_ResetCache(s32 scene, s32 quality) {
    memset(sCells, 0, sizeof(sCells));
    sCachedScene = scene;
    sCachedQuality = quality;
    sCachedLayer = INT_MIN;
    sCacheMisses = 0;
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

/* Collision queries are spread over frames to avoid a long first-frame pause.
 * Null means a query has been deferred; occupied=0 is a completed, empty query.
 */
static SGKCell* SGK_GetCell(PlayState* play, s32 gx, s32 gz, s32 tier,
                            s32 layer, s32 spacing, f32 playerY, u32 raycastBudget) {
    u32 seed = SGK_Seed(gx, gz, tier);
    u32 slot = SGK_Hash(seed ^ (u32)layer * 0x27d4eb2dU) & SGK_CACHE_MASK;
    SGKCell* cell = &sCells[slot];
    f32 x, z, floorY;
    Vec3f pos;
    CollisionPoly* poly = NULL;
    s32 bgId = BG_ACTOR_MAX;

    if (cell->valid && cell->gridX == gx && cell->gridZ == gz &&
        cell->tier == tier && cell->layer == layer) return cell;
    if (sRaycastsThisFrame >= raycastBudget) return NULL;

    cell->valid = 1;
    cell->occupied = 0;
    cell->gridX = gx;
    cell->gridZ = gz;
    cell->tier = (u8)tier;
    cell->layer = layer;
    cell->seed = seed;
    cell->pushX = 0.0f; cell->pushZ = 0.0f;
    cell->lastBendFrame = 0;
    ++sCacheMisses;
    ++sRaycastsThisFrame;

    /* The same grid cell always selects the same position. */
    x = ((f32)gx + 0.15f + 0.70f * SGK_Unit(SGK_Hash(seed ^ 0xa77u))) * spacing;
    z = ((f32)gz + 0.15f + 0.70f * SGK_Unit(SGK_Hash(seed ^ 0xb91u))) * spacing;
    pos.x = x;
    pos.y = playerY + 210.0f;
    pos.z = z;
    floorY = BgCheck_EntityRaycastFloor3(&play->colCtx, &poly, &bgId, &pos);
    if (poly == NULL || bgId != BGCHECK_SCENE || floorY <= BGCHECK_Y_MIN + 1.0f) return cell;
    if (fabsf(floorY - playerY) > 170.0f) return cell;
    if (COLPOLY_GET_NORMAL(poly->normal.y) < 0.72f) return cell;
    if (SurfaceType_GetSfx(&play->colCtx, poly, bgId) != SGK_FLOOR_GRASS_SFX) return cell;
    if (SGK_IsUnderwater(play, x, floorY, z)) return cell;

    cell->y = floorY + 0.65f;
    cell->normalX = poly->normal.x;
    cell->normalY = poly->normal.y;
    cell->normalZ = poly->normal.z;
    cell->occupied = 1;
    return cell;
}

static void SGK_Vertex(Vtx* v, f32 x, f32 y, f32 z, u8 r, u8 g, u8 b) {
    v->v.ob[0] = (s16)x;
    v->v.ob[1] = (s16)y;
    v->v.ob[2] = (s16)z;
    v->v.flag = 0;
    v->v.tc[0] = 0;
    v->v.tc[1] = 0;
    v->v.cn[0] = r;
    v->v.cn[1] = g;
    v->v.cn[2] = b;
    v->v.cn[3] = 255;
}

/* A blade is a much narrower triangle, not a broad triangular tuft.
 * Each blade is separately scattered and animated, yielding a "filament" look.
 * The collision plane normal approximates the Y offset for each blade.
 */
static void SGK_MakeBlade(Vtx* v, const SGKCell* cell, s32 gx, s32 gz,
                          s32 spacing, u32 bladeIndex, s32 tier, f32 phase, s32 wind,
                          f32 scale) {
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
    f32 h = (heightBase + 6.0f * SGK_Unit(SGK_Hash(n ^ 0xf66u))) * scale;
    f32 w = ((tier == 0 ? 0.38f : tier == 1 ? 0.63f : 0.95f) +
             0.49f * SGK_Unit(SGK_Hash(n ^ 0x117u))) * scale;
    f32 gust = sinf(phase * 0.47f + x * 0.017f + z * 0.013f);
    f32 ripple = sinf(phase * 1.27f - z * 0.035f + (f32)(n & 255u) * 0.027f);
    f32 sway = wind ? (gust * 1.30f + ripple * 0.55f) * scale : 0.0f;
    f32 bendX = sway * (0.56f + 0.17f * (f32)(n & 3u)) + cell->pushX * scale;
    f32 bendZ = sway * (0.33f + 0.15f * (f32)((n >> 3) & 3u)) + cell->pushZ * scale;
    u32 tint = SGK_Hash(n ^ 0x3185u) % 25u;
    u8 r0 = (u8)(kind == 3 ? 58u + tint : 42u + tint);
    u8 g0 = (u8)(kind == 3 ? 73u + tint : 66u + tint);
    u8 b0 = (u8)(26u + tint / 3u);
    u8 r1 = (u8)(kind == 3 ? 110u + tint : 76u + tint);
    u8 g1 = (u8)(kind == 3 ? 119u + tint : 108u + tint);
    u8 b1 = (u8)(40u + tint / 2u);

    if (cell->normalY > 0) {
        y -= (cell->normalX * offsetX + cell->normalZ * offsetZ) / (f32)cell->normalY;
    }

    SGK_Vertex(&v[0], x - dx * w, y, z - dz * w, r0, g0, b0);
    SGK_Vertex(&v[1], x + dx * w, y, z + dz * w, r0, g0, b0);
    SGK_Vertex(&v[2], x + bendX + dx * (w * 0.25f), y + h,
               z + bendZ + dz * (w * 0.25f), r1, g1, b1);
}

void SGKGrass_Draw(PlayState* play) {
    s32 quality, tier, distancePercent, layer, wind, budget, interaction, viewCull;
    f32 px, py, pz;
    f32 phase;
    u32 vertsInBatch = 0;
    Vtx* batch = NULL;
    u32 totalBlades = 0, totalCells = 0;

    ++sFrameNumber;
    if (play == NULL || play->sceneNum != SCENE_KOKIRI_FOREST ||
        !CVarGetInteger("SGK.GrassEnabled", 1) || play->colCtx.colHeader == NULL) return;

    quality = CVarGetInteger("SGK.GrassQuality", 1);
    if (quality < 0) quality = 0;
    if (quality > 2) quality = 2;
    wind = CVarGetInteger("SGK.GrassWind", 1);
    distancePercent = CVarGetInteger("SGK.GrassDistance", 100);
    distancePercent = (s32)SGK_Clamp((f32)distancePercent, 50.0f, 180.0f);
    budget = CVarGetInteger("SGK.GrassRaycastBudget", 400);
    if (budget < 80) budget = 80;
    if (budget > 850) budget = 850;
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
    layer = SGK_GridFloor(py, 330);
    phase = (f32)sFrameNumber * 0.035f;
    sRaycastsThisFrame = 0;

    if (sCachedScene != play->sceneNum || sCachedQuality != quality) SGK_ResetCache(play->sceneNum, quality);
    if (sCachedLayer != layer) {
        SGK_ResetCache(play->sceneNum, quality);
        sCachedLayer = layer;
    }

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
    Matrix_Translate(0.0f, 0.0f, 0.0f, MTXMODE_NEW);
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
        SGKTier cfg = sTierSettings[quality][tier];
        s32 minR = cfg.innerRadius * distancePercent / 100;
        s32 maxR = cfg.outerRadius * distancePercent / 100;
        s32 spacing = cfg.spacing;
        s32 xStart = SGK_GridFloor(px - maxR, spacing);
        s32 xEnd = SGK_GridFloor(px + maxR, spacing);
        s32 zStart = SGK_GridFloor(pz - maxR, spacing);
        s32 zEnd = SGK_GridFloor(pz + maxR, spacing);
        s32 chunkX, chunkZ;
        f32 density = (quality == 0 ? (tier == 0 ? 0.93f : tier == 1 ? 0.78f : 0.70f) :
                       quality == 1 ? (tier == 0 ? 1.0f : tier == 1 ? 0.95f : 0.75f) :
                                      (tier == 0 ? 1.0f : tier == 1 ? 0.98f : 0.80f));
        u32 maxBlades = (quality == 0 ? (tier == 0 ? 11000u : tier == 1 ? 7000u : 2700u) :
                         quality == 1 ? (tier == 0 ? 22000u : tier == 1 ? 14000u : 4300u) :
                                        (tier == 0 ? 32000u : tier == 1 ? 21000u : 6300u));
        u32 tierBlades = 0;
        if (distancePercent > 100) density *= 10000.0f / (f32)(distancePercent * distancePercent);
        for (chunkZ = zStart; chunkZ <= zEnd; chunkZ += SGK_CHUNK_SIDE) {
            for (chunkX = xStart; chunkX <= xEnd; chunkX += SGK_CHUNK_SIDE) {
                s32 iz, ix;
                s32 countX = (chunkX + SGK_CHUNK_SIDE - 1 < xEnd) ? SGK_CHUNK_SIDE : xEnd - chunkX + 1;
                s32 countZ = (chunkZ + SGK_CHUNK_SIDE - 1 < zEnd) ? SGK_CHUNK_SIDE : zEnd - chunkZ + 1;
                f32 centerX = ((f32)chunkX + 0.5f * (f32)countX) * spacing;
                f32 centerZ = ((f32)chunkZ + 0.5f * (f32)countZ) * spacing;
                f32 chunkRadius = (f32)SGK_CHUNK_SIDE * (f32)spacing * 0.75f + 110.0f;
                if (!SGK_Visible(centerX, py, centerZ, chunkRadius)) { ++sCulledChunks; continue; }
                for (iz = chunkZ; iz < chunkZ + countZ; ++iz) {
                    for (ix = chunkX; ix < chunkX + countX; ++ix) {
                        f32 x = ((f32)ix + 0.5f) * spacing;
                        f32 z = ((f32)iz + 0.5f) * spacing;
                        f32 dx = x - px, dz = z - pz;
                        f32 d2 = dx * dx + dz * dz;
                        f32 distance, fade;
                        SGKCell* cell;
                        u32 blade, seed;
                        if (d2 < (f32)(minR * minR) || d2 > (f32)(maxR * maxR)) continue;
                        ++sConsideredCells;
                        if (!SGK_Visible(x, py, z, (f32)spacing * 1.3f + 65.0f)) {
                            ++sCulledCells;
                            continue;
                        }
                        seed = SGK_Seed(ix, iz, tier);
                        if (SGK_Unit(SGK_Hash(seed ^ 0x3bau)) > density) continue;
                        if (tierBlades + cfg.bladesPerCell > maxBlades) continue;
                        cell = SGK_GetCell(play, ix, iz, tier, layer, spacing, py, (u32)budget);
                        if (cell == NULL || !cell->occupied) continue;
                        distance = sqrtf(d2);
                        fade = SGK_Clamp((maxR - distance) / (f32)(spacing * 4), 0.0f, 1.0f);
                        if (tier != 0) {
                            f32 nearFade = SGK_Clamp((distance - minR) / (f32)(spacing * 3), 0.0f, 1.0f);
                            if (nearFade < fade) fade = nearFade;
                        }
                        if (fade < 0.24f) continue;
                        SGK_UpdateBend(cell, x, cell->y, z, interaction);
                        for (blade = 0; blade < cfg.bladesPerCell; ++blade) {
                            if (batch == NULL) {
                                batch = Graph_Alloc(play->state.gfxCtx, sizeof(Vtx) * 3u * SGK_BATCH_BLADES);
                                if (batch == NULL) goto SGK_Finish;
                            }
                            SGK_MakeBlade(&batch[vertsInBatch], cell, ix, iz, spacing,
                                          blade, tier, phase, wind, fade);
                            vertsInBatch += 3u;
                            ++totalBlades;
                            ++tierBlades;
                            if (vertsInBatch == SGK_BATCH_BLADES * 3u) SGK_FLUSH_BATCH();
                        }
                        ++totalCells;
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

    if (CVarGetInteger("SGK.GrassDebug", 0) && (sFrameNumber % 300u) == 0u) {
        osSyncPrintf("[SGK 0.3] quality=%d dist=%d%% cells=%u blades=%u rays=%u cache=%u culled_chunks=%u culled_cells=%u considered=%u actors=%u\n",
                     quality, distancePercent, sDrawnCells, sDrawnBlades, sRaycastsThisFrame,
                     sCacheMisses, sCulledChunks, sCulledCells, sConsideredCells, sInfluenceCount);
    }
}