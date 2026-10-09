#ifndef PLATFORM_H
#define PLATFORM_H

struct PlatformMempackArena
{
	void *base;
	void *start;
	void *endOfMemory;
	int size;
	int backingSize;
};

void Platform_Init(const char *title, int width, int height);
int Platform_IsInitialized(void);
int Platform_IsHostActive(void);
int Platform_ShouldQuit(void);
void Platform_Shutdown(void);
void Platform_InitScratchpad(void);
const struct PlatformMempackArena *Platform_InitMempackArena(void);
const struct PlatformMempackArena *Platform_GetMempackArena(void);
void Platform_BeginFrame(void);
int Platform_BeginScene(void);
void Platform_EndScene(void);
void Platform_EndFrame(void);
void Platform_PresentVRAMDisplay(void);
void Platform_PinVRAMDisplayFrames(int frameCount);
void Platform_PinVRAMDisplayRect(int x, int y, int w, int h, int frameCount);
int Platform_GetVBlankCount(void);
void Platform_WaitUntilVBlank(int targetVBlank);
void Platform_PollHostEvents(void);

// PGXP textures: bind a prim field that just received raw GTE output (packed
// SXY) to the transform's view depth, keyed by its address. Called by the
// RenderBucket prim writers; see platform/native_gpu.c. No-ops while the
// pgxp_textures option is off.
void Pgxp_NoteSxyStore(const void *addrField, unsigned int packedSxy, int szLow);
void Pgxp_NotePrimWrite(void *dstField, const void *srcField, unsigned int packed, int szLow);
void Pgxp_NoteTransformCopy(void *dstField, const void *srcField);
void Pgxp_SetPackedSource(const void *field, unsigned int packed);
int Platform_PollInput(void);
void Platform_InputAcknowledgeRetailPoll(void);
int Platform_StartDisplayLoop(void (*callback)(void *), void *userdata);
void Platform_StopDisplayLoop(void);
int Platform_RunLifecycleSelfTest(void);
int Platform_RunFrameStatsSelfTest(void);

#if defined(CTR_NATIVE)
int NikoGetEnterKey(void);
#endif

#endif
