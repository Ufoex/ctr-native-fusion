/*
 * Derived from REDRIVER2/PsyCross MIT source:
 * externals/PsyCross/src/gpu/PsyX_GPU.cpp
 * See THIRD_PARTY_NOTICES.md for copyright and license details.
 */

#include <macros.h>
#include "platform/native_gpu.h"

#include <platform.h>

#include <SDL3/SDL.h>

#include "platform/native_log.h"
#include "platform/native_perf.h"
#include "platform/native_renderer.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void Platform_PollHostEvents(void);
extern int g_cfg_bilinearFiltering;
extern int g_dbg_emulatorPaused;
extern int g_dbg_polygonSelected;

#define NATIVE_GPU_LOG(fmt, ...)   Platform_Log("[CTR GPU] " fmt, __VA_ARGS__)
#define NATIVE_GPU_ERROR(fmt, ...) Platform_LogError("[CTR GPU] [%s] - " fmt, __func__, __VA_ARGS__)

// NOTE(aalhendi): Little-endian tag `CTRG` = CTR native GPU snapshot.
#define NATIVE_GPU_STATE_MAGIC     0x47525443
#define NATIVE_GPU_STATE_VERSION   1

#define GET_TPAGE_BLEND(tpage)     ((BlendMode)(((tpage >> 5) & 3) + 1))

#define GET_TPAGE_DITHER(tpage)    ((tpage >> 9) & 0x1)

#define GET_CLUT_X(clut)           ((clut & 0x3F) << 4)
#define GET_CLUT_Y(clut)           (clut >> 6)

internal TexFormat GetTPageFormat(int tpage)
{
	const int mode = (tpage >> 7) & 0x3;

	// NOTE(aalhendi): ctr-native local divergence from upstream PsyCross. PS1
	// mode 3 is reserved; TF_32_BIT_RGBA is only for explicit native override
	// textures, not raw retail TPAGE mode bits.
	return mode == 3 ? TF_16_BIT : (TexFormat)mode;
}

internal short GetTPageBase(int tpage)
{
	const u16 page = (u16)tpage;

	// NOTE(aalhendi): ctr-native local divergence for CTR retail emitters. The
	// shader wants xPage + yPage * 16, not raw draw-mode bits.
	return (s16)((page & 0xf) | ((page & 0x10) ? 0x10 : 0));
}

internal s16 NativeGpu_SignExtend11(u32 value)
{
	value &= 0x7ff;
	return (s16)((value ^ 0x400) - 0x400);
}

DISPENV activeDispEnv;
DRAWENV activeDrawEnv;
int g_GPUDisabledState = 0;

typedef struct
{
	DRAWENV drawenv;
	DISPENV dispenv;

	BlendMode blendMode;

	TexFormat texFormat;
	TextureID textureId;

	int drawPrimMode;
	bool psxTexturedSemiTrans;
	bool psxTextureOutputSTP;
	bool psxDrawMaskSet;

	u16 startVertex;
	u16 numVerts;

	const char *debugText;
} GPUDrawSplit;

#define MAX_DRAW_SPLITS 4096

typedef struct
{
	const char *currentSplitDebugText;
	TextureID overrideTexture;
	int overrideTextureWidth;
	int overrideTextureHeight;

	int drawPrimMode;
	bool psxDrawMaskSet;
	bool framebufferFeedbackRunActive;

	GrVertex vertexBuffer[MAX_VERTEX_BUFFER_SIZE];
	GPUDrawSplit splits[MAX_DRAW_SPLITS];
	int vertexIndex;
	int splitIndex;
} NativeGpuState;

global_variable NativeGpuState s_gpu;

struct NativeGpuSnapshot
{
	u32 magic;
	u32 version;
	u32 size;
	DRAWENV drawEnv;
	DISPENV dispEnv;
	s32 gpuDisabledState;
	s32 psxDrawMaskSet;
	u16 vram[VRAM_WIDTH * VRAM_HEIGHT];
};

int NativeGpu_HasPendingSplits(void)
{
	return s_gpu.splitIndex > 0;
}

//------------------------------------------------------------------------------------------------------------------------
// PGXP - independent reimplementation written for this port. The PGXP concept
// originates with iCatButler (PCSX-Reloaded, GPL-3.0); beetle-psx-libretro's
// pgxp/ (GPL-2.0) and DuckStation were studied as architectural references
// only - no code was copied. See CREDITS.md.
//
// PGXP (Parallel/Precision Geometry Transform Pipeline), textures only: the
// GTE keeps a high-precision side channel of every transformed vertex (see
// native_gte_core.c). The game copies the clamped s16 screen position into its
// primitives; those are matched back to the transform and the renderer is
// handed the view depth so it can interpolate texture coordinates
// perspective-correctly. Vertex positions stay PSX-exact and the GTE registers
// are never touched. 2D primitives are never eligible.
//
// Provenance is tracked by field ADDRESS (the "memory cache"): GTE stores bind
// the destination field to its transform, copies chain, and the prim writers
// bind the prim field. Coordinate matching on the push cache is only the
// fallback for paths the writers do not instrument.
//------------------------------------------------------------------------------------------------------------------------

typedef struct
{
	s16 sx, sy;
	u8 valid;
	u8 contested; // two different vertices wanted this slot within one frame
	u32 gen;
	u32 seq;      // monotonic push sequence
	u16 szLow;    // raw SZ register value (depth key for binding)
	float w; // view-space depth (SZ units)
} PgxpCachedVertex;

#define PGXP_CACHE_SIZE (65536)
#define PGXP_CACHE_WAYS (4)
#define PGXP_CACHE_SETS (PGXP_CACHE_SIZE / PGXP_CACHE_WAYS)

// Reject stale matches: entries older than this many epochs (~2 epochs advance
// per frame in this port) are ignored, so a spot vacated by a moving object
// does not hand its old depth to whatever lands there next.
#define PGXP_FRESH_WINDOW (16)

#define PGXP_SHADOW_SIZE (32768)
// Bind freshness is judged in frame eras (epochs), not push distance: this port
// transforms whole frames of vertices before the draw pass, so the
// transform->store distance routinely exceeds a frame of pushes.
// (sx, sy, SZ) uniqueness carries specificity; the era check only rejects old entries.
#define PGXP_SAMPLE_COUNT (24) // demoted GT3s listed once per run
#define PGXP_STATS_EPOCHS (120) // log coverage every this many frames
#define PGXP_BIND_EPOCHS (8)    // ~4 frames for the SZ-matched path
#define PGXP_XY_BIND_EPOCHS (2) // coordinate-only fallback: same frame only

// Table B: a prim field address -> the transform bound to it.
typedef struct
{
	const void *addr; // the prim field the value was stored into (&p->x0)
	s16 sx, sy;       // low-precision values as stored (validated at draw)
	u16 valid;
	float w; // view-space depth (SZ units)
} PgxpShadowEntry;

// Table A: a posScreen field address -> the transform bound to it right after
// the GTE store (CTR_GteStoreSXY*), consumed by the prim writers.
#define PGXP_TRANSFORM_SIZE (32768)
typedef struct
{
	const void *addr;
	s16 sx, sy;
	u16 valid;
	u16 gen; // epoch of the bind (the chain must consume it promptly)
	float w; // view-space depth (SZ units)
} PgxpTransformEntry;

internal PgxpCachedVertex s_pgxpCache[PGXP_CACHE_SIZE];
internal PgxpShadowEntry s_pgxpShadow[PGXP_SHADOW_SIZE];
internal PgxpTransformEntry s_pgxpTransform[PGXP_TRANSFORM_SIZE];
internal u32 s_pgxpPushSeq = 0;
internal u32 s_pgxpEpoch = 1;

// Sticky packed-source handoff between the DrawLevel vertex packer and the
// prim writer (adjacent expression; the packed value doubles as the guard).
// Only ever compared and used as an address key, never dereferenced.
internal uintptr_t s_pgxpPackedSrc = 0;
internal u32 s_pgxpPackedSrcValue = 0;

// Per-vertex side channel uploaded next to the vertex buffer: the view depth.
// 0 means "no high-precision source, stay affine".
internal float s_pgxpVertexData[MAX_VERTEX_BUFFER_SIZE];

// Coverage counters for the periodic log line (only advanced while PGXP is on).
// vertices: bound by address / by coordinate fallback / proven non-transform / unbound.
// prims: corrected, demoted to affine because a vertex was unbound, 2D (rect, line, screen-aligned quad).
internal struct
{
	u32 vtxAddr, vtxCoord, vtxNonXform, vtxMiss;
	u32 primOk, primDemoted, prim2D;
	// Per polygon kind (PGXP_KIND_*): primitives seen / left affine.
	u32 kindPrims[8], kindDemoted[8];
	// Why an unbound vertex missed, textured primitives only (PGXP_MISS_*).
	u32 missWhy[4];
	// Same textured misses, split by what the address table said (before any coordinate match).
	u32 missShadowNone, missShadowOverwritten;
} s_pgxpStats;

// Order matches the eight MakeVertex* call sites in ProcessFlatPoly / ProcessGouraudPoly.
enum
{
	PGXP_KIND_F3,
	PGXP_KIND_FT3,
	PGXP_KIND_F4,
	PGXP_KIND_FT4,
	PGXP_KIND_G3,
	PGXP_KIND_GT3,
	PGXP_KIND_G4,
	PGXP_KIND_GT4,
};

enum
{
	PGXP_MISS_NO_HOOK,     // no writer ever bound this field and no pixel match: unhooked code path
	PGXP_MISS_OVERWRITTEN, // bound once, but the field changed before the draw
	PGXP_MISS_CONTESTED,   // coordinate fallback refused: two objects shared the pixel
	PGXP_MISS_STALE,       // coordinate fallback refused: only an old frame had that pixel
};

internal int s_pgxpKind = PGXP_KIND_F3;
internal int s_pgxpShadowOverwritten = 0; // set by Pgxp_LookupShadow when it returns 0 for a changed field

int NativeGpu_PgxpEnabled(void)
{
#if defined(__ANDROID__)
	return 0;
#else
	return g_ctrds.pgxpTextures != 0;
#endif
}

internal u32 Pgxp_AddrHash(const void *addr)
{
	return ((u32)(uintptr_t)addr >> 2) * 2654435761u;
}

internal u32 Pgxp_CoordHash(s16 sx, s16 sy)
{
	return ((u32)(u16)sx * 73856093u) ^ ((u32)(u16)sy * 19349663u);
}

// Freshest push at these exact coordinates within maxWindow epochs. When szLow
// >= 0 the raw SZ register must match too: (sx, sy, SZ) is essentially unique,
// so a bind cannot land on a different vertex that shares the pixel.
internal const PgxpCachedVertex *Pgxp_FindFreshPush(s16 sx, s16 sy, int szLow, int maxWindow)
{
	const PgxpCachedVertex *set = &s_pgxpCache[(Pgxp_CoordHash(sx, sy) & (PGXP_CACHE_SETS - 1)) * PGXP_CACHE_WAYS];
	const PgxpCachedVertex *best = NULL;

	for (int way = 0; way < PGXP_CACHE_WAYS; way++)
	{
		const PgxpCachedVertex *c = &set[way];

		if ((c->valid == 0) || (c->sx != sx) || (c->sy != sy))
		{
			continue;
		}

		if ((szLow >= 0) && (c->szLow != (u16)szLow))
		{
			continue;
		}

		if ((s_pgxpEpoch - c->gen) > (u32)maxWindow)
		{
			continue;
		}

		if ((best == NULL) || (c->seq > best->seq))
		{
			best = c;
		}
	}

	return best;
}

// SZ-matched bind first; batched store paths (the SZ registers rotated past the
// vertex) fall back to a tight coordinate-only bind instead of refusing.
internal const PgxpCachedVertex *Pgxp_FindBindSource(s16 sx, s16 sy, int szLow)
{
	const PgxpCachedVertex *best = Pgxp_FindFreshPush(sx, sy, szLow, PGXP_BIND_EPOCHS);

	if (best == NULL)
	{
		best = Pgxp_FindFreshPush(sx, sy, -1, PGXP_XY_BIND_EPOCHS);
	}

	return best;
}

// Called right after a raw GTE SXY store into a projected-vertex field.
// szLow = the paired raw SZ register (MFC2 17/18/19), or -1 when unavailable.
void Pgxp_NoteTransformStore(void *field, u32 packed, int szLow)
{
	if (!NativeGpu_PgxpEnabled())
	{
		return;
	}

	const s16 sx = (s16)packed;
	const s16 sy = (s16)(packed >> 16);
	PgxpTransformEntry *e = &s_pgxpTransform[Pgxp_AddrHash(field) & (PGXP_TRANSFORM_SIZE - 1)];
	const PgxpCachedVertex *best = Pgxp_FindBindSource(sx, sy, szLow);

	e->addr = field;

	if (best != NULL)
	{
		e->sx = sx;
		e->sy = sy;
		e->w = best->w;
		e->valid = 1;
		e->gen = (u16)s_pgxpEpoch;
	}
	else
	{
		e->valid = 0;
	}
}

// posScreen field copy (value chain): the destination resolves to the same
// transform as the source.
void Pgxp_NoteTransformCopy(void *dstField, const void *srcField)
{
	if (!NativeGpu_PgxpEnabled())
	{
		return;
	}

	PgxpTransformEntry *d = &s_pgxpTransform[Pgxp_AddrHash(dstField) & (PGXP_TRANSFORM_SIZE - 1)];
	const PgxpTransformEntry *s = &s_pgxpTransform[Pgxp_AddrHash(srcField) & (PGXP_TRANSFORM_SIZE - 1)];

	if ((s->valid != 0) && (s->addr == srcField))
	{
		*d = *s;
		d->addr = dstField;
	}
	else
	{
		d->addr = dstField;
		d->valid = 0;
	}
}

// DrawLevel packer -> prim writer handoff.
void Pgxp_SetPackedSource(const void *field, u32 packed)
{
	if (!NativeGpu_PgxpEnabled())
	{
		return;
	}

	s_pgxpPackedSrc = (uintptr_t)field;
	s_pgxpPackedSrcValue = packed;
}

// Prim-field write: resolve the source (address chain, or the fresh push window
// when there is no source address) into a shadow entry for the destination field.
void Pgxp_NotePrimWrite(void *dstField, const void *srcField, u32 packed, int szLow)
{
	if (!NativeGpu_PgxpEnabled())
	{
		return;
	}

	const s16 sx = (s16)packed;
	const s16 sy = (s16)(packed >> 16);
	PgxpShadowEntry *e = &s_pgxpShadow[Pgxp_AddrHash(dstField) & (PGXP_SHADOW_SIZE - 1)];

	e->addr = dstField;
	e->valid = 0; // negative binding unless a source resolves below

	if (srcField != NULL)
	{
		const PgxpTransformEntry *s = &s_pgxpTransform[Pgxp_AddrHash(srcField) & (PGXP_TRANSFORM_SIZE - 1)];

		if ((s->valid != 0) && (s->addr == srcField) && (s->sx == sx) && (s->sy == sy) && ((s_pgxpEpoch - s->gen) <= 32))
		{
			e->sx = sx;
			e->sy = sy;
			e->w = s->w;
			e->valid = 1;
		}
		return;
	}

	const PgxpCachedVertex *best = Pgxp_FindBindSource(sx, sy, szLow);

	if (best != NULL)
	{
		e->sx = sx;
		e->sy = sy;
		e->w = best->w;
		e->valid = 1;
	}

	// Otherwise this address did NOT receive fresh GTE output (UI literal /
	// static / interpolated split vertex). The negative binding also clears any
	// older positive one, so a reused field cannot satisfy a stale match.
}

void Pgxp_NotePrimWriteSticky(void *dstField, u32 packed)
{
	if (!NativeGpu_PgxpEnabled())
	{
		return;
	}

	// Consume the sticky source only when the value matches exactly (the
	// packer/writer pair is adjacent); otherwise this write is not
	// transform-sourced.
	if ((s_pgxpPackedSrc != 0) && (s_pgxpPackedSrcValue == packed))
	{
		Pgxp_NotePrimWrite(dstField, (const void *)s_pgxpPackedSrc, packed, -1);
	}
	else
	{
		Pgxp_NotePrimWrite(dstField, NULL, packed, -1);
	}

	s_pgxpPackedSrc = 0;
	s_pgxpPackedSrcValue = 0;
}

// RenderBucket writers (post-transform, no source address available): bind
// against the fresh push window. szLow = the vertex's raw SZ register value
// when known, else -1.
void Pgxp_NoteSxyStore(const void *addrField, u32 packedSxy, int szLow)
{
	Pgxp_NotePrimWrite((void *)addrField, NULL, packedSxy, szLow);
}

// Returns 1 on a bound hit, -1 on a proven non-transform field, 0 when there is no binding.
internal int Pgxp_LookupShadow(const void *v, float *w)
{
	const PgxpShadowEntry *e = &s_pgxpShadow[Pgxp_AddrHash(v) & (PGXP_SHADOW_SIZE - 1)];

	if (e->addr != v)
	{
		return 0; // no binding for this field: fallback matching may apply
	}

	if (e->valid == 0)
	{
		return -1; // blocks coordinate matching too - the point of the memory cache
	}

	// The field must still hold the exact values that were bound (guards
	// against reuse/overwrite between the store and the draw).
	if ((e->sx != (s16)((const VERTTYPE *)v)[0]) || (e->sy != (s16)((const VERTTYPE *)v)[1]))
	{
		s_pgxpShadowOverwritten = 1;
		return 0;
	}

	*w = e->w;
	return 1;
}

void Pgxp_ClearCache(void)
{
	// Every table, so a toggle or a state load cannot let a pre-existing
	// association satisfy a lookup.
	memset(s_pgxpCache, 0, sizeof(s_pgxpCache));
	memset(s_pgxpShadow, 0, sizeof(s_pgxpShadow));
	memset(s_pgxpTransform, 0, sizeof(s_pgxpTransform));
	s_pgxpPackedSrc = 0;
	s_pgxpPackedSrcValue = 0;
}

void Pgxp_AdvanceEpoch(void)
{
	s_pgxpEpoch++;
}

// Called from the GTE perspective transform with the unclamped view depth (w,
// in SZ units). The integer registers are the PSX
// result; this is only the side channel.
void Pgxp_PushVertex(int sx, int sy, float w, int szLow)
{
	// w is the view-space depth and must stay positive for the renderer's
	// perspective divide.
	if (w <= 0.0f)
	{
		return;
	}

	PgxpCachedVertex *set = &s_pgxpCache[(Pgxp_CoordHash((s16)sx, (s16)sy) & (PGXP_CACHE_SETS - 1)) * PGXP_CACHE_WAYS];

	// 4-way set associative: update the same-pixel entry in place if present,
	// else take a free slot, else evict the oldest.
	PgxpCachedVertex *v = NULL;
	PgxpCachedVertex *oldest = &set[0];

	for (int way = 0; way < PGXP_CACHE_WAYS; way++)
	{
		PgxpCachedVertex *c = &set[way];

		if ((c->valid != 0) && (c->sx == (s16)sx) && (c->sy == (s16)sy))
		{
			v = c;
			break;
		}

		if ((v == NULL) && (c->valid == 0))
		{
			v = c;
		}

		if (c->gen < oldest->gen)
		{
			oldest = c;
		}
	}

	if (v == NULL)
	{
		v = oldest;
	}

	// True ambiguity = the same pixel, same frame, with a different depth: two
	// different objects projecting to the same point.
	const u8 samePixelThisEpoch = ((v->valid != 0) && (v->gen == s_pgxpEpoch) && (v->sx == (s16)sx) && (v->sy == (s16)sy)) ? 1 : 0;
	float wDelta = v->w - w;
	if (wDelta < 0.0f) { wDelta = -wDelta; }

	v->contested = (samePixelThisEpoch && (wDelta > 0.25f)) ? 1 : 0;
	v->sx = (s16)sx;
	v->sy = (s16)sy;
	v->valid = 1;
	v->gen = s_pgxpEpoch;
	v->seq = ++s_pgxpPushSeq;
	v->szLow = (u16)szLow;
	v->w = w;
}

// Coordinate fallback: EXACT match only (a +-1px tolerance matched unrelated
// geometry in the dense pool and smeared textures). Returns 1 on a hit (fills
// *w), 0 on a miss, 3 on an ambiguous pixel and 4 on a stale entry.
internal int Pgxp_LookupVertex(int x, int y, float *w)
{
	const PgxpCachedVertex *set = &s_pgxpCache[(Pgxp_CoordHash((s16)x, (s16)y) & (PGXP_CACHE_SETS - 1)) * PGXP_CACHE_WAYS];
	int refusal = 0;

	for (int way = 0; way < PGXP_CACHE_WAYS; way++)
	{
		const PgxpCachedVertex *v = &set[way];

		if ((v->valid == 0) || (v->sx != (s16)x) || (v->sy != (s16)y))
		{
			continue;
		}

		if (v->contested != 0)
		{
			if (refusal == 0)
			{
				refusal = 3; // ambiguous pixel: two objects wanted it this frame
			}
			continue;
		}

		if ((s_pgxpEpoch - v->gen) > PGXP_FRESH_WINDOW)
		{
			if (refusal == 0)
			{
				refusal = 4; // an object long gone from this pixel
			}
			continue;
		}

		*w = v->w;
		return 1;
	}

	return refusal;
}

internal void Pgxp_Mark2D(int base, int count)
{
	if ((base < 0) || ((base + count) > MAX_VERTEX_BUFFER_SIZE))
	{
		return;
	}

	for (int i = 0; i < count; i++)
	{
		s_pgxpVertexData[base + i] = 0.0f;
	}
}

// A primitive that is never PGXP-eligible (rect, line, screen-aligned quad).
internal void Pgxp_Mark2DPrim(int base, int count)
{
	Pgxp_Mark2D(base, count);

	if (NativeGpu_PgxpEnabled())
	{
		s_pgxpStats.prim2D++;
	}
}

// Fills one vertex: its view depth, or 0 (affine) when nothing trustworthy binds.
// Returns a one-letter status for the sample log: A address-bound, X proven
// non-transform, C coordinate-bound, N no table entry, O entry overwritten,
// K contested, S stale (the last four are misses).
internal char Pgxp_FillVertex(int index, VERTTYPE *p)
{
	float w = 0.0f;
	char status = 'A';

	// Address-keyed provenance first: exact, no coordinate matching involved.
	// A proven non-transform field (-1) stays affine and blocks the fallback.
	s_pgxpShadowOverwritten = 0;
	const int shadow = Pgxp_LookupShadow(p, &w);

	if (shadow == 1)
	{
		s_pgxpStats.vtxAddr++;
	}
	else if (shadow < 0)
	{
		s_pgxpStats.vtxNonXform++;
		status = 'X';
	}
	else
	{
		const int coord = Pgxp_LookupVertex(p[0], p[1], &w);

		if (coord == 1)
		{
			s_pgxpStats.vtxCoord++;
			status = 'C';
		}
		else
		{
			w = 0.0f;
			s_pgxpStats.vtxMiss++;
			status = (coord == 3) ? 'K' : (coord == 4) ? 'S' : s_pgxpShadowOverwritten ? 'O' : 'N';

			if ((s_pgxpKind & 1) != 0) // odd kinds are the textured ones
			{
				const int why = (coord == 3) ? PGXP_MISS_CONTESTED : (coord == 4) ? PGXP_MISS_STALE : s_pgxpShadowOverwritten ? PGXP_MISS_OVERWRITTEN : PGXP_MISS_NO_HOOK;

				s_pgxpStats.missWhy[why]++;

				if (s_pgxpShadowOverwritten)
				{
					s_pgxpStats.missShadowOverwritten++;
				}
				else
				{
					s_pgxpStats.missShadowNone++;
				}
			}
		}
	}

	s_pgxpVertexData[index] = w;
	return status;
}

internal void Pgxp_CopyVertex(int dstIndex, int srcIndex)
{
	s_pgxpVertexData[dstIndex] = s_pgxpVertexData[srcIndex];
}

// Consistency rule: a triangle must be corrected for ALL of its vertices or
// none. Mixing perspective and affine interpolation inside one triangle tears
// the texture apart.
internal void Pgxp_RequireConsistent(int base, int count)
{
	for (int i = 0; i < count; i++)
	{
		if (s_pgxpVertexData[base + i] == 0.0f)
		{
			Pgxp_Mark2D(base, count);
			s_pgxpStats.primDemoted++;
			s_pgxpStats.kindPrims[s_pgxpKind]++;
			s_pgxpStats.kindDemoted[s_pgxpKind]++;
			return;
		}
	}

	s_pgxpStats.primOk++;
	s_pgxpStats.kindPrims[s_pgxpKind]++;
}

// Resolve a primitive's vertices (all affine when PGXP is off).
internal void Pgxp_FillPrimitive(int base, VERTTYPE **p, int count)
{
	if (!NativeGpu_PgxpEnabled())
	{
		Pgxp_Mark2D(base, count);
		return;
	}

	char status[4];
	const u32 demotedBefore = s_pgxpStats.primDemoted;

	for (int i = 0; i < count; i++)
	{
		status[i] = Pgxp_FillVertex(base + i, p[i]);
	}

	Pgxp_RequireConsistent(base, count);

	// First few affine-demoted GT3s of the run: where they live and why each vertex missed.
	if ((s_pgxpStats.primDemoted != demotedBefore) && (s_pgxpKind == PGXP_KIND_GT3))
	{
		local_persist int sampled = 0;

		if (sampled < PGXP_SAMPLE_COUNT)
		{
			sampled++;
			Platform_LogWarn("[CTR PGXP] sample GT3 #%d at %p: v0=%c(%d,%d) v1=%c(%d,%d) v2=%c(%d,%d) [A addr, C coord, N no entry, O overwritten, K contested, S stale]\n", sampled,
			                 (void *)p[0], status[0], (int)p[0][0], (int)p[0][1], status[1], (int)p[1][0], (int)p[1][1], status[2], (int)p[2][0], (int)p[2][1]);
		}
	}
}

void ClearSplits(void)
{
	s_gpu.currentSplitDebugText = NULL;
	s_gpu.vertexIndex = 0;
	s_gpu.splitIndex = 0;
	s_gpu.splits[0].texFormat = (TexFormat)0xFFFF;
	s_gpu.splits[0].psxTexturedSemiTrans = false;
	s_gpu.splits[0].psxTextureOutputSTP = false;
	s_gpu.splits[0].psxDrawMaskSet = false;
	s_gpu.framebufferFeedbackRunActive = false;

	if (NativeGpu_PgxpEnabled() && ((s_pgxpEpoch % PGXP_STATS_EPOCHS) == 0))
	{
		const u32 prims = s_pgxpStats.primOk + s_pgxpStats.primDemoted + s_pgxpStats.prim2D;
		const u32 eligible = s_pgxpStats.primOk + s_pgxpStats.primDemoted;

		Platform_LogWarn("[CTR PGXP] %u frames: prims=%u 2D=%u 3D=%u corrected=%u (%.1f%% of 3D) affine-demoted=%u | vertices: addr=%u coord=%u non-xform=%u miss=%u\n",
		                 (unsigned)PGXP_STATS_EPOCHS, prims, s_pgxpStats.prim2D, eligible, s_pgxpStats.primOk,
		                 eligible ? (100.0 * s_pgxpStats.primOk / eligible) : 0.0, s_pgxpStats.primDemoted, s_pgxpStats.vtxAddr, s_pgxpStats.vtxCoord,
		                 s_pgxpStats.vtxNonXform, s_pgxpStats.vtxMiss);
		u32 texPrims = 0, texDemoted = 0, flatDemoted = 0;

		for (int k = 0; k < 8; k++)
		{
			if ((k & 1) != 0)
			{
				texPrims += s_pgxpStats.kindPrims[k];
				texDemoted += s_pgxpStats.kindDemoted[k];
			}
			else
			{
				flatDemoted += s_pgxpStats.kindDemoted[k];
			}
		}

		Platform_LogWarn("[CTR PGXP]   textured: prims=%u affine=%u (%.1f%%) [FT3=%u/%u GT3=%u/%u FT4=%u/%u GT4=%u/%u] | untextured affine=%u | textured vertex misses: no-hook=%u overwritten=%u contested=%u stale=%u | address table said: none=%u overwritten=%u\n",
		                 texPrims, texDemoted, texPrims ? (100.0 * texDemoted / texPrims) : 0.0, s_pgxpStats.kindDemoted[1], s_pgxpStats.kindPrims[1],
		                 s_pgxpStats.kindDemoted[5], s_pgxpStats.kindPrims[5], s_pgxpStats.kindDemoted[3], s_pgxpStats.kindPrims[3], s_pgxpStats.kindDemoted[7],
		                 s_pgxpStats.kindPrims[7], flatDemoted, s_pgxpStats.missWhy[PGXP_MISS_NO_HOOK], s_pgxpStats.missWhy[PGXP_MISS_OVERWRITTEN],
		                 s_pgxpStats.missWhy[PGXP_MISS_CONTESTED], s_pgxpStats.missWhy[PGXP_MISS_STALE], s_pgxpStats.missShadowNone, s_pgxpStats.missShadowOverwritten);
		memset(&s_pgxpStats, 0, sizeof(s_pgxpStats));
	}

	// Frame boundary for PGXP matching: all lookups of this frame happened
	// before this point (draw-time parsing), next frame's pushes come after.
	Pgxp_AdvanceEpoch();
}

int NativeGpu_GetStateSize(void)
{
	return (int)sizeof(struct NativeGpuSnapshot);
}

int NativeGpu_CaptureState(void *dst, int dstSize)
{
	struct NativeGpuSnapshot *snapshot = (struct NativeGpuSnapshot *)dst;

	if ((dst == NULL) || (dstSize < (int)sizeof(*snapshot)))
	{
		return 0;
	}
	if (NativeRenderer_GetVRAMStateSize() != (int)sizeof(snapshot->vram))
	{
		return 0;
	}

	memset(snapshot, 0, sizeof(*snapshot));
	snapshot->magic = NATIVE_GPU_STATE_MAGIC;
	snapshot->version = NATIVE_GPU_STATE_VERSION;
	snapshot->size = sizeof(*snapshot);
	snapshot->drawEnv = activeDrawEnv;
	snapshot->dispEnv = activeDispEnv;
	snapshot->gpuDisabledState = g_GPUDisabledState;
	snapshot->psxDrawMaskSet = s_gpu.psxDrawMaskSet;

	return NativeRenderer_CaptureVRAMState(snapshot->vram, sizeof(snapshot->vram));
}

int NativeGpu_RestoreState(const void *src, int srcSize)
{
	const struct NativeGpuSnapshot *snapshot = (const struct NativeGpuSnapshot *)src;

	if ((src == NULL) || (srcSize < (int)sizeof(*snapshot)))
	{
		return 0;
	}
	if ((snapshot->magic != NATIVE_GPU_STATE_MAGIC) || (snapshot->version != NATIVE_GPU_STATE_VERSION) || (snapshot->size != sizeof(*snapshot)))
	{
		return 0;
	}
	if ((snapshot->gpuDisabledState < 0) || (snapshot->gpuDisabledState > 1) || (snapshot->psxDrawMaskSet < 0) || (snapshot->psxDrawMaskSet > 1))
	{
		return 0;
	}
	if (NativeRenderer_GetVRAMStateSize() != (int)sizeof(snapshot->vram))
	{
		return 0;
	}
	if (!NativeRenderer_RestoreVRAMState(snapshot->vram, sizeof(snapshot->vram)))
	{
		return 0;
	}

	activeDrawEnv = snapshot->drawEnv;
	activeDispEnv = snapshot->dispEnv;
	g_GPUDisabledState = snapshot->gpuDisabledState;
	s_gpu.psxDrawMaskSet = snapshot->psxDrawMaskSet;
	ClearSplits();
	return 1;
}

internal void DrawEnvDimensionsInt(int *width, int *height)
{
	if (activeDrawEnv.dfe)
	{
		*width = activeDispEnv.disp.w;
		*height = activeDispEnv.disp.h;
	}
	else
	{
		*width = activeDrawEnv.clip.w;
		*height = activeDrawEnv.clip.h;
	}
}

void DrawEnvOffset(float *ofsX, float *ofsY)
{
	if (activeDrawEnv.dfe)
	{
		int w, h;
		DrawEnvDimensionsInt(&w, &h);

		if (w <= 0)
		{
			w = 1;
		}

		// NOTE(aalhendi): Convert PS1 VRAM-page draw offsets into display-relative host-screen offsets.
		// CTR alternates draw pages at y=0 and y=0x128; using raw modulo VRAM offsets shifts every other native frame vertically.
		*ofsX = activeDrawEnv.ofs[0] - activeDispEnv.disp.x;
		*ofsY = activeDrawEnv.ofs[1] - activeDispEnv.disp.y;
	}
	else
	{
		*ofsX = activeDrawEnv.ofs[0] - activeDrawEnv.clip.x;
		*ofsY = activeDrawEnv.ofs[1] - activeDrawEnv.clip.y;
	}
}

void LineSwapSourceVerts(VERTTYPE **p0, VERTTYPE **p1, u8 **c0, u8 **c1)
{
	// swap line coordinates for left-to-right and up-to-bottom direction
	if (((*p0)[0] > (*p1)[0]) || ((*p0)[1] > (*p1)[1] && (*p0)[0] == (*p1)[0]))
	{
		VERTTYPE *tmp = *p0;
		*p0 = *p1;
		*p1 = tmp;

		u8 *tmpCol = *c0;
		*c0 = *c1;
		*c1 = tmpCol;
	}
}

void MakeLineArray(GrVertex *vertex, VERTTYPE *p0, VERTTYPE *p1)
{
	const VERTTYPE dx = p1[0] - p0[0];
	const VERTTYPE dy = p1[1] - p0[1];

	float ofsX, ofsY;
	DrawEnvOffset(&ofsX, &ofsY);

	memset(vertex, 0, sizeof(GrVertex) * 4);
	Pgxp_Mark2DPrim((int)(vertex - s_gpu.vertexBuffer), 4);

	if (dx > abs((s16)dy))
	{ // horizontal
		vertex[0].x = p0[0] + ofsX;
		vertex[0].y = p0[1] + ofsY;

		vertex[1].x = p1[0] + ofsX + 1;
		vertex[1].y = p1[1] + ofsY;

		vertex[2].x = vertex[1].x;
		vertex[2].y = vertex[1].y + 1;

		vertex[3].x = vertex[0].x;
		vertex[3].y = vertex[0].y + 1;
	}
	else
	{ // vertical
		vertex[0].x = p0[0] + ofsX;
		vertex[0].y = p0[1] + ofsY;

		vertex[1].x = p1[0] + ofsX;
		vertex[1].y = p1[1] + ofsY + 1;

		vertex[2].x = vertex[1].x + 1;
		vertex[2].y = vertex[1].y;

		vertex[3].x = vertex[0].x + 1;
		vertex[3].y = vertex[0].y;
	} // TODO diagonal line alignment
}

// NOTE(ctrds): the PSX GPU refuses to draw a primitive spanning more than 1023
// pixels horizontally or 511 vertically, and games rely on that: a vertex that
// projects far off-screen produces a huge bogus polygon which the hardware
// simply discards. Nothing here enforced that, so those polygons were drawn --
// stray wedges cutting across the sky and the scene edges. Widescreen makes it
// more common, because the wider frustum admits geometry closer to the camera
// at the edges, where projections blow up.
//
// Collapsing the primitive to a point is the cheapest way to drop it: it keeps
// the packet walk and the vertex counting exactly as they were, and a zero-area
// primitive rasterises nothing.
#define PSX_PRIM_MAX_W 1023.0f
#define PSX_PRIM_MAX_H 511.0f

internal void RejectOversizedPrimitive(GrVertex *vertex, int count)
{
	float minX = vertex[0].x;
	float maxX = vertex[0].x;
	float minY = vertex[0].y;
	float maxY = vertex[0].y;
	int i;

	for (i = 1; i < count; i++)
	{
		if (vertex[i].x < minX) { minX = vertex[i].x; }
		if (vertex[i].x > maxX) { maxX = vertex[i].x; }
		if (vertex[i].y < minY) { minY = vertex[i].y; }
		if (vertex[i].y > maxY) { maxY = vertex[i].y; }
	}

	const float spanX = maxX - minX;
	const float spanY = maxY - minY;

	if (!g_ctrds.primReject)
	{
		return;
	}

	if ((spanX <= PSX_PRIM_MAX_W) && (spanY <= PSX_PRIM_MAX_H))
	{
		return;
	}

	// Reporting the span matters more than the count: a stray wedge from a
	// vertex that projected into nowhere spans thousands of pixels, while a wall
	// the camera has simply got close to only just crosses the line. If the
	// spans being dropped are the second kind, the limit is wrong here rather
	// than the geometry.
	if (g_ctrds.primReject > 1)
	{
		local_persist int reported = 0;

		if (reported < 64)
		{
			reported++;
			Platform_Log("[CTR-DS/prim] dropped span %.0fx%.0f at %.0f,%.0f\n", (double)spanX, (double)spanY, (double)minX, (double)minY);
		}
	}

	for (i = 1; i < count; i++)
	{
		vertex[i].x = vertex[0].x;
		vertex[i].y = vertex[0].y;
	}
}

void MakeVertexTriangle(GrVertex *vertex, VERTTYPE *p0, VERTTYPE *p1, VERTTYPE *p2)
{
	assert(p0);
	assert(p1);
	assert(p2);

	float ofsX, ofsY;
	DrawEnvOffset(&ofsX, &ofsY);

	memset(vertex, 0, sizeof(GrVertex) * 3);

	{
		VERTTYPE *pgxpSrc[3] = {p0, p1, p2};
		Pgxp_FillPrimitive(s_gpu.vertexIndex, pgxpSrc, 3);
	}

	vertex[0].x = p0[0] + ofsX;
	vertex[0].y = p0[1] + ofsY;

	vertex[1].x = p1[0] + ofsX;
	vertex[1].y = p1[1] + ofsY;

	vertex[2].x = p2[0] + ofsX;
	vertex[2].y = p2[1] + ofsY;

	RejectOversizedPrimitive(vertex, 3);
}

void MakeVertexQuad(GrVertex *vertex, VERTTYPE *p0, VERTTYPE *p1, VERTTYPE *p2, VERTTYPE *p3)
{
	assert(p0);
	assert(p1);
	assert(p2);
	assert(p3);

	float ofsX, ofsY;
	DrawEnvOffset(&ofsX, &ofsY);

	memset(vertex, 0, sizeof(GrVertex) * 4);

	// A screen-aligned integer quad (PSX order TL, TR, BR, BL) is a sprite/UI
	// element in practice and never came from a GTE projection: keep it affine
	// instead of risking an exact-coordinate match with an unrelated 3D vertex.
	if ((p0[0] == p3[0]) && (p1[0] == p2[0]) && (p0[1] == p1[1]) && (p2[1] == p3[1]))
	{
		Pgxp_Mark2DPrim(s_gpu.vertexIndex, 4);
	}
	else
	{
		VERTTYPE *pgxpSrc[4] = {p0, p1, p2, p3};
		Pgxp_FillPrimitive(s_gpu.vertexIndex, pgxpSrc, 4);
	}

	vertex[0].x = p0[0] + ofsX;
	vertex[0].y = p0[1] + ofsY;

	vertex[1].x = p1[0] + ofsX;
	vertex[1].y = p1[1] + ofsY;

	vertex[2].x = p2[0] + ofsX;
	vertex[2].y = p2[1] + ofsY;

	vertex[3].x = p3[0] + ofsX;
	vertex[3].y = p3[1] + ofsY;

	RejectOversizedPrimitive(vertex, 4);
}

void MakeVertexRect(GrVertex *vertex, VERTTYPE *p0, s16 w, s16 h)
{
	assert(p0);

	float ofsX, ofsY;
	DrawEnvOffset(&ofsX, &ofsY);

	memset(vertex, 0, sizeof(GrVertex) * 4);

	// Rects (TILE/SPRT) are 2D elements: never eligible. Without this, stale
	// depth left in the rolling side-channel buffer by an earlier 3D primitive
	// would leak into HUD sprites. TriangulateQuad copies the slots.
	Pgxp_Mark2DPrim((int)(vertex - s_gpu.vertexBuffer), 4);

	vertex[0].x = p0[0] + ofsX;
	vertex[0].y = p0[1] + ofsY;

	vertex[1].x = vertex[0].x;
	vertex[1].y = vertex[0].y + h;

	vertex[2].x = vertex[0].x + w;
	vertex[2].y = vertex[0].y + h;

	vertex[3].x = vertex[0].x + w;
	vertex[3].y = vertex[0].y;
}

void MakeTexcoordQuad(GrVertex *vertex, u8 *uv0, u8 *uv1, u8 *uv2, u8 *uv3, s16 page, s16 clut, u8 dither)
{
	assert(uv0);
	assert(uv1);
	assert(uv2);
	assert(uv3);

	const u8 bright = 2;
	const short texPage = GetTPageBase(page);

	vertex[0].u = uv0[0];
	vertex[0].v = uv0[1];
	vertex[0].bright = bright;
	vertex[0].dither = dither;
	vertex[0].page = texPage;
	vertex[0].clut = clut;

	vertex[1].u = uv1[0];
	vertex[1].v = uv1[1];
	vertex[1].bright = bright;
	vertex[1].dither = dither;
	vertex[1].page = texPage;
	vertex[1].clut = clut;

	vertex[2].u = uv2[0];
	vertex[2].v = uv2[1];
	vertex[2].bright = bright;
	vertex[2].dither = dither;
	vertex[2].page = texPage;
	vertex[2].clut = clut;

	vertex[3].u = uv3[0];
	vertex[3].v = uv3[1];
	vertex[3].bright = bright;
	vertex[3].dither = dither;
	vertex[3].page = texPage;
	vertex[3].clut = clut;
	/*
	if (g_cfg_bilinearFiltering)
	{
	    vertex[0].tcx = -1;
	    vertex[0].tcy = -1;

	    vertex[1].tcx = -1;
	    vertex[1].tcy = -1;

	    vertex[2].tcx = -1;
	    vertex[2].tcy = -1;

	    vertex[3].tcx = -1;
	    vertex[3].tcy = -1;
	}*/
}

void MakeTexcoordTriangle(GrVertex *vertex, u8 *uv0, u8 *uv1, u8 *uv2, s16 page, s16 clut, u8 dither)
{
	assert(uv0);
	assert(uv1);
	assert(uv2);

	const u8 bright = 2;
	const short texPage = GetTPageBase(page);

	vertex[0].u = uv0[0];
	vertex[0].v = uv0[1];
	vertex[0].bright = bright;
	vertex[0].dither = dither;
	vertex[0].page = texPage;
	vertex[0].clut = clut;

	vertex[1].u = uv1[0];
	vertex[1].v = uv1[1];
	vertex[1].bright = bright;
	vertex[1].dither = dither;
	vertex[1].page = texPage;
	vertex[1].clut = clut;

	vertex[2].u = uv2[0];
	vertex[2].v = uv2[1];
	vertex[2].bright = bright;
	vertex[2].dither = dither;
	vertex[2].page = texPage;
	vertex[2].clut = clut;
	/*
	if (g_cfg_bilinearFiltering)
	{
	    vertex[0].tcx = -1;
	    vertex[0].tcy = -1;

	    vertex[1].tcx = -1;
	    vertex[1].tcy = -1;

	    vertex[2].tcx = -1;
	    vertex[2].tcy = -1;

	    vertex[3].tcx = -1;
	    vertex[3].tcy = -1;
	}*/
}

void MakeTexcoordRect(GrVertex *vertex, u8 *uv, s16 page, s16 clut, s16 w, s16 h)
{
	assert(uv);

	// sim overflow
	if ((int)uv[0] + w > 255)
	{
		w = 255 - uv[0];
	}
	if ((int)uv[1] + h > 255)
	{
		h = 255 - uv[1];
	}

	const u8 bright = 2;
	const u8 dither = 0;
	const short texPage = GetTPageBase(page);

	vertex[0].u = uv[0];
	vertex[0].v = uv[1];
	vertex[0].bright = bright;
	vertex[0].dither = dither;
	vertex[0].page = texPage;
	vertex[0].clut = clut;

	vertex[1].u = uv[0];
	vertex[1].v = uv[1] + h;
	vertex[1].bright = bright;
	vertex[1].dither = dither;
	vertex[1].page = texPage;
	vertex[1].clut = clut;

	vertex[2].u = uv[0] + w;
	vertex[2].v = uv[1] + h;
	vertex[2].bright = bright;
	vertex[2].dither = dither;
	vertex[2].page = texPage;
	vertex[2].clut = clut;

	vertex[3].u = uv[0] + w;
	vertex[3].v = uv[1];
	vertex[3].bright = bright;
	vertex[3].dither = dither;
	vertex[3].page = texPage;
	vertex[3].clut = clut;

	if (g_cfg_bilinearFiltering)
	{
		vertex[0].tcx = -1;
		vertex[0].tcy = -1;

		vertex[1].tcx = -1;
		vertex[1].tcy = -1;

		vertex[2].tcx = -1;
		vertex[2].tcy = -1;

		vertex[3].tcx = -1;
		vertex[3].tcy = -1;
	}
}

void MakeTexcoordLineZero(GrVertex *vertex, u8 dither)
{
	const u8 bright = 1;

	vertex[0].u = 0;
	vertex[0].v = 0;
	vertex[0].bright = bright;
	vertex[0].dither = dither;
	vertex[0].page = 0;
	vertex[0].clut = 0;

	vertex[1].u = 0;
	vertex[1].v = 0;
	vertex[1].bright = bright;
	vertex[1].dither = dither;
	vertex[1].page = 0;
	vertex[1].clut = 0;

	vertex[2].u = 0;
	vertex[2].v = 0;
	vertex[2].bright = bright;
	vertex[2].dither = dither;
	vertex[2].page = 0;
	vertex[2].clut = 0;

	vertex[3].u = 0;
	vertex[3].v = 0;
	vertex[3].bright = bright;
	vertex[3].dither = dither;
	vertex[3].page = 0;
	vertex[3].clut = 0;
}

void MakeTexcoordTriangleZero(GrVertex *vertex, u8 dither)
{
	const u8 bright = 1;

	vertex[0].u = 0;
	vertex[0].v = 0;
	vertex[0].bright = bright;
	vertex[0].dither = dither;
	vertex[0].page = 0;
	vertex[0].clut = 0;

	vertex[1].u = 0;
	vertex[1].v = 0;
	vertex[1].bright = bright;
	vertex[1].dither = dither;
	vertex[1].page = 0;
	vertex[1].clut = 0;

	vertex[2].u = 0;
	vertex[2].v = 0;
	vertex[2].bright = bright;
	vertex[2].dither = dither;
	vertex[2].page = 0;
	vertex[2].clut = 0;
}

void MakeTexcoordQuadZero(GrVertex *vertex, u8 dither)
{
	const u8 bright = 1;

	vertex[0].u = 0;
	vertex[0].v = 0;
	vertex[0].bright = bright;
	vertex[0].dither = dither;
	vertex[0].page = 0;
	vertex[0].clut = 0;

	vertex[1].u = 0;
	vertex[1].v = 0;
	vertex[1].bright = bright;
	vertex[1].dither = dither;
	vertex[1].page = 0;
	vertex[1].clut = 0;

	vertex[2].u = 0;
	vertex[2].v = 0;
	vertex[2].bright = bright;
	vertex[2].dither = dither;
	vertex[2].page = 0;
	vertex[2].clut = 0;

	vertex[3].u = 0;
	vertex[3].v = 0;
	vertex[3].bright = bright;
	vertex[3].dither = dither;
	vertex[3].page = 0;
	vertex[3].clut = 0;
}

void MakeColourNoShade(GrVertex *vertex, int n)
{
	--n;
	while (n >= 0)
	{
		vertex[n].r = 128;
		vertex[n].g = 128;
		vertex[n].b = 128;
		vertex[n].a = 255;
		--n;
	}
}

void MakeColourLine(GrVertex *vertex, bool shadeTexOn, u8 *col0, u8 *col1)
{
	if (!shadeTexOn)
	{
		MakeColourNoShade(vertex, 4);
		return;
	}
	assert(col0);
	assert(col1);

	vertex[0].r = col0[0];
	vertex[0].g = col0[1];
	vertex[0].b = col0[2];
	vertex[0].a = 255;

	vertex[1].r = col1[0];
	vertex[1].g = col1[1];
	vertex[1].b = col1[2];
	vertex[1].a = 255;

	vertex[2].r = col1[0];
	vertex[2].g = col1[1];
	vertex[2].b = col1[2];
	vertex[2].a = 255;

	vertex[3].r = col0[0];
	vertex[3].g = col0[1];
	vertex[3].b = col0[2];
	vertex[3].a = 255;
}

void MakeColourTriangle(GrVertex *vertex, bool shadeTexOn, u8 *col0, u8 *col1, u8 *col2)
{
	if (!shadeTexOn)
	{
		MakeColourNoShade(vertex, 3);
		return;
	}

	assert(col0);
	assert(col1);
	assert(col2);

	vertex[0].r = col0[0];
	vertex[0].g = col0[1];
	vertex[0].b = col0[2];
	vertex[0].a = 255;

	vertex[1].r = col1[0];
	vertex[1].g = col1[1];
	vertex[1].b = col1[2];
	vertex[1].a = 255;

	vertex[2].r = col2[0];
	vertex[2].g = col2[1];
	vertex[2].b = col2[2];
	vertex[2].a = 255;
}

void MakeColourQuad(GrVertex *vertex, bool shadeTexOn, u8 *col0, u8 *col1, u8 *col2, u8 *col3)
{
	if (!shadeTexOn)
	{
		MakeColourNoShade(vertex, 4);
		return;
	}

	assert(col0);
	assert(col1);
	assert(col2);
	assert(col3);

	vertex[0].r = col0[0];
	vertex[0].g = col0[1];
	vertex[0].b = col0[2];
	vertex[0].a = 255;

	vertex[1].r = col1[0];
	vertex[1].g = col1[1];
	vertex[1].b = col1[2];
	vertex[1].a = 255;

	vertex[2].r = col2[0];
	vertex[2].g = col2[1];
	vertex[2].b = col2[2];
	vertex[2].a = 255;

	vertex[3].r = col3[0];
	vertex[3].g = col3[1];
	vertex[3].b = col3[2];
	vertex[3].a = 255;
}

void TriangulateQuad()
{
	/*
	Triangulate like this:

	v0--v1
	|  / |
	| /  |
	v2--v3

	NOTE: v2 swapped with v3 during primitive parsing but it not shown here
	*/

	s_gpu.vertexBuffer[s_gpu.vertexIndex + 4] = s_gpu.vertexBuffer[s_gpu.vertexIndex + 3];

	s_gpu.vertexBuffer[s_gpu.vertexIndex + 5] = s_gpu.vertexBuffer[s_gpu.vertexIndex + 2];
	s_gpu.vertexBuffer[s_gpu.vertexIndex + 2] = s_gpu.vertexBuffer[s_gpu.vertexIndex + 3];
	s_gpu.vertexBuffer[s_gpu.vertexIndex + 3] = s_gpu.vertexBuffer[s_gpu.vertexIndex + 1];

	Pgxp_CopyVertex(s_gpu.vertexIndex + 4, s_gpu.vertexIndex + 3);
	Pgxp_CopyVertex(s_gpu.vertexIndex + 5, s_gpu.vertexIndex + 2);
	Pgxp_CopyVertex(s_gpu.vertexIndex + 2, s_gpu.vertexIndex + 3);
	Pgxp_CopyVertex(s_gpu.vertexIndex + 3, s_gpu.vertexIndex + 1);
}

//------------------------------------------------------------------------------------------------------------------------

internal bool NativeGpu_RectOverlaps(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh)
{
	return (aw > 0) && (ah > 0) && (bw > 0) && (bh > 0) && (ax < bx + bw) && (bx < ax + aw) && (ay < by + bh) && (by < ay + ah);
}

internal bool NativeGpu_TPageOverlapsActiveDrawPage(int tpage)
{
	const int pageX = (tpage & 0xf) << 6;
	const int pageY = (tpage & 0x10) ? 0x100 : 0;
	const int pageW = 0x100;
	const int pageH = 0x100;

	if (!activeDrawEnv.dfe)
	{
		return false;
	}

	if (GetTPageFormat(tpage) != TF_16_BIT)
	{
		return false;
	}

	return NativeGpu_RectOverlaps(pageX, pageY, pageW, pageH, activeDrawEnv.clip.x, activeDrawEnv.clip.y, activeDrawEnv.clip.w, activeDrawEnv.clip.h);
}

internal void NativeGpu_PrepareFramebufferFeedback(int tpage)
{
	if (!NativeGpu_TPageOverlapsActiveDrawPage(tpage))
	{
		return;
	}

	if (s_gpu.framebufferFeedbackRunActive)
	{
		return;
	}

	// NOTE(aalhendi): PS1 can draw into VRAM and immediately texture from that
	// same draw page. Native batches primitives, so screen-feedback effects
	// like heat warp need an explicit barrier before their framebuffer-sampling
	// polygons consume the VRAM texture.
	if (NativeGpu_HasPendingSplits())
	{
		DrawAllSplits();
	}

	NativeRenderer_StoreFrameBuffer(activeDrawEnv.clip.x, activeDrawEnv.clip.y, activeDrawEnv.clip.w, activeDrawEnv.clip.h);
	s_gpu.framebufferFeedbackRunActive = true;
}

internal void AddSplit(bool semiTrans, bool textured, bool framebufferFeedback)
{
	int tpage = activeDrawEnv.tpage;

	if (framebufferFeedback)
	{
		NativeGpu_PrepareFramebufferFeedback(tpage);
	}
	else
	{
		s_gpu.framebufferFeedbackRunActive = false;
	}

	GPUDrawSplit *curSplit = &s_gpu.splits[s_gpu.splitIndex];

	BlendMode blendMode = semiTrans ? GET_TPAGE_BLEND(tpage) : BM_NONE;
	TexFormat texFormat = GetTPageFormat(tpage);
	TextureID textureId = textured ? NativeRenderer_GetVRAMTexture() : NativeRenderer_GetWhiteTexture();
	bool psxTexturedSemiTrans = semiTrans && textured && s_gpu.overrideTexture == 0;
	// NOTE(aalhendi): PS1 framebuffer bit 15 follows sampled texture STP for
	// textured draws unless E6 forces it. Recursive screen-copy effects depend
	// on this bit surviving after the blended textured pass.
	bool psxTextureOutputSTP = textured && s_gpu.overrideTexture == 0;

	if (textured && s_gpu.overrideTexture != 0)
	{
		// override texture format, zero tpage
		texFormat = TF_32_BIT_RGBA;
		textureId = s_gpu.overrideTexture;
		psxTexturedSemiTrans = false;
	}

	// FIXME: compare drawing environment too?
	if (!psxTexturedSemiTrans && curSplit->blendMode == blendMode && curSplit->texFormat == texFormat && curSplit->textureId == textureId &&
	    curSplit->drawPrimMode == s_gpu.drawPrimMode && curSplit->psxTexturedSemiTrans == psxTexturedSemiTrans &&
	    curSplit->psxTextureOutputSTP == psxTextureOutputSTP && curSplit->psxDrawMaskSet == s_gpu.psxDrawMaskSet &&
	    curSplit->drawenv.clip.x == activeDrawEnv.clip.x && curSplit->drawenv.clip.y == activeDrawEnv.clip.y &&
	    curSplit->drawenv.clip.w == activeDrawEnv.clip.w && curSplit->drawenv.clip.h == activeDrawEnv.clip.h && curSplit->drawenv.dfe == activeDrawEnv.dfe &&
	    curSplit->debugText == s_gpu.currentSplitDebugText)
	{
		return;
	}

	curSplit->numVerts = s_gpu.vertexIndex - curSplit->startVertex;

	if (s_gpu.splitIndex + 1 >= MAX_DRAW_SPLITS)
	{
		NATIVE_GPU_ERROR("%s\n", "MAX_DRAW_SPLITS reached (too many blend modes, texture formats, drawEnv clip rects, dfe switches), expect rendering errors");
		return;
	}

	GPUDrawSplit *split = &s_gpu.splits[++s_gpu.splitIndex];
	split->blendMode = blendMode;
	split->texFormat = texFormat;
	split->textureId = textureId;
	split->drawPrimMode = s_gpu.drawPrimMode;
	split->psxTexturedSemiTrans = psxTexturedSemiTrans;
	split->psxTextureOutputSTP = psxTextureOutputSTP;
	split->psxDrawMaskSet = s_gpu.psxDrawMaskSet;
	split->drawenv = activeDrawEnv;
	split->dispenv = activeDispEnv;
	split->debugText = s_gpu.currentSplitDebugText;

	split->drawenv.tw.w = s_gpu.overrideTextureWidth;
	split->drawenv.tw.h = s_gpu.overrideTextureHeight;

	split->startVertex = s_gpu.vertexIndex;
	split->numVerts = 0;
}

void DrawSplit(const GPUDrawSplit *split)
{
	if (split->debugText)
	{
		NativeRenderer_PushDebugLabel(split->debugText);
	}

	const bool drawOnScreen = split->drawenv.dfe;
	if ((split->drawenv.clip.w <= 0) || (split->drawenv.clip.h <= 0))
	{
		// NOTE(aalhendi): VS/Battle end previews can shrink a losing viewport
		// into an empty retail draw area. Empty clips should consume no pixels,
		// and must not leak stale native offscreen/scissor state.
		NativeRenderer_SetupClipMode(&split->drawenv.clip, &split->dispenv, drawOnScreen);
		NativeRenderer_SetOffscreenState(&split->drawenv.clip, 0);
		if (split->debugText)
		{
			NativeRenderer_PopDebugLabel();
		}
		return;
	}

	NativeRenderer_SetStencilMode(split->drawPrimMode); // draw with mask 0x16

	NativeRenderer_SetTexture(split->textureId, split->texFormat);

	if (split->texFormat == TF_32_BIT_RGBA)
	{
		NativeRenderer_SetOverrideTextureSize(split->drawenv.tw.w, split->drawenv.tw.h);
	}

	NativeRenderer_SetPSXDrawMaskSet(split->psxDrawMaskSet);
	NativeRenderer_SetPSXTextureOutputSTP(split->psxTextureOutputSTP);

	NativeRenderer_SetupClipMode(&split->drawenv.clip, &split->dispenv, drawOnScreen);
	NativeRenderer_SetOffscreenState(&split->drawenv.clip, !drawOnScreen);
	NativeRenderer_SetProjection(&split->drawenv.clip, &split->dispenv, !drawOnScreen);

	if (split->psxTexturedSemiTrans)
	{
		// NOTE(aalhendi): CTR native renderer divergence from upstream PsyCross.
		// PS1 textured ABE only blends texels whose sampled 16-bit color has STP
		// set; non-STP texels remain opaque. Native split state is per draw,
		// so draw this primitive-sized split twice with shader-side STP masks.
		NativeRenderer_SetBlendMode(BM_NONE);
		NativeRenderer_SetPSXTextureSemiTransPass(1);
		NativeRenderer_DrawTriangles(split->startVertex, split->numVerts / 3);

		NativeRenderer_SetBlendMode(split->blendMode);
		NativeRenderer_SetPSXTextureSemiTransPass(2);
		NativeRenderer_DrawTriangles(split->startVertex, split->numVerts / 3);

		NativeRenderer_SetPSXTextureSemiTransPass(0);
	}
	else
	{
		NativeRenderer_SetBlendMode(split->blendMode);
		NativeRenderer_SetPSXTextureSemiTransPass(0);
		NativeRenderer_DrawTriangles(split->startVertex, split->numVerts / 3);
	}

	if (split->debugText)
	{
		NativeRenderer_PopDebugLabel();
	}
}

internal void SetPSXMaskState(u32 code)
{
	s_gpu.psxDrawMaskSet = (code & 1) != 0;
}

//
// Draws all polygons after AggregatePTAG
//
void DrawAllSplits()
{
	NativePerf_BeginScope(NATIVE_PERF_BUCKET_DRAW_ALL_SPLITS);
	// CPU-originated LoadImage, MoveImage, and fill commands are GPU-visible
	// before the next draw batch, matching PS1 command ordering.
	NativeRenderer_UpdateVRAM();
#ifdef CTR_INTERNAL
	if (g_dbg_emulatorPaused)
	{
		for (int i = 0; i < 3; i++)
		{
			GrVertex *vert = &s_gpu.vertexBuffer[g_dbg_polygonSelected + i];
			vert->r = 255;
			vert->g = 0;
			vert->b = 0;

			NATIVE_GPU_LOG("%s\n", "==========================================");
			NATIVE_GPU_LOG("POLYGON: %d\n", g_dbg_polygonSelected);
			NATIVE_GPU_LOG("X: %d Y: %d\n", vert->x, vert->y);
			NATIVE_GPU_LOG("U: %d V: %d\n", vert->u, vert->v);
			NATIVE_GPU_LOG("TP: %d CLT: %d\n", vert->page, vert->clut);

			NATIVE_GPU_LOG("%s\n", "==========================================");
		}

		Platform_PollHostEvents();
	}
#endif

	// next code ideally should be called before EndScene
	NativeRenderer_UpdateVertexBuffer(s_gpu.vertexBuffer, s_gpu.vertexIndex, s_pgxpVertexData);

	for (int i = 1; i <= s_gpu.splitIndex; i++)
	{
		DrawSplit(&s_gpu.splits[i]);
	}

	ClearSplits();
	NativePerf_EndScope(NATIVE_PERF_BUCKET_DRAW_ALL_SPLITS);
}

// forward declarations
int ParsePrimitive(P_TAG *polyTag);
int ParseTaglessPrimitive(u32 *command);

internal bool NativeGpu_IsValidOTLink(uintptr_t link)
{
	if (NativeGpuLinks_IsRegisteredHostPointer((const void *)link))
	{
		return (link & (sizeof(u32) - 1)) == 0;
	}

	return false;
}

internal u32 NativeGpu_ReadPacketWordForLog(uintptr_t packet, int wordIndex)
{
	const struct PlatformMempackArena *arena = Platform_GetMempackArena();
	const uintptr_t word = packet + (uintptr_t)wordIndex * sizeof(u32);
	const uintptr_t end = word + sizeof(u32);

	if ((NativeGpuLinks_IsRegisteredHostRange((const void *)word, sizeof(u32))) && ((word & (sizeof(u32) - 1)) == 0))
	{
		return *(const u32 *)word;
	}

	if ((word < (uintptr_t)arena->base) || (end > (uintptr_t)arena->endOfMemory) || ((word & (sizeof(u32) - 1)) != 0))
	{
		return 0xffffffffu;
	}

	return *(const u32 *)word;
}

internal void NativeGpu_FormatPointerRegion(char *dst, size_t dstSize, uintptr_t ptr)
{
	if ((sdata == NULL) || (sdata->gGT == NULL))
	{
		snprintf(dst, dstSize, "no-gGT");
		return;
	}

	struct GameTracker *gGT = sdata->gGT;

	for (int playerIndex = 0; playerIndex < 4; playerIndex++)
	{
		struct PushBuffer *pb = &gGT->pushBuffer[playerIndex];
		const uintptr_t start = (uintptr_t)pb->ptrOT;
		const uintptr_t end = (uintptr_t)pb->renderBucketOTRangeEnd;
		if ((start != 0) && (end != 0) && (ptr >= start) && (ptr <= end))
		{
			snprintf(dst, dstSize, "pb%d.ot+0x%zx", playerIndex, (size_t)(ptr - start));
			return;
		}
	}

	struct PushBuffer *uiPB = &gGT->pushBuffer_UI;
	const uintptr_t uiStart = (uintptr_t)uiPB->ptrOT;
	const uintptr_t uiEnd = (uintptr_t)uiPB->renderBucketOTRangeEnd;
	if ((uiStart != 0) && (uiEnd != 0) && (ptr >= uiStart) && (ptr <= uiEnd))
	{
		snprintf(dst, dstSize, "ui.ot+0x%zx", (size_t)(ptr - uiStart));
		return;
	}

	for (int dbIndex = 0; dbIndex < 2; dbIndex++)
	{
		struct DB *db = &gGT->db[dbIndex];
		const uintptr_t primStart = (uintptr_t)db->primMem.start;
		const uintptr_t primEnd = (uintptr_t)db->primMem.end;
		if ((primStart != 0) && (ptr >= primStart) && (ptr < primEnd))
		{
			snprintf(dst, dstSize, "db%d.prim+0x%zx", dbIndex, (size_t)(ptr - primStart));
			return;
		}

		const uintptr_t otStart = (uintptr_t)db->otMem.start;
		const uintptr_t otEnd = (uintptr_t)db->otMem.end;
		if ((otStart != 0) && (ptr >= otStart) && (ptr < otEnd))
		{
			snprintf(dst, dstSize, "db%d.ot+0x%zx", dbIndex, (size_t)(ptr - otStart));
			return;
		}
	}

	snprintf(dst, dstSize, "unknown");
}

void ParsePrimitivesLinkedList(u32 *p, int singlePrimitive)
{
	if (!p)
	{
		return;
	}

	NativePerf_BeginScope(NATIVE_PERF_BUCKET_DRAW_OTAG_PARSE);

#ifdef CTR_NATIVE
	if (!singlePrimitive && !NativeGpuLinks_IsRegisteredHostPointer(p) && !isendprim(p))
	{
		char packetRegion[64];
		NativeGpu_FormatPointerRegion(packetRegion, sizeof(packetRegion), (uintptr_t)p);
		NATIVE_GPU_ERROR("unregistered linked DrawOTag packet: packet=%p region=%s addr=%06x len=%d code=%02x words=%08x %08x %08x %08x\n", (void *)p,
		                 packetRegion, getaddr(p), getlen(p), getcode(p), NativeGpu_ReadPacketWordForLog((uintptr_t)p, 0),
		                 NativeGpu_ReadPacketWordForLog((uintptr_t)p, 1), NativeGpu_ReadPacketWordForLog((uintptr_t)p, 2),
		                 NativeGpu_ReadPacketWordForLog((uintptr_t)p, 3));
		NativePerf_EndScope(NATIVE_PERF_BUCKET_DRAW_OTAG_PARSE);
		return;
	}
#endif

	// setup single primitive flag (needed for AddSplits)
	s_gpu.drawPrimMode = singlePrimitive;

	if (singlePrimitive)
	{
		P_TAG *polyTag = (P_TAG *)p;
		ParsePrimitive(polyTag);

		GPUDrawSplit *lastSplit = &s_gpu.splits[s_gpu.splitIndex];
		lastSplit->numVerts = s_gpu.vertexIndex - lastSplit->startVertex;
	}
	else
	{
		// walk OT_TAG linked list
		u8 *basePacket = (u8 *)p;
		while (true)
		{
			const int tagLength = getlen(basePacket);
			if (tagLength > 0)
			{
				if (tagLength > 32)
				{
					char packetRegion[64];
					NativeGpu_FormatPointerRegion(packetRegion, sizeof(packetRegion), (uintptr_t)basePacket);
					NATIVE_GPU_ERROR("got invalid tag length %d, code %d packet=%p region=%s words=%08x %08x %08x %08x\n", tagLength,
					                 ((P_TAG *)basePacket)->code, (void *)basePacket, packetRegion, NativeGpu_ReadPacketWordForLog((uintptr_t)basePacket, 0),
					                 NativeGpu_ReadPacketWordForLog((uintptr_t)basePacket, 1), NativeGpu_ReadPacketWordForLog((uintptr_t)basePacket, 2),
					                 NativeGpu_ReadPacketWordForLog((uintptr_t)basePacket, 3));
					break;
				}

				u8 *currentPacket = basePacket;
				u8 *endPacket = basePacket + (tagLength + P_LEN) * sizeof(u32);
				int primLength = 0;
				if (currentPacket < endPacket)
				{
					primLength = ParsePrimitive((P_TAG *)currentPacket);
					currentPacket += (primLength + P_LEN) * sizeof(u32);
				}

				while (currentPacket < endPacket)
				{
					primLength = ParseTaglessPrimitive((u32 *)currentPacket);
					currentPacket += primLength * sizeof(u32);
				}

				if (currentPacket != endPacket)
				{
					char packetRegion[64];
					NativeGpu_FormatPointerRegion(packetRegion, sizeof(packetRegion), (uintptr_t)basePacket);
					NATIVE_GPU_ERROR("did not output valid primitive or ptag length is not valid (diff=%d packet=%p region=%s words=%08x %08x %08x %08x)\n",
					                 endPacket - currentPacket, (void *)basePacket, packetRegion, NativeGpu_ReadPacketWordForLog((uintptr_t)basePacket, 0),
					                 NativeGpu_ReadPacketWordForLog((uintptr_t)basePacket, 1), NativeGpu_ReadPacketWordForLog((uintptr_t)basePacket, 2),
					                 NativeGpu_ReadPacketWordForLog((uintptr_t)basePacket, 3));
				}
			}

			GPUDrawSplit *lastSplit = &s_gpu.splits[s_gpu.splitIndex];
			lastSplit->numVerts = s_gpu.vertexIndex - lastSplit->startVertex;

			if (isendprim(basePacket))
			{
				break;
			}

			u8 *nextPacket = nextPrim(basePacket);
			if (!NativeGpu_IsValidOTLink((uintptr_t)nextPacket))
			{
				char packetRegion[64];
				char nextRegion[64];
				NativeGpu_FormatPointerRegion(packetRegion, sizeof(packetRegion), (uintptr_t)basePacket);
				NativeGpu_FormatPointerRegion(nextRegion, sizeof(nextRegion), (uintptr_t)nextPacket);
				NATIVE_GPU_ERROR("invalid OT link: packet=%p region=%s addr=%06x next=%p nextRegion=%s len=%d code=%02x words=%08x %08x %08x %08x\n",
				                 (void *)basePacket, packetRegion, getaddr(basePacket), (void *)nextPacket, nextRegion, getlen(basePacket), getcode(basePacket),
				                 NativeGpu_ReadPacketWordForLog((uintptr_t)basePacket, 0), NativeGpu_ReadPacketWordForLog((uintptr_t)basePacket, 1),
				                 NativeGpu_ReadPacketWordForLog((uintptr_t)basePacket, 2), NativeGpu_ReadPacketWordForLog((uintptr_t)basePacket, 3));
				break;
			}

			basePacket = nextPacket;
		}
	}

	NativePerf_EndScope(NATIVE_PERF_BUCKET_DRAW_OTAG_PARSE);
}

internal inline int IsNull(POLY_FT3 *poly)
{
	return poly->x0 == -1 && poly->y0 == -1 && poly->x1 == -1 && poly->y1 == -1 && poly->x2 == -1 && poly->y2 == -1;
}

internal int ProcessFlatLines(P_TAG *polyTag)
{
	const bool shadeTexOn = true;
	const bool semiTrans = (polyTag->code & 2);
	const int primSubType = polyTag->code & 0x0C;

	switch (primSubType)
	{
	case 0x0:
	{
		LINE_F2 *poly = (LINE_F2 *)polyTag;

		AddSplit(semiTrans, false, false);

		VERTTYPE *p0 = &poly->x0;
		VERTTYPE *p1 = &poly->x1;
		u8 *c0 = &poly->r0;
		u8 *c1 = c0;

		GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
		LineSwapSourceVerts(&p0, &p1, &c0, &c1);
		MakeLineArray(firstVertex, p0, p1);
		MakeTexcoordLineZero(firstVertex, 0);
		MakeColourLine(firstVertex, shadeTexOn, c0, c1);

		TriangulateQuad();

		s_gpu.vertexIndex += 6;

		return 3;
	}
	case 0x8: // TODO (unused)
	{
		LINE_F3 *poly = (LINE_F3 *)polyTag;

		AddSplit(semiTrans, false, false);

		{
			VERTTYPE *p0 = &poly->x0;
			VERTTYPE *p1 = &poly->x1;
			u8 *c0 = &poly->r0;
			u8 *c1 = c0;

			GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
			LineSwapSourceVerts(&p0, &p1, &c0, &c1);
			MakeLineArray(firstVertex, p0, p1);
			MakeTexcoordLineZero(firstVertex, 0);
			MakeColourLine(firstVertex, shadeTexOn, c0, c1);

			TriangulateQuad();

			s_gpu.vertexIndex += 6;
		}

		{
			VERTTYPE *p0 = &poly->x1;
			VERTTYPE *p1 = &poly->x2;
			u8 *c0 = &poly->r0;
			u8 *c1 = c0;

			GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
			LineSwapSourceVerts(&p0, &p1, &c0, &c1);
			MakeLineArray(firstVertex, p0, p1);
			MakeTexcoordLineZero(firstVertex, 0);
			MakeColourLine(firstVertex, shadeTexOn, c0, c1);

			TriangulateQuad();

			s_gpu.vertexIndex += 6;
		}

		return 5;
	}
	case 0xc:
	{
		LINE_F4 *poly = (LINE_F4 *)polyTag;

		AddSplit(semiTrans, false, false);

		{
			VERTTYPE *p0 = &poly->x0;
			VERTTYPE *p1 = &poly->x1;
			u8 *c0 = &poly->r0;
			u8 *c1 = c0;

			GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
			LineSwapSourceVerts(&p0, &p1, &c0, &c1);
			MakeLineArray(firstVertex, p0, p1);
			MakeTexcoordLineZero(firstVertex, 0);
			MakeColourLine(firstVertex, shadeTexOn, c0, c1);

			TriangulateQuad();

			s_gpu.vertexIndex += 6;
		}

		{
			VERTTYPE *p0 = &poly->x1;
			VERTTYPE *p1 = &poly->x2;
			u8 *c0 = &poly->r0;
			u8 *c1 = c0;

			GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
			LineSwapSourceVerts(&p0, &p1, &c0, &c1);
			MakeLineArray(firstVertex, p0, p1);
			MakeTexcoordLineZero(firstVertex, 0);
			MakeColourLine(firstVertex, shadeTexOn, c0, c1);

			TriangulateQuad();

			s_gpu.vertexIndex += 6;
		}

		{
			VERTTYPE *p0 = &poly->x2;
			VERTTYPE *p1 = &poly->x3;
			u8 *c0 = &poly->r0;
			u8 *c1 = c0;

			GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
			LineSwapSourceVerts(&p0, &p1, &c0, &c1);
			MakeLineArray(firstVertex, p0, p1);
			MakeTexcoordLineZero(firstVertex, 0);
			MakeColourLine(firstVertex, shadeTexOn, c0, c1);

			TriangulateQuad();

			s_gpu.vertexIndex += 6;
		}

		return 6;
	}
	}
	return 0;
}

internal int ProcessGouraudLines(P_TAG *polyTag)
{
	const bool shadeTexOn = true;
	const bool semiTrans = (polyTag->code & 2);
	const int primSubType = polyTag->code & 0x0C;

	switch (primSubType)
	{
	case 0x0:
	{
		LINE_G2 *poly = (LINE_G2 *)polyTag;

		AddSplit(semiTrans, false, false);

		VERTTYPE *p0 = &poly->x0;
		VERTTYPE *p1 = &poly->x1;
		u8 *c0 = &poly->r0;
		u8 *c1 = &poly->r1;

		GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
		LineSwapSourceVerts(&p0, &p1, &c0, &c1);
		MakeLineArray(firstVertex, p0, p1);
		MakeTexcoordLineZero(firstVertex, 0);
		MakeColourLine(firstVertex, shadeTexOn, c0, c1);

		TriangulateQuad();

		s_gpu.vertexIndex += 6;

		return 4;
	}
	case 0x8:
	{
		// TODO: LINE_G3
		return 7;
	}
	case 0xC:
	{
		// TODO: LINE_G4
		return 9;
	}
	}
	return 0;
}

internal int ProcessFlatPoly(P_TAG *polyTag)
{
	const bool shadeTexOn = (polyTag->code & 1) == 0;
	const bool semiTrans = (polyTag->code & 2);
	const int primSubType = polyTag->code & 0x0C;

	switch (primSubType)
	{
	case 0x0:
	{
		POLY_F3 *poly = (POLY_F3 *)polyTag;

		AddSplit(semiTrans, false, false);

		GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
		s_pgxpKind = PGXP_KIND_F3;
		MakeVertexTriangle(firstVertex, &poly->x0, &poly->x1, &poly->x2);
		MakeTexcoordTriangleZero(firstVertex, 0);
		MakeColourTriangle(firstVertex, shadeTexOn, &poly->r0, &poly->r0, &poly->r0);

		s_gpu.vertexIndex += 3;

		return 4;
	}
	case 0x4:
	{
		POLY_FT3 *poly = (POLY_FT3 *)polyTag;
		activeDrawEnv.tpage = poly->tpage;

		// It is an official hack from SCE devs to not use DR_TPAGE and instead use null polygon
		if (!IsNull(poly))
		{
			AddSplit(semiTrans, true, NativeGpu_TPageOverlapsActiveDrawPage(poly->tpage));

			GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
			s_pgxpKind = PGXP_KIND_FT3;
			MakeVertexTriangle(firstVertex, &poly->x0, &poly->x1, &poly->x2);
			MakeTexcoordTriangle(firstVertex, &poly->u0, &poly->u1, &poly->u2, poly->tpage, poly->clut,
			                     GET_TPAGE_DITHER(activeDrawEnv.tpage) || activeDrawEnv.dtd);
			MakeColourTriangle(firstVertex, shadeTexOn, &poly->r0, &poly->r0, &poly->r0);

			s_gpu.vertexIndex += 3;
		}
		return 7;
	}
	case 0x8:
	{
		POLY_F4 *poly = (POLY_F4 *)polyTag;

		AddSplit(semiTrans, false, false);

		GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
		s_pgxpKind = PGXP_KIND_F4;
		MakeVertexQuad(firstVertex, &poly->x0, &poly->x1, &poly->x3, &poly->x2);
		MakeTexcoordQuadZero(firstVertex, 0);
		MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r0, &poly->r0, &poly->r0);

		TriangulateQuad();

		s_gpu.vertexIndex += 6;
		return 5;
	}
	case 0xC:
	{
		POLY_FT4 *poly = (POLY_FT4 *)polyTag;
		activeDrawEnv.tpage = poly->tpage;

		AddSplit(semiTrans, true, NativeGpu_TPageOverlapsActiveDrawPage(poly->tpage));

		GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
		s_pgxpKind = PGXP_KIND_FT4;
		MakeVertexQuad(firstVertex, &poly->x0, &poly->x1, &poly->x3, &poly->x2);
		MakeTexcoordQuad(firstVertex, &poly->u0, &poly->u1, &poly->u3, &poly->u2, poly->tpage, poly->clut,
		                 GET_TPAGE_DITHER(activeDrawEnv.tpage) || activeDrawEnv.dtd);
		MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r0, &poly->r0, &poly->r0);

		TriangulateQuad();

		s_gpu.vertexIndex += 6;

		return 9;
	}
	}
	return 0;
}

internal int ProcessGouraudPoly(P_TAG *polyTag)
{
	const bool shadeTexOn = true;
	const bool semiTrans = (polyTag->code & 2);
	const int primSubType = polyTag->code & 0x0C;

	switch (primSubType)
	{
	case 0x0:
	{
		POLY_G3 *poly = (POLY_G3 *)polyTag;

		AddSplit(semiTrans, false, false);

		GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
		s_pgxpKind = PGXP_KIND_G3;
		MakeVertexTriangle(firstVertex, &poly->x0, &poly->x1, &poly->x2);
		MakeTexcoordTriangleZero(firstVertex, 1);
		MakeColourTriangle(firstVertex, shadeTexOn, &poly->r0, &poly->r1, &poly->r2);

		s_gpu.vertexIndex += 3;

		return 6;
	}
	case 0x4:
	{
		POLY_GT3 *poly = (POLY_GT3 *)polyTag;
		activeDrawEnv.tpage = poly->tpage;

		AddSplit(semiTrans, true, NativeGpu_TPageOverlapsActiveDrawPage(poly->tpage));

		GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
		s_pgxpKind = PGXP_KIND_GT3;
		MakeVertexTriangle(firstVertex, &poly->x0, &poly->x1, &poly->x2);
		MakeTexcoordTriangle(firstVertex, &poly->u0, &poly->u1, &poly->u2, poly->tpage, poly->clut, GET_TPAGE_DITHER(activeDrawEnv.tpage) || activeDrawEnv.dtd);
		MakeColourTriangle(firstVertex, shadeTexOn, &poly->r0, &poly->r1, &poly->r2);

		s_gpu.vertexIndex += 3;

		return 9;
	}
	case 0x8:
	{
		POLY_G4 *poly = (POLY_G4 *)polyTag;

		AddSplit(semiTrans, false, false);

		GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
		s_pgxpKind = PGXP_KIND_G4;
		MakeVertexQuad(firstVertex, &poly->x0, &poly->x1, &poly->x3, &poly->x2);
		MakeTexcoordQuadZero(firstVertex, 1);
		MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r1, &poly->r3, &poly->r2);

		TriangulateQuad();

		s_gpu.vertexIndex += 6;

		return 8;
	}
	case 0xC:
	{
		POLY_GT4 *poly = (POLY_GT4 *)polyTag;
		activeDrawEnv.tpage = poly->tpage;

		AddSplit(semiTrans, true, NativeGpu_TPageOverlapsActiveDrawPage(poly->tpage));

		GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
		s_pgxpKind = PGXP_KIND_GT4;
		MakeVertexQuad(firstVertex, &poly->x0, &poly->x1, &poly->x3, &poly->x2);
		MakeTexcoordQuad(firstVertex, &poly->u0, &poly->u1, &poly->u3, &poly->u2, poly->tpage, poly->clut,
		                 GET_TPAGE_DITHER(activeDrawEnv.tpage) || activeDrawEnv.dtd);
		MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r1, &poly->r3, &poly->r2);

		TriangulateQuad();

		s_gpu.vertexIndex += 6;

		return 12;
	}
	}
	return 0;
}

internal int ProcessTileAndSprt(P_TAG *polyTag)
{
	// NOTE: TILE does not support switching shadeTex on real PSX
	const bool shadeTexOn = (polyTag->code & 1) == 0;
	const bool semiTrans = (polyTag->code & 2);

	switch (polyTag->code & 0xFD)
	{
	case 0x60:
	{
		TILE *poly = (TILE *)polyTag;

		AddSplit(semiTrans, false, false);

		GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
		MakeVertexRect(firstVertex, &poly->x0, poly->w, poly->h);
		MakeTexcoordQuadZero(firstVertex, 0);
		MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r0, &poly->r0, &poly->r0);

		TriangulateQuad();

		s_gpu.vertexIndex += 6;

		return 3;
	}
	case 0x64:
	{
		SPRT *poly = (SPRT *)polyTag;

		AddSplit(semiTrans, true, NativeGpu_TPageOverlapsActiveDrawPage(activeDrawEnv.tpage));

		GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
		MakeVertexRect(firstVertex, &poly->x0, poly->w, poly->h);
		MakeTexcoordRect(firstVertex, &poly->u0, activeDrawEnv.tpage, poly->clut, poly->w, poly->h);
		MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r0, &poly->r0, &poly->r0);

		TriangulateQuad();

		s_gpu.vertexIndex += 6;

		return 4;
	}
	case 0x68:
	{
		TILE_1 *poly = (TILE_1 *)polyTag;

		AddSplit(semiTrans, false, false);

		GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
		MakeVertexRect(firstVertex, &poly->x0, 1, 1);
		MakeTexcoordQuadZero(firstVertex, 0);
		MakeColourQuad(firstVertex, true, &poly->r0, &poly->r0, &poly->r0, &poly->r0);

		TriangulateQuad();

		s_gpu.vertexIndex += 6;

		return 2;
	}
	case 0x70:
	{
		TILE_8 *poly = (TILE_8 *)polyTag;

		AddSplit(semiTrans, false, false);

		GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
		MakeVertexRect(firstVertex, &poly->x0, 8, 8);
		MakeTexcoordQuadZero(firstVertex, 0);
		MakeColourQuad(firstVertex, true, &poly->r0, &poly->r0, &poly->r0, &poly->r0);

		TriangulateQuad();

		s_gpu.vertexIndex += 6;

		return 2;
	}
	case 0x74:
	{
		SPRT_8 *poly = (SPRT_8 *)polyTag;

		AddSplit(semiTrans, true, NativeGpu_TPageOverlapsActiveDrawPage(activeDrawEnv.tpage));

		GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
		MakeVertexRect(firstVertex, &poly->x0, 8, 8);
		MakeTexcoordRect(firstVertex, &poly->u0, activeDrawEnv.tpage, poly->clut, 8, 8);
		MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r0, &poly->r0, &poly->r0);

		TriangulateQuad();

		s_gpu.vertexIndex += 6;

		return 3;
	}
	case 0x78:
	{
		TILE_16 *poly = (TILE_16 *)polyTag;

		AddSplit(semiTrans, false, false);

		GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
		MakeVertexRect(firstVertex, &poly->x0, 16, 16);
		MakeTexcoordQuadZero(firstVertex, 0);
		MakeColourQuad(firstVertex, true, &poly->r0, &poly->r0, &poly->r0, &poly->r0);

		TriangulateQuad();

		s_gpu.vertexIndex += 6;

		return 2;
	}
	case 0x7C:
	{
		SPRT_16 *poly = (SPRT_16 *)polyTag;

		AddSplit(semiTrans, true, NativeGpu_TPageOverlapsActiveDrawPage(activeDrawEnv.tpage));

		GrVertex *firstVertex = &s_gpu.vertexBuffer[s_gpu.vertexIndex];
		MakeVertexRect(firstVertex, &poly->x0, 16, 16);
		MakeTexcoordRect(firstVertex, &poly->u0, activeDrawEnv.tpage, poly->clut, 16, 16);
		MakeColourQuad(firstVertex, shadeTexOn, &poly->r0, &poly->r0, &poly->r0, &poly->r0);

		TriangulateQuad();

		s_gpu.vertexIndex += 6;

		return 3;
	}
	}
	return 0;
}

internal int ProcessDrawEnv(P_TAG *polyTag)
{
	const u32 *codePtr = (u32 *)&polyTag->pad0;
	int processedLongs = 0;
	bool fullDrawEnvPacket = false;
	for (int i = 0; i < polyTag->len; ++i)
	{
		const u32 code = codePtr[i];
		const int primType = code >> 24 & 0xF0;
		const int primSubType = code >> 24 & 0x0F;

		// NOTE(aalhendi): CTR can pack draw-env commands, tagless geometry,
		// and more draw-env commands into one OT entry. Stop at the first
		// non-E command so ParseTaglessPrimitive owns the geometry payload.
		if (primType != 0xE0)
		{
			return processedLongs;
		}

		switch (primSubType)
		{
		case 0x1:
		{
			// DR_TPAGE
			activeDrawEnv.tpage = (code & 0x1FF);
			activeDrawEnv.dtd = (code >> 9) & 1;
			// NOTE(aalhendi): Standalone DR_TPAGE packets use the same E1 word
			// for blend changes; only full DRAWENV packets retarget native
			// on-screen/offscreen rendering.
			if (fullDrawEnvPacket)
			{
				activeDrawEnv.dfe = (code >> 10) & 1;
			}
			break;
		}
		case 0x2:
		{
			// DR_TWIN
			activeDrawEnv.tw.w = (code & 0x1F);
			activeDrawEnv.tw.h = ((code >> 5) & 0x1F);
			activeDrawEnv.tw.x = ((code >> 10) & 0x1F);
			activeDrawEnv.tw.y = ((code >> 15) & 0x1F);
			break;
		}
		case 0x3:
		{
			// DR_AREA
			activeDrawEnv.clip.x = code & 1023;
			activeDrawEnv.clip.y = (code >> 10) & 1023;
			fullDrawEnvPacket = true;
			break;
		}
		case 0x4:
		{
			// DR_AREA (second part)
			activeDrawEnv.clip.w = code & 1023;
			activeDrawEnv.clip.h = (code >> 10) & 1023;

			activeDrawEnv.clip.w = activeDrawEnv.clip.w - activeDrawEnv.clip.x + 1;
			activeDrawEnv.clip.h = activeDrawEnv.clip.h - activeDrawEnv.clip.y + 1;
			fullDrawEnvPacket = true;
			break;
		}
		case 0x5:
		{
			// DR_OFFSET
			activeDrawEnv.ofs[0] = NativeGpu_SignExtend11(code);
			activeDrawEnv.ofs[1] = NativeGpu_SignExtend11(code >> 11);
			fullDrawEnvPacket = true;
			break;
		}
		case 0x6:
		{
			SetPSXMaskState(code);
			break;
		}
		case 0:
			// NOTE(aalhendi): ctr-native local divergence for CTR OTs. A zero
			// word can be terminal draw-env padding, or the tag word for the
			// next primitive packed into the same OT entry.
			// return processedLongs;
			if (i + 1 != polyTag->len)
			{
				return processedLongs;
			}
			break;
		}
		++processedLongs;
	}

	return processedLongs;
}

internal void ProcessDrawEnvCommand(u32 code)
{
	const int primSubType = code >> 24 & 0x0F;

	switch (primSubType)
	{
	case 0x1:
		activeDrawEnv.tpage = (code & 0x1FF);
		activeDrawEnv.dtd = (code >> 9) & 1;
		break;
	case 0x2:
		activeDrawEnv.tw.w = (code & 0x1F);
		activeDrawEnv.tw.h = ((code >> 5) & 0x1F);
		activeDrawEnv.tw.x = ((code >> 10) & 0x1F);
		activeDrawEnv.tw.y = ((code >> 15) & 0x1F);
		break;
	case 0x3:
		activeDrawEnv.clip.x = code & 1023;
		activeDrawEnv.clip.y = (code >> 10) & 1023;
		break;
	case 0x4:
		activeDrawEnv.clip.w = code & 1023;
		activeDrawEnv.clip.h = (code >> 10) & 1023;
		activeDrawEnv.clip.w = activeDrawEnv.clip.w - activeDrawEnv.clip.x + 1;
		activeDrawEnv.clip.h = activeDrawEnv.clip.h - activeDrawEnv.clip.y + 1;
		break;
	case 0x5:
		activeDrawEnv.ofs[0] = NativeGpu_SignExtend11(code);
		activeDrawEnv.ofs[1] = NativeGpu_SignExtend11(code >> 11);
		break;
	case 0x6:
		SetPSXMaskState(code);
		break;
	}
}

internal int ProcessPsyXPrims(P_TAG *polyTag)
{
	const int primSubType = polyTag->code & 0x0F;

	switch (primSubType)
	{
	case 0x01:
	{
		DR_PSYX_TEX *psytex = (DR_PSYX_TEX *)polyTag;
		s_gpu.overrideTexture = psytex->code[0] & 0xFFFFFF;
		s_gpu.overrideTextureWidth = psytex->code[1] & 0xFFF;
		s_gpu.overrideTextureHeight = psytex->code[1] >> 16 & 0xFFF;
		return 2;
	}
	case 0x02:
	{
		// [A] Psy-X custom debug marker packet
		DR_PSYX_DBGMARKER *psydbg = (DR_PSYX_DBGMARKER *)polyTag;
		s_gpu.currentSplitDebugText = psydbg->text;
		return 2;
	}
	}

	return 0;
}

// Processes primitive
// returns processed primitive primLength in longs
int ParsePrimitive(P_TAG *polyTag)
{
	const int primType = polyTag->code & 0xF0;

	int primLength = 0;
	bool handledZeroLength = false;

	switch (primType)
	{
	case 0x00:
	{
		const int primSubType = polyTag->code & 0x0F;
		const u32 *codePtr = (u32 *)&polyTag->pad0;
		// NOTE(aalhendi): ctr-native local divergence. CTR RenderWeather can
		// emit a retail length-2 zero packet when weather is enabled but the
		// level has no fill-mode payload. The PSX consumes it by tag length;
		// the native parser must advance past it too.
		if (polyTag->len == 2 && codePtr[0] == 0 && codePtr[1] == 0)
		{
			primLength = 2;
		}
		else if (polyTag->len == 0 && *(u32 *)polyTag == 0)
		{
			// CTR ghost transparency packets include raw GPU NOP words between
			// draw-mode changes and triangle commands. They consume exactly one
			// command word; ParsePrimitivesLinkedList adds P_LEN to the return.
			handledZeroLength = true;
		}
		else if (primSubType == 0x0)
		{
			primLength = 3;
		}
		else if (primSubType == 0x1)
		{
			DR_MOVE *drmove = (DR_MOVE *)polyTag;
			const u32 rectPos = drmove->code[2];
			const u32 rectSize = drmove->code[4];

			const int y = drmove->code[3] >> 0x10 & 0xFFFF;
			const int x = drmove->code[3] & 0xFFFF;

			RECT16 rect;
			rect.x = (s16)(rectPos & 0xffff);
			rect.y = (s16)(rectPos >> 16);
			rect.w = (s16)(rectSize & 0xffff);
			rect.h = (s16)(rectSize >> 16);

			if (NativeGpu_HasPendingSplits())
			{
				DrawAllSplits();
			}
			MoveImage(&rect, x, y);
			primLength = 5;
		}
		else if (primSubType == 0x2)
		{
			// NOTE(aalhendi): ctr-native local divergence. CTR emits retail
			// FILL packets in OTs; the old PsyCross parser did not consume them, which caused
			// zero-length primitive spam.
			TILE *fill = (TILE *)polyTag;
			RECT16 rect;

			rect.x = fill->x0;
			rect.y = fill->y0;
			rect.w = fill->w;
			rect.h = fill->h;

			if (NativeGpu_HasPendingSplits())
			{
				DrawAllSplits();
			}
			ClearImage(&rect, fill->r0, fill->g0, fill->b0);
			primLength = 3;
		}
		break;
	}
	case 0x20:
		// Flat polygons
		primLength = ProcessFlatPoly(polyTag);
		break;
	case 0x30:
		// Gouraud shaded polygons
		primLength = ProcessGouraudPoly(polyTag);
		break;
	case 0x40:
		// Flat (single colour) Lines
		primLength = ProcessFlatLines(polyTag);
		break;
	case 0x50:
		// Gouraud lines
		primLength = ProcessGouraudLines(polyTag);
		break;
	case 0x60:
	case 0x70:
		// TILE and SPRT
		primLength = ProcessTileAndSprt(polyTag);
		break;
	case 0xA0:
		// DR_LOAD
		{
			DR_LOAD *drload = (DR_LOAD *)polyTag;
			const u32 rectPos = drload->code[1];
			const u32 rectSize = drload->code[2];

			RECT16 rect;
			rect.x = (s16)(rectPos & 0xffff);
			rect.y = (s16)(rectPos >> 16);
			rect.w = (s16)(rectSize & 0xffff);
			rect.h = (s16)(rectSize >> 16);

			LoadImage(&rect, (uint32_t *)drload->p);

			// TODO(aalhendi): Audit whether CTR ever appends additional GPU
			// commands after a DR_LOAD payload in the same packet.
		}
		primLength = getlen(polyTag);
		break;
	case 0xB0:
		// [A] Psy-X custom primitives
		primLength = ProcessPsyXPrims(polyTag);
		break;
	case 0xE0:
		// Draw Env setup
		primLength = ProcessDrawEnv(polyTag);
		break;
		// default:
		//	NATIVE_GPU_ERROR("got %0x primitive\n", primType);
	}

	if (primLength == 0 && !handledZeroLength)
	{
		NATIVE_GPU_ERROR("Unhandled zero length %0x primitive\n", primType);
	}

	return primLength;
}

int ParseTaglessPrimitive(u32 *command)
{
	const u32 code = *command;
	const int primType = (code >> 24) & 0xF0;

	if (code == 0)
	{
		return 1;
	}

	if (primType == 0xE0)
	{
		ProcessDrawEnvCommand(code);
		return 1;
	}

	P_TAG *polyTag = (P_TAG *)(command - P_LEN);
	int primLength = ParsePrimitive(polyTag);

	if (primLength == 0)
	{
		NATIVE_GPU_ERROR("Unhandled tagless primitive %08x\n", code);
		return 1;
	}

	return primLength;
}
