/*
 * SGK Grass 0.1 - teste de integracao em Ship of Harkinian 9.3.1.
 * Grama poligonal 3D, inteiramente world-space, baseada em superficies
 * de COLISAO marcadas como GRAMA. Nao altera texturas/mods .otr/.o2r.
 *
 * LIMITACAO: o material de colisao NAO equivale a uma mascara de textura
 * de cada triangulo visual. Trilhas sobre colisao de grama ainda podem
 * receber folhas. Este fork experimental ativa o recurso por padrao, apenas em Kokiri.
 * Configuracao via CVars: SGK.GrassEnabled=1, SGK.GrassQuality=0/1/2,
 * SGK.GrassWind=1, SGK.GrassDebug=1 (log a cada 300 frames).
 */

#include "global.h"
#include "soh/Enhancements/SGK/SGKGrass.h"
#include <math.h>
#include <limits.h>
#include <string.h>

#include <libultraship/bridge/consolevariablebridge.h>

#define SGK_CACHE_CAPACITY 8192u
#define SGK_CACHE_MASK (SGK_CACHE_CAPACITY - 1u)
#define SGK_FLOOR_GRASS_SFX 8u /* z_bgcheck.c: tabela D_80119E10[8] */

/* A posicao e a identidade da planta NAO dependem do angulo da camera. */
typedef struct SGKCell {
    s32 gridX;
    s32 gridZ;
    s32 layer;
    s16 y;
    u8 valid;
    u8 occupied;
    u32 seed;
} SGKCell;

static SGKCell sCells[SGK_CACHE_CAPACITY];
static s32 sCachedScene = -1;
static s32 sCachedQuality = -1;
static s32 sCachedLayer = INT_MIN;
static u32 sFrameNumber = 0;
static u32 sCacheMisses = 0;
static u32 sDrawn = 0;

static u32 SGK_Hash(u32 n) {
    n ^= n >> 16;
    n *= 0x7feb352dU;
    n ^= n >> 15;
    n *= 0x846ca68bU;
    return n ^ (n >> 16);
}

static u32 SGK_CellSeed(s32 x, s32 z) {
    return SGK_Hash((u32)x * 0x9e3779b9U ^ (u32)z * 0x85ebca6bU ^ 0x53a71f2bU);
}

static f32 SGK_Unit(u32 value) {
    return (f32)(value & 65535U) / 65535.0f;
}

static s32 SGK_GridFloor(f32 value, s32 step) {
    return (s32)floorf(value / (f32)step);
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
    if (head == NULL || head->waterBoxes == NULL) {
        return 0;
    }
    for (i = 0; i < head->numWaterBoxes; ++i) {
        WaterBox* w = &head->waterBoxes[i];
        if (x >= w->xMin && x < w->xMin + w->xLength &&
            z >= w->zMin && z < w->zMin + w->zLength &&
            y <= (f32)w->ySurface + 2.0f) {
            return 1;
        }
    }
    return 0;
}

static SGKCell* SGK_GetCell(PlayState* play, s32 gx, s32 gz, s32 layer,
                            s32 spacing, f32 playerY) {
    u32 seed = SGK_CellSeed(gx, gz);
    u32 slot = SGK_Hash(seed ^ ((u32)layer * 0x27d4eb2dU)) & SGK_CACHE_MASK;
    SGKCell* cell = &sCells[slot];
    f32 x;
    f32 z;
    Vec3f pos;
    CollisionPoly* poly = NULL;
    s32 bgId = BG_ACTOR_MAX;
    f32 floorY;

    if (cell->valid && cell->gridX == gx && cell->gridZ == gz && cell->layer == layer) {
        return cell;
    }
    /* Cache direct-mapped: colisao apenas ao entrar em celula (ou substituicao). */
    cell->valid = 1;
    cell->occupied = 0;
    cell->gridX = gx;
    cell->gridZ = gz;
    cell->layer = layer;
    cell->seed = seed;
    sCacheMisses++;

    x = ((f32)gx + 0.20f + 0.60f * SGK_Unit(SGK_Hash(seed ^ 0xA77U))) * spacing;
    z = ((f32)gz + 0.20f + 0.60f * SGK_Unit(SGK_Hash(seed ^ 0xB91U))) * spacing;
    pos.x = x;
    pos.y = playerY + 90.0f;
    pos.z = z;

    floorY = BgCheck_EntityRaycastFloor3(&play->colCtx, &poly, &bgId, &pos);
    if (poly == NULL || bgId != BGCHECK_SCENE || floorY <= BGCHECK_Y_MIN + 1.0f) {
        return cell;
    }
    /* Nao plantar em terra de outro andar, plataformas acima ou vales profundos. */
    if (fabsf(floorY - playerY) > 65.0f) {
        return cell;
    }
    if (COLPOLY_GET_NORMAL(poly->normal.y) < 0.80f) {
        return cell;
    }
    if ((SurfaceType_GetData(&play->colCtx, poly, bgId, 1) & 0xFU) != SGK_FLOOR_GRASS_SFX) {
        return cell;
    }
    if (SGK_IsUnderwater(play, x, floorY, z)) {
        return cell;
    }
    cell->y = (s16)(floorY + 0.8f);
    cell->occupied = 1;
    return cell;
}

static void SGK_PutVertex(Vtx* v, f32 x, f32 y, f32 z, u8 r, u8 g, u8 b) {
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

/* Dois planos triangulares que se cruzam, com coloracao por vertice. */
static void SGK_BuildTuft(Vtx* verts, s32 cx, s32 cz, s16 cy, s32 spacing, u32 seed,
                          f32 phase, s32 wind) {
    f32 x = ((f32)cx + 0.20f + 0.60f * SGK_Unit(SGK_Hash(seed ^ 0xA77U))) * spacing;
    f32 z = ((f32)cz + 0.20f + 0.60f * SGK_Unit(SGK_Hash(seed ^ 0xB91U))) * spacing;
    f32 h = 8.0f + 6.0f * SGK_Unit(seed >> 3);
    f32 w = 3.0f + 1.7f * SGK_Unit(seed >> 11);
    f32 a = SGK_Unit(seed) * 6.28318530718f;
    f32 sway = wind ? (2.0f * sinf(phase + (f32)(seed & 255U) * 0.031f)) : 0.0f;
    u32 tint = (seed >> 17) & 15U;
    u8 r0 = (u8)(44 + tint);
    u8 g0 = (u8)(65 + tint);
    u8 b0 = (u8)(26 + tint / 2);
    u8 r1 = (u8)(99 + tint);
    u8 g1 = (u8)(130 + tint);
    u8 b1 = (u8)(55 + tint / 2);
    u32 k;

    for (k = 0; k < 2; ++k) {
        f32 ang = a + (f32)k * 1.57079632679f;
        f32 dx = cosf(ang);
        f32 dz = sinf(ang);
        f32 bendX = sway * (0.5f + k * 0.25f);
        f32 bendZ = sway * (0.28f + k * 0.45f);
        Vtx* v = verts + k * 3;
        SGK_PutVertex(&v[0], x - dx * w, (f32)cy, z - dz * w, r0, g0, b0);
        SGK_PutVertex(&v[1], x + dx * w, (f32)cy, z + dz * w, r0, g0, b0);
        SGK_PutVertex(&v[2], x + bendX + dx * (w * 0.30f), (f32)cy + h,
                      z + bendZ + dz * (w * 0.30f), r1, g1, b1);
    }
}

void SGKGrass_Draw(PlayState* play) {
    /* Ativado apenas neste fork experimental em Kokiri; preserva mods HD. */
    s32 quality;
    s32 spacing;
    s32 radius;
    s32 layer;
    s32 playerGx;
    s32 playerGz;
    s32 searchCells;
    s32 ix;
    s32 iz;
    s32 wind;
    f32 px;
    f32 pz;
    f32 py;
    u32 maxTufts;
    u32 tuftCount = 0;
    sFrameNumber++;

    if (play == NULL || play->sceneNum != SCENE_SPOT04 ||
        !CVarGetInteger("SGK.GrassEnabled", 1) || play->colCtx.colHeader == NULL) {
        return;
    }
    quality = CVarGetInteger("SGK.GrassQuality", 0);
    if (quality < 0) quality = 0;
    if (quality > 2) quality = 2;
    wind = CVarGetInteger("SGK.GrassWind", 1);
    spacing = (quality == 0) ? 30 : ((quality == 1) ? 24 : 19);
    radius = (quality == 0) ? 170 : ((quality == 1) ? 235 : 290);
    maxTufts = (quality == 0) ? 150u : ((quality == 1) ? 450u : 850u);
    px = GET_PLAYER(play)->actor.world.pos.x;
    py = GET_PLAYER(play)->actor.world.pos.y;
    pz = GET_PLAYER(play)->actor.world.pos.z;
    layer = SGK_GridFloor(py, 135);

    if (sCachedScene != play->sceneNum || sCachedQuality != quality) {
        SGK_ResetCache(play->sceneNum, quality);
    }
    if (sCachedLayer != layer) {
        SGK_ResetCache(play->sceneNum, quality);
        sCachedLayer = layer;
    }

    playerGx = SGK_GridFloor(px, spacing);
    playerGz = SGK_GridFloor(pz, spacing);
    searchCells = radius / spacing + 2;
    sDrawn = 0;

    OPEN_DISPS(play->state.gfxCtx);

    /* Renderizador Fast3D do proprio jogo: funciona no DX11 e no OpenGL. */
    POLY_OPA_DISP = Gfx_SetupDL(POLY_OPA_DISP, 25);
    gDPPipeSync(POLY_OPA_DISP++);
    gDPSetCycleType(POLY_OPA_DISP++, G_CYC_1CYCLE);
    gDPSetCombineMode(POLY_OPA_DISP++, G_CC_SHADE, G_CC_SHADE);
    gDPSetRenderMode(POLY_OPA_DISP++, G_RM_AA_ZB_OPA_SURF, G_RM_AA_ZB_OPA_SURF2);
    gSPTexture(POLY_OPA_DISP++, 0, 0, 0, G_TX_RENDERTILE, G_OFF);
    gSPClearGeometryMode(POLY_OPA_DISP++, G_LIGHTING | G_CULL_BOTH | G_TEXTURE_GEN | G_TEXTURE_GEN_LINEAR);
    gSPSetGeometryMode(POLY_OPA_DISP++, G_ZBUFFER | G_SHADE | G_SHADING_SMOOTH);
    Matrix_Push();
    Matrix_Identity();
    gSPMatrix(POLY_OPA_DISP++, MATRIX_NEWMTX(play->state.gfxCtx),
              G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    Matrix_Pop();

    /* Nao usa direcao da camera nem rotacao do jogador para distribuir/cortar. */
    for (iz = playerGz - searchCells; iz <= playerGz + searchCells; ++iz) {
        for (ix = playerGx - searchCells; ix <= playerGx + searchCells; ++ix) {
            f32 x = ((f32)ix + 0.5f) * spacing;
            f32 z = ((f32)iz + 0.5f) * spacing;
            f32 dx = x - px;
            f32 dz = z - pz;
            SGKCell* cell;
            Vtx* v;
            if (dx * dx + dz * dz > (f32)(radius * radius)) continue;
            cell = SGK_GetCell(play, ix, iz, layer, spacing, py);
            if (!cell->occupied) continue;
            if (tuftCount >= maxTufts) goto SGK_Done;

            v = Graph_Alloc(play->state.gfxCtx, sizeof(Vtx) * 6);
            if (v == NULL) goto SGK_Done;
            SGK_BuildTuft(v, ix, iz, cell->y, spacing, cell->seed,
                          (f32)sFrameNumber * 0.035f, wind);
            gSPVertex(POLY_OPA_DISP++, v, 6, 0);
            gSP2Triangles(POLY_OPA_DISP++, 0, 1, 2, 0, 3, 4, 5, 0);
            ++tuftCount;
        }
    }
SGK_Done:
    sDrawn = tuftCount;
    /* Restabelece estado visual para as chamadas seguintes. */
    gDPPipeSync(POLY_OPA_DISP++);
    POLY_OPA_DISP = Gfx_SetupDL(POLY_OPA_DISP, 25);
    CLOSE_DISPS(play->state.gfxCtx);

    if (CVarGetInteger("SGK.GrassDebug", 0) && (sFrameNumber % 300U) == 0U) {
        osSyncPrintf("[SGK] cena %d qualidade %d tufos %u cache_misses %u\n",
                     play->sceneNum, quality, sDrawn, sCacheMisses);
    }
}
