/*
 * SGK Grass 0.2 - Kokiri, Ship of Harkinian 9.3.1.
 * World-space blades, three LOD rings, stable sampling and streamed collision cache.
 * Does not replace textures, OTR/O2R mods or character models.
 * The mask is based on collision surface sound, NOT visual texture/material.
 */
#include "global.h"
#include "soh/Enhancements/SGK/SGKGrass.h"
#include <math.h>
#include <limits.h>
#include <string.h>
#include <libultraship/bridge/consolevariablebridge.h>

#define SGK_CACHE_CAPACITY 32768u
#define SGK_CACHE_MASK (SGK_CACHE_CAPACITY - 1u)
#define SGK_BATCH_BLADES 10u /* 10 triangles = 30 vertices; N64 Fast3D limit is 32 */
#define SGK_FLOOR_GRASS_SFX (NA_SE_PL_WALK_GRASS - SFX_FLAG)
#define SGK_PI 3.14159265358979323846f

typedef struct SGKCell {
    s32 gridX, gridZ, layer;
    u32 seed;
    f32 y;
    s16 normalX, normalY, normalZ;
    u8 tier, valid, occupied;
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
    { { 18,  0, 235, 5 }, { 30, 220, 480, 3 }, { 48, 445, 700, 2 } },
    { { 13,  0, 275, 7 }, { 23, 255, 570, 4 }, { 37, 530, 850, 3 } },
    { { 12,  0, 315, 8 }, { 21, 295, 685, 5 }, { 38, 645, 1050, 3 } },
};

static SGKCell sCells[SGK_CACHE_CAPACITY];
static s32 sCachedScene = -1;
static s32 sCachedQuality = -1;
static s32 sCachedLayer = INT_MIN;
static u32 sFrameNumber = 0;
static u32 sCacheMisses = 0;
static u32 sDrawnBlades = 0;
static u32 sDrawnCells = 0;
static u32 sRaycastsThisFrame = 0;

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
    f32 h = (5.5f + 6.0f * SGK_Unit(SGK_Hash(n ^ 0xf66u))) * scale;
    f32 w = ((tier == 0 ? 0.60f : (tier == 1 ? 0.81f : 1.00f)) +
             0.50f * SGK_Unit(SGK_Hash(n ^ 0x117u))) * scale;
    f32 sway = wind ? sinf(phase + (f32)(n & 255u) * 0.032f) * 1.45f * scale : 0.0f;
    f32 bendX = sway * (0.62f + 0.12f * (f32)(n & 3u));
    f32 bendZ = sway * (0.38f + 0.10f * (f32)((n >> 3) & 3u));
    u32 tint = (n >> 17) & 19u;
    u8 r0 = (u8)(48u + tint), g0 = (u8)(64u + tint), b0 = (u8)(25u + tint / 2u);
    u8 r1 = (u8)(78u + tint), g1 = (u8)(99u + tint), b1 = (u8)(37u + tint / 2u);

    if (cell->normalY > 0) {
        y -= (cell->normalX * offsetX + cell->normalZ * offsetZ) / (f32)cell->normalY;
    }

    SGK_Vertex(&v[0], x - dx * w, y, z - dz * w, r0, g0, b0);
    SGK_Vertex(&v[1], x + dx * w, y, z + dz * w, r0, g0, b0);
    SGK_Vertex(&v[2], x + bendX + dx * (w * 0.25f), y + h,
               z + bendZ + dz * (w * 0.25f), r1, g1, b1);
}

void SGKGrass_Draw(PlayState* play) {
    s32 quality, tier, distancePercent, layer, wind, budget;
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
    distancePercent = (s32)SGK_Clamp((f32)distancePercent, 50.0f, 150.0f);
    budget = CVarGetInteger("SGK.GrassRaycastBudget", 300);
    if (budget < 80) budget = 80;
    if (budget > 1200) budget = 1200;
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

    /* Three separate rings avoid rendering expensive near detail far away.
     * Their positions and sampling depend on world coordinates, not camera angle.
     */
    for (tier = 0; tier < 3; ++tier) {
        SGKTier cfg = sTierSettings[quality][tier];
        s32 minR = cfg.innerRadius * distancePercent / 100;
        s32 maxR = cfg.outerRadius * distancePercent / 100;
        s32 spacing = cfg.spacing;
        s32 xStart = SGK_GridFloor(px - maxR, spacing);
        s32 xEnd   = SGK_GridFloor(px + maxR, spacing);
        s32 zStart = SGK_GridFloor(pz - maxR, spacing);
        s32 zEnd   = SGK_GridFloor(pz + maxR, spacing);
        s32 iz, ix;
        u32 tierCells = 0;
        u32 cap = (quality == 0) ? (tier == 0 ? 900u : (tier == 1 ? 800u : 700u)) :
                  (quality == 1) ? (tier == 0 ? 1600u : (tier == 1 ? 1900u : 1300u)) :
                                   (tier == 0 ? 2500u : (tier == 1 ? 3000u : 2200u));
        for (iz = zStart; iz <= zEnd; ++iz) {
            for (ix = xStart; ix <= xEnd; ++ix) {
                f32 x = ((f32)ix + 0.5f) * spacing;
                f32 z = ((f32)iz + 0.5f) * spacing;
                f32 dx = x - px, dz = z - pz;
                f32 d2 = dx * dx + dz * dz;
                f32 distance;
                f32 fade;
                SGKCell* cell;
                u32 blade;
                if (d2 < (f32)(minR * minR) || d2 > (f32)(maxR * maxR)) continue;
                /* World-stable thinning when distance scaling enlarges coverage.
                 * Does not favor north/south or camera direction.
                 */
                if (distancePercent > 100 &&
                    SGK_Unit(SGK_Hash(SGK_Seed(ix, iz, tier) ^ 0x3bau)) >
                    10000.0f / (f32)(distancePercent * distancePercent)) continue;
                if (tierCells >= cap) continue;
                cell = SGK_GetCell(play, ix, iz, tier, layer, spacing, py, (u32)budget);
                if (cell == NULL || !cell->occupied) continue;

                /* Subtle distance fade by reducing height, not using alpha blend. */
                distance = sqrtf(d2);
                fade = SGK_Clamp((maxR - distance) / (f32)(spacing * 5), 0.0f, 1.0f);
                if (tier != 0) {
                    f32 nearFade = SGK_Clamp((distance - minR) / (f32)(spacing * 3), 0.0f, 1.0f);
                    if (nearFade < fade) fade = nearFade;
                }
                if (fade < 0.22f) continue;

                for (blade = 0; blade < cfg.bladesPerCell; ++blade) {
                    if (batch == NULL) {
                        batch = Graph_Alloc(play->state.gfxCtx, sizeof(Vtx) * 3u * SGK_BATCH_BLADES);
                        if (batch == NULL) goto SGK_Finish;
                    }
                    SGK_MakeBlade(&batch[vertsInBatch], cell, ix, iz, spacing,
                                  blade, tier, phase, wind, fade);
                    vertsInBatch += 3u;
                    ++totalBlades;
                    if (vertsInBatch == SGK_BATCH_BLADES * 3u) SGK_FLUSH_BATCH();
                }
                ++tierCells;
                ++totalCells;
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
        osSyncPrintf("[SGK 0.2] quality=%d distance=%d%% cells=%u blades=%u rays=%u total_rays=%u\n",
                     quality, distancePercent, sDrawnCells, sDrawnBlades,
                     sRaycastsThisFrame, sCacheMisses);
    }
}