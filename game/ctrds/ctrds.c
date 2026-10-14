#include <common.h>

#include "ctrds_online.h"
#include <platform/native_companion.h>
#include <platform/native_renderer.h>
#include <platform/native_gpu_links.h>

// CTR-DS companion HUD: layout data and the few helpers the UI code calls.
//
// Coordinates are absolute framebuffer coordinates. The game view occupies
// rows 0..CTRDS_GAME_HEIGHT; the companion panel is everything below it, so a
// panel-relative Y of 10 is written here as 216 + 10 = 226.

// Panel-relative origin. CTRDS_INLINE renders the companion over the normal
// 512x216 frame (origin 0) so the layout can be verified on a desktop build.
// CTRDS_SECOND_SCREEN will place it at its own origin once the companion has a
// render target of its own -- see the VRAM note in ctrds.h.
#define CTRDS_PANEL_TOP 0

// Retail anchors that the companion values are re-derived from.
enum CtrdsRetailAnchors
{
	// UI_RenderFrame_Racing draws the live map with its bottom-right corner here.
	CTRDS_RETAIL_MAP_X = 500,
	CTRDS_RETAIL_MAP_Y = 195,

	// UI_MAP_ICON_Y_OFFSET, applied by UI_Map_GetIconPos to every dot.
	CTRDS_MAP_ICON_Y_OFFSET = 0x10,
};

struct CtrdsLayout g_ctrds = {
    .mode = CTRDS_SECOND_SCREEN,
    .tallFramebuffer = 0,
    .fbHeight = CTRDS_FB_HEIGHT,
    .fbPitch = CTRDS_FB_PITCH,

    .screenW = CTRDS_PANEL_W,
    .screenH = CTRDS_PANEL_H,

    // --- top strip: time, item, fruit, lap ---
    .clockX = 14,
    .clockY = 6,

    // --- speedometer: upper half of the right column. Retail draws the
    // backdrop at (480,190) and the needle at (414,145), so the backdrop keeps
    // its +66/+45 offset from the needle. ---
    .speedBgX = 398 + 66,
    .speedBgY = 60 + 45,

    // --- left column: the full eight-driver order, not retail's top four.
    // No vertical fudge any more: the panel itself now stretches art 1.78x. ---
    .rankIconX = 16,
    .rankIconBaseY = CTRDS_BODY_Y + 6,
    .rankSlotH = 25,
    .rankVisible = 8,
    .rankTextX = 54,
    .rankTextStartY = CTRDS_BODY_Y + 8,
    // Eight portraits share the column, so trim them slightly to stop the
    // taller ones touching their neighbours.
    .rankIconScale = (CTRDS_FP_ONE * 7) / 8,

    // --- live map: centre column. Scale and anchor are computed from the
    // region by Ctrds_InitLayout, so changing the grid re-fits the map. ---
    .mapX = CTRDS_MAP_RX + CTRDS_MAP_RW,
    .mapY = CTRDS_BODY_Y + CTRDS_BODY_H,
    .mapScale = CTRDS_FP_ONE,
    // Native size. These are tiny icons -- the warpball and its target marker --
    // and magnifying them samples past their cell into the neighbouring atlas
    // art, which in this group is lettering, so a bolt came out as a glyph. The
    // driver markers no longer need this at all, since they draw portraits.
    .mapIconScale = CTRDS_FP_ONE,

    .widescreen = 1,
    .aspectMode = 1,
    .aspectW = 16,
    .aspectH = 9,
    .vsyncsPerFlip = 1,
    .fxaa = 0,
    .crt = 0,
    .dither = 1,
    .swapFaceButtons = 1,
    .touchControls = 0,
    .skipAv = 0,
    .primReject = 1,
    .pgxpTextures = 0,
    .hdArt = 1,
    .hdLog = 0,
    .hdDump = 0,
    .internalScale = 4,
    .targetFps = 60,

    .mapRetailX = CTRDS_RETAIL_MAP_X,
    .mapRetailY = CTRDS_RETAIL_MAP_Y,

    .increaseDrawDistance = 0,
    .speedMultiplier = 100,
    .turnMultiplier = 100,
    .jumpMultiplier = 100,
    .gravityMultiplier = 100,
    .unlockAllCharacters = 0,
    .unlockAllGates = 0,
    .unlockAllPortals = 0,
    .skipIntro = 0,
    .skipHints = 0,
};

// Companion replacement for data.hud_1P_P1. Slots the companion does not use
// (battle, relic, adventure rewards) keep their retail values.
#define CTRDS_HUD_BLOCK                                                                                          \
    /* 0x00 WEAPON           */ {191, 11, 0, 4096},                                                              \
    /* 0x01 LAP_COUNT        */ {466, 10, 0, 0},                                                                 \
    /* 0x02 BIG1             */ {410, CTRDS_NUM_Y + 52, 256, 5530},                               \
    /* 0x03 FRUIT_MODEL      */ {330, 18, 512, 4096},                                                            \
    /* 0x04 WUMPA_COUNT      */ {350, 10, 0, 0},                                                                 \
    /* 0x05 RANK             */ {450, CTRDS_NUM_Y + 30, 0, 0},                                   \
    /* 0x06 JUMP_METER       */ {398 + 77, 60 + 61, 0, 0},                                 \
    /* 0x07 (unused)         */ {475, 164, 0, 0},                                                                \
    /* 0x08 SLIDE_METER      */ {398 + 76, 60 + 61, 0, 0},                                 \
    /* 0x09 SPEEDOMETER      */ {398, 60, 0, 4096},                                             \
    /* 0x0a (unused)         */ {20, 57, 0, 4096},                                                               \
    /* 0x0b BATTLE_WEAPON_BG */ {191 - 23, 11 - 10, 0, 4096},                                                              \
    /* 0x0c RACING_WEAPON_BG */ {330 - 30, 18 - 15, 0, 2457},                                                              \
    /* 0x0d BATTLE_SCORE     */ {454, 8, 0, 0},                                                                  \
    /* 0x0e RELIC            */ {50, 24, 256, 1536},                                                             \
    /* 0x0f KEY              */ {256, 24, 512, 3072},                                                            \
    /* 0x10 TROPHY           */ {406, 24, 512, 6144},                                                            \
    /* 0x11 CRYSTAL          */ {389, 30, 512, 2048},                                                            \
    /* 0x12 TOKEN_OR_CTR     */ {145, 30, 512, 2048},                                                            \
    /* 0x13 TIMEBOX          */ {200, 30, 256, 768},

struct UiElement2D g_ctrdsHud1P[UI_HUD_SLOT_COUNT * CTRDS_HUD_BLOCKS] = {
    CTRDS_HUD_BLOCK CTRDS_HUD_BLOCK CTRDS_HUD_BLOCK CTRDS_HUD_BLOCK CTRDS_HUD_BLOCK CTRDS_HUD_BLOCK CTRDS_HUD_BLOCK CTRDS_HUD_BLOCK
};

// Live-map scale is only applied while the companion map is being built, so
// the track-select and adventure-hub maps keep drawing at 1:1.
internal s16 s_ctrdsMapScale = CTRDS_FP_ONE;

// 0x20 is the elapsed-time value a 30fps frame produces. Never scale below 1x:
// a slow frame should not shorten cooldowns.
#define CTRDS_RETAIL_FRAME_MS 0x20

int Ctrds_ScaleFrames(int frames30)
{
	int ms;

	if (!Ctrds_PacingActive())
	{
		return frames30;
	}

	ms = sdata->gGT->elapsedTimeMS;

	if (ms < 4)
	{
		ms = 4;
	}
	if (ms > CTRDS_RETAIL_FRAME_MS)
	{
		ms = CTRDS_RETAIL_FRAME_MS;
	}

	return (frames30 * CTRDS_RETAIL_FRAME_MS) / ms;
}

int Ctrds_ScaleStep(int step30)
{
	int ms;

	if (!Ctrds_PacingActive())
	{
		return step30;
	}

	ms = sdata->gGT->elapsedTimeMS;

	if (ms < 1)
	{
		ms = 1;
	}
	if (ms > CTRDS_RETAIL_FRAME_MS)
	{
		ms = CTRDS_RETAIL_FRAME_MS;
	}

	return (step30 * ms) / CTRDS_RETAIL_FRAME_MS;
}

internal int s_ctrdsGatedElapsed = 0;
internal int s_ctrdsGatedPending = 0;
internal int s_ctrdsRetailTicks = 0;

// gGT->timer counts rendered frames, so above 30fps it runs fast -- four times
// fast at a 120 cap. Retail used it as the clock for anything that cycles:
// animated textures, flashing text, the countdown beep, rumble. All of those
// then play at the cap divided by thirty, which is what "Uka Uka spins too
// fast" was.
//
// This advances once per 30Hz tick instead, so it counts what gGT->timer
// counted on hardware. Use it for animation; use gGT->timer where the frame
// count itself is the point.
int Ctrds_RetailTicks(void)
{
	if (!Ctrds_PacingActive())
	{
		return (int)sdata->gGT->timer;
	}

	return s_ctrdsRetailTicks;
}

void Ctrds_BeginFrameGating(void)
{
	if (!Ctrds_PacingActive())
	{
		return;
	}

	s_ctrdsGatedElapsed += sdata->gGT->elapsedTimeMS;

	// On the frames where the gated buckets actually run, hand them everything
	// that has elapsed since they last ran.
	if (Ctrds_Is30HzTick())
	{
		s_ctrdsGatedPending = s_ctrdsGatedElapsed;
		s_ctrdsGatedElapsed = 0;
		s_ctrdsRetailTicks++;
	}
}

int Ctrds_GatedElapsedMs(void)
{
	return (s_ctrdsGatedPending > 0) ? s_ctrdsGatedPending : sdata->gGT->elapsedTimeMS;
}

int Ctrds_BucketRunsEveryFrame(int bucket)
{
	if (!Ctrds_PacingActive())
	{
		return 1;
	}

	switch (bucket)
	{
	// Delta-timed off elapsedTimeMS, so these are already correct at any frame
	// rate and need every frame to stay smooth.
	case PLAYER:
	case ROBOT:
	case GHOST:
	case TRACKING:
	case CAMERA:
	case HUD:
	case PAUSE:
		return 1;

	// Everything else steps by a fixed amount per frame -- barrels, carts,
	// platforms, mines, warp pads, the start banner -- and would run at two or
	// four times speed if it ran on every frame at 60 or 120fps.
	default:
		return 0;
	}
}

int Ctrds_BucketRunsThisFrame(int bucket)
{
	if (Ctrds_BucketRunsEveryFrame(bucket))
	{
		return 1;
	}

	return Ctrds_Is30HzTick();
}

int Ctrds_Is30HzTick(void)
{
	int step;

	if (!Ctrds_PacingActive())
	{
		return 1;
	}

	// 1 at 30fps, 2 at 60, 4 at 120.
	step = Ctrds_ScaleFrames(1);
	if (step < 2)
	{
		return 1;
	}

	return ((int)sdata->gGT->timer % step) == 0;
}

// Panel settings. These live on the bottom screen because the game's own menus
// have nowhere to put them, and the panel is idle on the main menu anyway.
enum CtrdsSetting
{
	CTRDS_SET_RESOLUTION = 0,
	CTRDS_SET_FPS,
	CTRDS_SET_ASPECT,
	CTRDS_SET_FXAA,
	CTRDS_SET_CRT,
	CTRDS_SET_DITHER,
	CTRDS_SET_DRAW_DISTANCE,
	CTRDS_SET_SPEED,
	CTRDS_SET_TURN,
	CTRDS_SET_JUMP,
	CTRDS_SET_GRAVITY,
	CTRDS_SET_UNLOCK_CHARACTERS,
	CTRDS_SET_UNLOCK_GATES,
	CTRDS_SET_UNLOCK_PORTALS,
	CTRDS_SET_SKIP_INTRO,
	CTRDS_SET_SKIP_HINTS,
	CTRDS_SET_EXIT_GAME,
	CTRDS_SET_ONLINE,
	CTRDS_SET_ALL
};

// Online is built and working but there is no public server to reach, so it is
// left out of the list rather than offering something that cannot connect. Drop
// this back to CTRDS_SET_ALL to show it again.
#define CTRDS_SET_COUNT CTRDS_SET_ONLINE

// The caps offered on the panel. 480 is "uncapped" in practice: the hardware
// runs out long before the pacing does.
internal const int s_ctrdsFpsSteps[] = {30, 60, 90, 120, CTRDS_FPS_UNLIMITED};

// Reduces a ratio for display, so 1920x1080 reads as 16:9 rather than 1920:1080.
internal int Ctrds_Gcd(int a, int b)
{
	while (b != 0)
	{
		const int t = a % b;
		a = b;
		b = t;
	}

	return (a > 0) ? a : 1;
}
#define CTRDS_FPS_STEP_COUNT ((int)(sizeof(s_ctrdsFpsSteps) / sizeof(s_ctrdsFpsSteps[0])))

internal int s_ctrdsSetting = CTRDS_SET_RESOLUTION;
internal int s_ctrdsMenuOpen = 0;

void Ctrds_SaveConfig(void)
{
	char path[1024];
	FILE *f;

	const char *dir = NativeAssets_GetAssetDir();

	if ((dir == NULL) || (dir[0] == '\0'))
	{
		return;
	}

	snprintf(path, sizeof(path), "%s/ctrds.cfg", dir);

	f = fopen(path, "w");
	if (f == NULL)
	{
		return;
	}

	fprintf(f, "# CTR-DS settings. Edit and restart, or change them on the panel.\n");
	fprintf(f, "target_fps=%d\n", g_ctrds.targetFps);
	fprintf(f, "internal_scale=%d\n", g_ctrds.internalScale);
	fprintf(f, "fxaa=%d\n", g_ctrds.fxaa);
	fprintf(f, "crt=%d\n", g_ctrds.crt);
	fprintf(f, "dither=%d\n", g_ctrds.dither);
	fprintf(f, "pgxp_textures=%d\n", g_ctrds.pgxpTextures);
	fprintf(f, "aspect_mode=%d\n", g_ctrds.aspectMode);
	fprintf(f, "aspect_w=%d\n", g_ctrds.aspectW);
	fprintf(f, "aspect_h=%d\n", g_ctrds.aspectH);
	fprintf(f, "touch_controls=%d\n", g_ctrds.touchControls);
	fprintf(f, "swap_face_buttons=%d\n", g_ctrds.swapFaceButtons);
	fprintf(f, "increase_draw_distance=%d\n", g_ctrds.increaseDrawDistance);
	fprintf(f, "speed_stat_multiplier=%d\n", g_ctrds.speedMultiplier);
	fprintf(f, "turn_stat_multiplier=%d\n", g_ctrds.turnMultiplier);
	fprintf(f, "jump_stat_multiplier=%d\n", g_ctrds.jumpMultiplier);
	fprintf(f, "gravity_stat_multiplier=%d\n", g_ctrds.gravityMultiplier);
	fprintf(f, "unlock_all_characters=%d\n", g_ctrds.unlockAllCharacters);
	fprintf(f, "unlock_all_gates=%d\n", g_ctrds.unlockAllGates);
	fprintf(f, "unlock_all_portals=%d\n", g_ctrds.unlockAllPortals);
	fprintf(f, "skip_intro=%d\n", g_ctrds.skipIntro);
	fprintf(f, "skip_hints=%d\n", g_ctrds.skipHints);
	fprintf(f, "online=%d\n", Ctrds_OnlineEnabled() ? 1 : 0);
	fprintf(f, "online_host=%s\n", Ctrds_OnlineConfig()->host);
	fprintf(f, "online_port=%d\n", Ctrds_OnlineConfig()->port);
	fprintf(f, "online_name=%s\n", Ctrds_OnlineConfig()->name);
	fprintf(f, "online_room=%d\n", Ctrds_OnlineConfig()->room);

	fclose(f);
}

internal void Ctrds_AdjustSetting(int delta)
{
	switch (s_ctrdsSetting)
	{
	case CTRDS_SET_RESOLUTION:
	{
		int scale = g_ctrds.internalScale + delta;

		if (scale < 1)
		{
			scale = 1;
		}
		if (scale > CTR_NATIVE_MAX_INTERNAL_SCALE)
		{
			scale = CTR_NATIVE_MAX_INTERNAL_SCALE;
		}

		g_ctrds.internalScale = scale;
		NativeRenderer_SetInternalScale(scale);
		break;
	}

	case CTRDS_SET_FPS:
	{
		int i;
		int found = 0;

		for (i = 0; i < CTRDS_FPS_STEP_COUNT; i++)
		{
			if (s_ctrdsFpsSteps[i] == g_ctrds.targetFps)
			{
				found = i;
				break;
			}
		}

		found += delta;
		if (found < 0)
		{
			found = CTRDS_FPS_STEP_COUNT - 1;
		}
		if (found >= CTRDS_FPS_STEP_COUNT)
		{
			found = 0;
		}

		g_ctrds.targetFps = s_ctrdsFpsSteps[found];
		break;
	}

	case CTRDS_SET_ASPECT:
		// Two choices only: the original 4:3, or whatever this display is.
		g_ctrds.aspectMode = (g_ctrds.aspectMode == 0) ? 1 : 0;
		g_ctrds.widescreen = Ctrds_Widescreen();
		break;

	case CTRDS_SET_FXAA:
		g_ctrds.fxaa = !g_ctrds.fxaa;
		break;

	case CTRDS_SET_CRT:
		g_ctrds.crt = !g_ctrds.crt;
		break;

	case CTRDS_SET_DITHER:
		g_ctrds.dither = !g_ctrds.dither;
		break;

	case CTRDS_SET_DRAW_DISTANCE:
		g_ctrds.increaseDrawDistance = !g_ctrds.increaseDrawDistance;
		break;

	case CTRDS_SET_SPEED:
	{
		int v = g_ctrds.speedMultiplier + delta * 10;
		if (v < 10) v = 10;
		if (v > 200) v = 200;
		g_ctrds.speedMultiplier = v;
		break;
	}

	case CTRDS_SET_TURN:
	{
		int v = g_ctrds.turnMultiplier + delta * 10;
		if (v < 10) v = 10;
		if (v > 400) v = 400;
		g_ctrds.turnMultiplier = v;
		break;
	}

	case CTRDS_SET_JUMP:
	{
		int v = g_ctrds.jumpMultiplier + delta * 10;
		if (v < 10) v = 10;
		if (v > 300) v = 300;
		g_ctrds.jumpMultiplier = v;
		break;
	}

	case CTRDS_SET_GRAVITY:
	{
		int v = g_ctrds.gravityMultiplier + delta * 10;
		if (v < 10) v = 10;
		if (v > 300) v = 300;
		g_ctrds.gravityMultiplier = v;
		break;
	}

	case CTRDS_SET_UNLOCK_CHARACTERS:
		g_ctrds.unlockAllCharacters = !g_ctrds.unlockAllCharacters;
		break;

	case CTRDS_SET_UNLOCK_GATES:
		g_ctrds.unlockAllGates = !g_ctrds.unlockAllGates;
		break;

	case CTRDS_SET_UNLOCK_PORTALS:
		g_ctrds.unlockAllPortals = !g_ctrds.unlockAllPortals;
		break;

	case CTRDS_SET_SKIP_INTRO:
		g_ctrds.skipIntro = !g_ctrds.skipIntro;
		break;

	case CTRDS_SET_SKIP_HINTS:
		g_ctrds.skipHints = !g_ctrds.skipHints;
		break;

	case CTRDS_SET_EXIT_GAME:
		// An action, not a value: either shoulder confirms it, no need to
		// distinguish direction.
		NativeCompanion_RequestExit();
		break;

	case CTRDS_SET_ONLINE:
		Ctrds_OnlineToggle();
		break;

	default:
		break;
	}

	Ctrds_SaveConfig();
}

// A tap on the panel. Rows are laid out here exactly as Ctrds_DrawIdlePanel
// draws them, so the two cannot drift apart.
// Draws the settings list into the given ordering table. Used by the panel and,
// on a device with only one screen, by the main screen itself -- otherwise the
// settings would be unreachable there.
int Ctrds_DrawSettingsList(uint32_t *head, int centreX, int topY, int maxVisible)
{
	struct CtrdsOnlineStatus st;
	char line[96];
	int row;
	int y = topY;
	int first = 0;
	int last = CTRDS_SET_COUNT;

	Ctrds_OnlineGetStatus(&st);

	// Scroll the window to keep the selected row visible instead of always
	// drawing from the top -- otherwise a selection past maxVisible rows
	// moves off the bottom edge with nothing on screen to show it happened.
	if ((maxVisible > 0) && (maxVisible < CTRDS_SET_COUNT))
	{
		first = s_ctrdsSetting - maxVisible / 2;

		if (first > CTRDS_SET_COUNT - maxVisible)
		{
			first = CTRDS_SET_COUNT - maxVisible;
		}
		if (first < 0)
		{
			first = 0;
		}

		last = first + maxVisible;
	}

	for (row = first; row < last; row++)
	{
		const char *marker = (row == s_ctrdsSetting) ? "*" : " ";

		switch (row)
		{
		case CTRDS_SET_RESOLUTION:
			// A step is one PSX frame of 240 lines, so the height is the
			// useful number; the multiplier stays for anyone who thinks in it.
			snprintf(line, sizeof(line), "%s RESOLUTION  %dP (%dX)", marker, g_ctrds.internalScale * 240,
			    g_ctrds.internalScale);
			break;
		case CTRDS_SET_FPS:
			if (g_ctrds.targetFps >= CTRDS_FPS_UNLIMITED)
			{
				snprintf(line, sizeof(line), "%s FPS CAP  UNCAPPED", marker);
			}
			else
			{
				snprintf(line, sizeof(line), "%s FPS CAP  %d", marker, g_ctrds.targetFps);
			}
			break;
		case CTRDS_SET_ASPECT:
			if (g_ctrds.aspectMode == 0)
			{
				snprintf(line, sizeof(line), "%s ASPECT  4:3", marker);
			}
			else
			{
				const int g = Ctrds_Gcd(Ctrds_AspectW(), Ctrds_AspectH());

				snprintf(line, sizeof(line), "%s ASPECT  WIDE %d:%d", marker, Ctrds_AspectW() / g, Ctrds_AspectH() / g);
			}
			break;
		case CTRDS_SET_FXAA:
			snprintf(line, sizeof(line), "%s FXAA  %s", marker, g_ctrds.fxaa ? "ON" : "OFF");
			break;
		case CTRDS_SET_CRT:
			snprintf(line, sizeof(line), "%s CRT  %s", marker, g_ctrds.crt ? "ON" : "OFF");
			break;
		case CTRDS_SET_DITHER:
			snprintf(line, sizeof(line), "%s DITHER  %s", marker, g_ctrds.dither ? "ON" : "OFF");
			break;
		case CTRDS_SET_DRAW_DISTANCE:
			snprintf(line, sizeof(line), "%s DRAW DISTANCE  %s", marker, g_ctrds.increaseDrawDistance ? "FAR" : "NORMAL");
			break;
		case CTRDS_SET_SPEED:
			snprintf(line, sizeof(line), "%s SPEED  %d%%", marker, g_ctrds.speedMultiplier);
			break;
		case CTRDS_SET_TURN:
			snprintf(line, sizeof(line), "%s TURN  %d%%", marker, g_ctrds.turnMultiplier);
			break;
		case CTRDS_SET_JUMP:
			snprintf(line, sizeof(line), "%s JUMP  %d%%", marker, g_ctrds.jumpMultiplier);
			break;
		case CTRDS_SET_GRAVITY:
			snprintf(line, sizeof(line), "%s GRAVITY  %d%%", marker, g_ctrds.gravityMultiplier);
			break;
		case CTRDS_SET_UNLOCK_CHARACTERS:
			snprintf(line, sizeof(line), "%s UNLOCK CHARACTERS  %s", marker, g_ctrds.unlockAllCharacters ? "ON" : "OFF");
			break;
		case CTRDS_SET_UNLOCK_GATES:
			snprintf(line, sizeof(line), "%s UNLOCK GATES  %s", marker, g_ctrds.unlockAllGates ? "ON" : "OFF");
			break;
		case CTRDS_SET_UNLOCK_PORTALS:
			snprintf(line, sizeof(line), "%s UNLOCK PORTALS  %s", marker, g_ctrds.unlockAllPortals ? "ON" : "OFF");
			break;
		case CTRDS_SET_SKIP_INTRO:
			snprintf(line, sizeof(line), "%s SKIP INTRO  %s", marker, g_ctrds.skipIntro ? "ON" : "OFF");
			break;
		case CTRDS_SET_SKIP_HINTS:
			snprintf(line, sizeof(line), "%s SKIP HINTS  %s", marker, g_ctrds.skipHints ? "ON" : "OFF");
			break;
		case CTRDS_SET_EXIT_GAME:
			snprintf(line, sizeof(line), "%s EXIT GAME", marker);
			break;
		default:
			snprintf(line, sizeof(line), "%s ONLINE  %s", marker, Ctrds_OnlineEnabled() ? "ON" : "OFF");
			break;
		}

		DecalFont_DrawLineOT(line, centreX, y, FONT_SMALL, JUSTIFY_CENTER, head);
		y += 15;
	}

	// Online status stays hidden along with its row.
	if ((CTRDS_SET_COUNT > CTRDS_SET_ONLINE) && Ctrds_OnlineEnabled() && (st.message[0] != '\0'))
	{
		DecalFont_DrawLineOT(st.message, centreX, y + 3, FONT_SMALL, JUSTIFY_CENTER, head);
		y += 15;
	}

	DecalFont_DrawLineOT(Ctrds_SecondScreen() ? "SELECT MOVES   L1/R1 CHANGES" : "UP/DOWN MOVES   LEFT/RIGHT CHANGES", centreX, y + 6,
	        FONT_SMALL, JUSTIFY_CENTER, head);

	return y;
}

// With no panel there is nothing to tap, so this is the only way in on a
// single-screen phone.
// The item slot is empty most of the time, and an empty gap in the strip reads
// as nothing at all. A box drawn every frame gives the slot a permanent home, so
// picking something up registers as the box filling rather than art appearing
// out of nowhere.
// The panel stretches Y by about 1.78, so a box that reads square on screen is
// wider than it is tall in these units: 72 x 40, not 72 x 72.
#define CTRDS_ITEM_BOX_X 178
#define CTRDS_ITEM_BOX_Y 5
#define CTRDS_ITEM_BOX_W 72
#define CTRDS_ITEM_BOX_H 40

void Ctrds_DrawItemBox(struct GameTracker *gGT)
{
	RECT r;

	if (!Ctrds_Enabled() || !Ctrds_InRace())
	{
		return;
	}

	r.x = CTRDS_ITEM_BOX_X;
	r.y = CTRDS_ITEM_BOX_Y;
	r.w = CTRDS_ITEM_BOX_W;
	r.h = CTRDS_ITEM_BOX_H;

	// Entry 2 keeps it behind the item and its shine, both of which sit nearer
	// the front of the ordering table.
	RECTMENU_DrawInnerRect(&r, 0, &gGT->pushBuffer_UI.ptrOT[2]);
}

// Single-screen platforms (PC, or Android/handhelds with no companion
// display) have no idle panel to draw the settings list on, and no launcher
// UI to set them from either on PC. Ctrds_PollPanelInput() already reads
// Select/L1/R1 regardless of whether a companion screen exists - this is
// only the other half, the visible feedback, drawn straight onto the main
// screen's own ordering table instead of the panel's. Hidden until
// Ctrds_ToggleMenu() opens it (Tab/Back), not shown just for sitting at the
// main menu -- it used to be, and a demo-mode timeout kicking the game out of
// the main menu made it look like the menu itself vanished on its own.
void Ctrds_DrawSettingsOnMainScreen(struct GameTracker *gGT)
{
	uint32_t *ot;

	if (!s_ctrdsMenuOpen || Ctrds_SecondScreen() || !Ctrds_MenuAvailableHere())
	{
		return;
	}

	// pushBuffer[0].ptrOT[0x3ff] is claimed earlier in the frame for the draw
	// env/skybox glow (MainFrame_RenderFrame.c), so text linked in there after
	// never made it to the screen. pushBuffer_UI is the table RenderSubmit
	// already uses for other 2D overlays, and its entries are one shared OT
	// with the game's own menu widgets, ordered near-to-far by index -- index 0
	// is the nearest slot there is. The title/mode-select menu draws its own
	// box and text straight into that slot (gGT->backBuffer->otMem.uiOT, the
	// same pointer as pushBuffer_UI.ptrOT), so an offset slot here put our
	// panel strictly behind it: the menu's own letters always painted over
	// ours. Sharing index 0, and queued earlier in the frame than the menu
	// queues its own draws (see the call site in MainFrame_RenderFrame.c),
	// puts our panel on top instead.
	ot = gGT->pushBuffer_UI.ptrOT;

	// Full screen, not boxed to a narrow column: a fixed width cut the list's
	// own text off the edge of the box on longer rows. Rows/topY are centred
	// so the list still never runs past the 216px-tall screen.
	{
		const s16 centreX = (s16)(SCREEN_WIDTH / 2);

		// All CTRDS_SET_COUNT rows run to ~240px, past the bottom edge on a
		// plain 216px-tall single screen -- capped and scrolled so the
		// selected row is always the one that's visible.
		const int visibleRows = 11;
		const int contentHeight = visibleRows * 15 + 20;
		const s16 topY = (s16)((CTRDS_GAME_HEIGHT - contentHeight) / 2);

		// AddPrim prepends, so within one OT slot the FIRST prim linked in this
		// frame is the LAST one walked at draw time -- i.e. on top. The list text
		// has to go in before the backing rect, not after, or the (translucent)
		// rect draws over its own text and dims it.
		Ctrds_DrawSettingsList(ot, centreX, topY, visibleRows);

		// A flat rect covering the whole screen behind it -- same box the
		// game's own menu uses (RECTMENU_DrawInnerRect) -- so the list reads as
		// its own screen instead of blending into whatever is on screen there.
		{
			RECT bg;

			bg.x = 0;
			bg.y = 0;
			bg.w = SCREEN_WIDTH;
			bg.h = CTRDS_GAME_HEIGHT;
			RECTMENU_DrawInnerRect(&bg, 0, ot);
		}
	}
}

// Main menu (drawn on the title screen itself) or mid-race (drawn over the
// paused race view) -- the only two places this has actually been wired up
// to render. Elsewhere (cutscenes, podium, loading) is a no-op rather than a
// promise the game state can't yet keep.
int Ctrds_MenuAvailableHere(void)
{
	return Ctrds_OnMainMenu() || Ctrds_InRace();
}

// PAUSE_2 is one of the four PAUSE_ALL bits retail leaves "unused, debug" --
// distinct from PAUSE_1, which is the real in-race pause menu's own bit, so
// the two can't stomp on each other's state if a player somehow reaches both.
// Every gameplay system already guards on the PAUSE_ALL mask, so this freezes
// the race (physics, AI, camera, HUD ticking) the same way retail pausing
// does, which is what a kart still steered by a D-pad that our list has just
// taken over actually needs.
#define CTRDS_PAUSE_BIT PAUSE_2

internal void Ctrds_SetMenuOpen(int open)
{
	s_ctrdsMenuOpen = open;

	// Unconditionally cleared on close (harmless if it was never set) rather
	// than only when Ctrds_InRace() still holds: that's what lets a forced
	// close -- the race having ended while the list was still open -- drop
	// the pause too, instead of leaving it stuck set with nothing left to
	// clear it.
	if (open && Ctrds_InRace())
	{
		sdata->gGT->gameMode1 |= CTRDS_PAUSE_BIT;
	}
	else
	{
		sdata->gGT->gameMode1 &= ~CTRDS_PAUSE_BIT;
	}

	Platform_Log("[CTR-DS] menu open=%d\n", s_ctrdsMenuOpen);
}

void Ctrds_ToggleMenu(void)
{
	// Closing is always allowed, whatever the game state drifted into while
	// the list was open -- most notably, stealing input for our own list
	// this whole time also starves the main menu's demo-mode idle timer,
	// which can fire and drop the title screen into attract mode before the
	// player gets back to Back. Gating the close the same way as the open
	// left it stuck open with no way in reach of Ctrds_ToggleMenu() to undo.
	if (s_ctrdsMenuOpen)
	{
		Ctrds_SetMenuOpen(0);
		return;
	}

	if (!Ctrds_MenuAvailableHere())
	{
		Platform_Log("[CTR-DS] menu toggle ignored, nowhere to show it\n");
		return;
	}

	Ctrds_SetMenuOpen(1);
}

void Ctrds_PanelTap(float nx, float ny)
{
	int y;
	int row;

	(void)nx;

	if (!Ctrds_Enabled() || !Ctrds_OnMainMenu())
	{
		return;
	}

	y = (int)(ny * (float)CTRDS_PANEL_H);

	for (row = 0; row < CTRDS_SET_COUNT; row++)
	{
		const int rowY = (g_ctrds.screenH / 2) - 8 + (row * 15);

		// The drawn text sits on rowY; accept a band around it, since a
		// fingertip is far larger than a line of this font.
		if ((y >= rowY - 9) && (y <= rowY + 9))
		{
			s_ctrdsSetting = row;
			Ctrds_AdjustSetting(1);
			return;
		}
	}
}

int Ctrds_TouchControlsMode(void)
{
	return g_ctrds.touchControls;
}

int Ctrds_OnMainMenu(void)
{
	return (sdata->gGT->gameMode1 & MAIN_MENU) != 0;
}

// Reads the pad directly rather than going through the menu code, which has no
// notion of a setting that lives on the other screen. Companion-panel mode
// only: its panel is always live, with no open/close concept, so Select/L1/R1
// stay free for it. The single-window case is handled by
// Ctrds_MaskMenuInput() instead, which also has to run before the game's own
// menu reads the pad.
void Ctrds_PollPanelInput(void)
{
	local_persist int s_wasOnMenu = 0;

	int onMenu;

	onMenu = Ctrds_OnMainMenu();

	// Ignore the frame the menu opens on, so a Select press that got us here
	// does not immediately toggle.
	if (onMenu && s_wasOnMenu && Ctrds_SecondScreen())
	{
		const int tapped = sdata->gGamepads->gamepad[0].buttonsTapped;

		// Select moves down the list; the shoulders change the highlighted
		// value. The d-pad is left alone because the game's own menu is using
		// it, and Select is not bound there.
		if ((tapped & BTN_SELECT) != 0)
		{
			s_ctrdsSetting = (s_ctrdsSetting + 1) % CTRDS_SET_COUNT;
		}

		if ((tapped & BTN_R1) != 0)
		{
			Ctrds_AdjustSetting(1);
		}
		else if ((tapped & BTN_L1) != 0)
		{
			Ctrds_AdjustSetting(-1);
		}
	}

	s_wasOnMenu = onMenu;
}

// Single-window case. Also where a controller's Back/Select button gets its
// chance to open/close the list (see below), since there's nowhere else in
// the single-window frame that already runs every frame with the pad in
// hand. Otherwise: steals the exact button set the game's own menu
// (RECTMENU_INPUT_MENU) listens for while our list is open, so navigating our
// list can't also move the CTR title/mode-select menu sitting underneath it --
// that was happening silently before, since both read the same pad. Must run
// before RECTMENU_CollectInput() so the theft actually lands before the game
// menu reads the pad.
// A noisy D-pad (some Android controllers, this one included by report,
// never fully debounce a direction in hardware) can report a single physical
// press as several rapid press/release blips a few milliseconds apart, each
// one a legitimate rising edge on buttonsTapped -- so edge-detection alone
// still reads it as several presses. This is the actual fix: once a press is
// accepted, further edges are ignored until this much wall-clock time has
// passed, collapsing a whole blip-storm into the one move it should be.
#define CTRDS_MENU_DEBOUNCE_MS 180

// A gamepad's very first polled frame can read as every button held at once
// (Odin2 confirmed live: tapped=held=0x0003bc2f, prevHeld=0 -- the controller
// hasn't produced a real HID report yet, so the zeroed snapshot decodes as
// "all pressed" the same way the PSX pad protocol's active-low bytes always
// would). That phantom BTN_SELECT edge is silently swallowed by
// Ctrds_ToggleMenu() logging "nowhere to show it" on any normal boot, since
// the crate intro is still playing -- but with skip_intro on, the game is
// already sitting on the main menu on frame one, so the same glitch opens the
// settings list with nobody having touched Select or Back. Ignoring this
// button for the console's first few frames costs nothing a real player could
// notice and only ever discards the one glitched read.
#define CTRDS_SELECT_SETTLE_FRAMES 5

void Ctrds_MaskMenuInput(struct GamepadSystem *gGamepads)
{
	local_persist int s_wasOpen = 0;
	local_persist int s_debounceMs = 0;
	local_persist int s_framesPolled = 0;

	struct GamepadBuffer *pad;
	u32 tapped;

	if (s_framesPolled < CTRDS_SELECT_SETTLE_FRAMES)
	{
		s_framesPolled++;
	}

	// A physical controller's own Back/Select button (Odin2 and most
	// USB/Bluetooth pads included) never reaches the keyboard hook Tab and
	// Android's system Back gesture go through in native_platform.c -- SDL
	// reports it as a gamepad button (mapped here to BTN_SELECT via
	// gc_select), not a key event -- so without this, that button opened
	// nothing. Skipped on a companion panel, where Select already means
	// "move to the next row" (Ctrds_PollPanelInput).
	if (!Ctrds_SecondScreen() && (s_framesPolled >= CTRDS_SELECT_SETTLE_FRAMES) &&
	    ((gGamepads->gamepad[0].buttonsTapped & BTN_SELECT) != 0))
	{
		Ctrds_ToggleMenu();
	}

	if (!s_ctrdsMenuOpen || Ctrds_SecondScreen() || !Ctrds_MenuAvailableHere())
	{
		if (s_ctrdsMenuOpen && !Ctrds_SecondScreen())
		{
			// The race ended (or otherwise left the only two screens this
			// can draw on) while the list was still open. Close it and drop
			// the pause instead of leaving the game stuck frozen with no way
			// left to reach Ctrds_ToggleMenu() and undo it.
			Ctrds_SetMenuOpen(0);
		}

		s_wasOpen = 0;
		s_debounceMs = 0;
		return;
	}

	pad = &gGamepads->gamepad[0];
	tapped = (u32)pad->buttonsTapped;

	pad->buttonsTapped &= ~RECTMENU_INPUT_MENU;
	pad->buttonsHeldCurrFrame &= ~RECTMENU_INPUT_MENU;

	// Ignore the frame the menu opened on, so whatever press opened it isn't
	// also read as a move.
	if (!s_wasOpen)
	{
		s_wasOpen = 1;
		s_debounceMs = 0;
		return;
	}

	if (s_debounceMs > 0)
	{
		s_debounceMs -= (int)sdata->gGT->elapsedTimeMS;
		if (s_debounceMs > 0)
		{
			// Still cooling down from the last accepted move: whatever just
			// came in is either the same press bouncing or a genuinely fast
			// re-press, and either way it's too soon to count.
			return;
		}

		s_debounceMs = 0;
	}

	tapped &= (BTN_UP | BTN_DOWN | BTN_LEFT | BTN_RIGHT);

	if (tapped == 0)
	{
		return;
	}

	s_debounceMs = CTRDS_MENU_DEBOUNCE_MS;

	// One move per press, full stop -- holding a direction does nothing
	// further until it's released and pressed again.
	if ((tapped & BTN_DOWN) != 0)
	{
		s_ctrdsSetting = (s_ctrdsSetting + 1) % CTRDS_SET_COUNT;
	}
	else if ((tapped & BTN_UP) != 0)
	{
		s_ctrdsSetting = (s_ctrdsSetting + CTRDS_SET_COUNT - 1) % CTRDS_SET_COUNT;
	}

	if ((tapped & BTN_RIGHT) != 0)
	{
		Ctrds_AdjustSetting(1);
	}
	else if ((tapped & BTN_LEFT) != 0)
	{
		Ctrds_AdjustSetting(-1);
	}
}

// The colour to start a frame from, as 0-255 RGB.
//
// Retail's sky is a band around the horizon, not a dome, and its top edge sits
// just outside a 4:3 view. A taller view -- which is what the main screen of a
// flip handheld is -- sees past it, and whatever the frame began with stays
// there. Black made that a hole in the sky.
//
// The level's own glow gradient is the colour the sky fades to, so starting
// from it leaves the gap the same colour as the sky above it. Levels without
// one keep black, which is what they had.
void Ctrds_SkyClearColor(int *r, int *g, int *b)
{
	const struct Level *level;

	*r = 0;
	*g = 0;
	*b = 0;

	if ((sdata == NULL) || (sdata->gGT == NULL))
	{
		return;
	}

	level = sdata->gGT->level1;
	if ((level == NULL) || ((level->configFlags & 1) == 0))
	{
		return;
	}

	{
		const u32 color = level->glowGradient[0].colorTo;

		*r = (int)(color & 0xFF);
		*g = (int)((color >> 8) & 0xFF);
		*b = (int)((color >> 16) & 0xFF);
	}
}

int Ctrds_InRace(void)
{
	const struct GameTracker *gGT = sdata->gGT;

	// END_OF_RACE covers the results and podium screens, which keep the race HUD
	// flag set but are menus: their HUD and menu belong on the main screen.
	return ((gGT->hudFlags & HUD_FLAG_RACE_HUD) != 0) && ((gGT->gameMode1 & ADVENTURE_ARENA) == 0)
	    && ((gGT->gameMode1 & END_OF_RACE) == 0);
}

// How many 60Hz audio updates this VBlank owes.
//
// howl_PlayAudio_Update advances the game's own audio engine -- sequencing,
// envelopes, the lot -- and retail called it from a VBlank that arrived sixty
// times a second. VBlanks here come at the configured cap, so at 120 the engine
// ran twice for every time it should have and the music played at double speed.
// The mixer was already decoupled; this is the other clock, and the one that
// sets tempo.
//
// A count rather than a yes/no, because a cap below 60 owes more than one
// update per VBlank: at 30 it owes two, and returning a flag would have played
// the music at half speed there instead.
int Ctrds_AudioUpdatesThisVBlank(void)
{
	local_persist int accumulator = 0;

	return Ctrds_Ticks60(&accumulator);
}

int Ctrds_VsyncsPerFlip(void)
{
	// VBlanks are emitted at the cap, not at retail's 60Hz, so a flip every
	// second one is only right when the cap is 60. At a cap of 30 it halved the
	// game to 15fps, which is what "cap 30, 2 vblanks per flip" read in the log.
	// The two branches below already answer this correctly for every cap: a race
	// flips every VBlank, a menu every cap/30 of them.

	if (Ctrds_InRace())
	{
		return (g_ctrds.vsyncsPerFlip > 0) ? g_ctrds.vsyncsPerFlip : 2;
	}

	// Retail runs its menus at 30fps. VBlanks now come at the configured cap, so
	// flip every T/30 of them to keep that cadence whatever the cap is.
	{
		const int perFlip = Ctrds_TargetFps() / 30;

		return (perFlip > 0) ? perFlip : 1;
	}
}

void Ctrds_FitMapToRegion(const struct Icon *mapTop, const struct Icon *mapBottom)
{
	int baseW;
	int baseH;
	int drawsTopHalf;
	int scaleX;
	int scaleY;
	int scale;
	int w;
	int h;

	if (!Ctrds_Enabled() || (mapTop == NULL) || (mapBottom == NULL))
	{
		return;
	}

	// UI_Map_DrawMap builds the map from these two textures, anchored by their
	// right edge and bottom edge, so this is its true drawn extent. A fixed
	// guess cannot work: every track's map is a different size, which is why
	// the map sat low in its region with a gap above it.
	// UI_Map_DrawMap only draws the top half under this condition; counting it
	// when it is not drawn overestimates the height and pushes the map down,
	// leaving a gap above it.
	{
		struct GameTracker *gGT = sdata->gGT;
		struct UIMapSpawnMetadata *mapMetadata = NULL;

		// The spawn table and the pointers inside it are serialized asset
		// data, so on a 64-bit host they hold tagged references rather than
		// addresses. Both hops are resolved rather than dereferenced, and
		// either can legitimately come back empty on a track without a map.
		struct SpawnType1 *spawn = Level_GetSpawnType1(gGT->level1, "CTR-DS map metadata");

		if (spawn != NULL)
		{
			mapMetadata = (struct UIMapSpawnMetadata *)SpawnType1_GetPointer(spawn, ST1_MAP, sizeof(*mapMetadata), _Alignof(struct UIMapSpawnMetadata),
			                                                                 "CTR-DS map metadata");
		}

		drawsTopHalf = (((mapMetadata != NULL) && (mapMetadata->topHalfMode == 0)) || ((gGT->gameMode1 & MAIN_MENU) != 0));
	}

	baseW = (int)((u16)mapBottom->texLayout.u1 - (u16)mapBottom->texLayout.u0);
	baseH = (int)((u16)mapBottom->texLayout.v2 - (u16)mapBottom->texLayout.v0);

	if (drawsTopHalf)
	{
		baseH += (int)((u16)mapTop->texLayout.v2 - (u16)mapTop->texLayout.v0);
	}

	if ((baseW <= 0) || (baseH <= 0))
	{
		return;
	}

	scaleX = ((CTRDS_MAP_RW - (2 * CTRDS_MAP_MARGIN_X)) * CTRDS_FP_ONE) / baseW;
	scaleY = ((CTRDS_BODY_H - (2 * CTRDS_MAP_MARGIN_Y)) * CTRDS_FP_ONE) / baseH;
	scale = (scaleX < scaleY) ? scaleX : scaleY;

	// Sit at 90% of the fit so the map has some air around it rather than
	// running right up to the neighbouring regions. Centring below uses the
	// scaled size, so the margins grow evenly on all four sides.
	scale = (scale * 9) / 10;

	w = (baseW * scale) / CTRDS_FP_ONE;
	h = (baseH * scale) / CTRDS_FP_ONE;

	g_ctrds.mapScale = (s16)scale;
	g_ctrds.mapX = (s16)(CTRDS_MAP_RX + ((CTRDS_MAP_RW + w) / 2));
	// The map art carries a few rows of transparent padding along its bottom
	// edge, so centring the texture leaves the visible track sitting high in the
	// region. Measured at ~6 units; there is no way to see it from texLayout,
	// which describes the rect and not what is opaque inside it.
	g_ctrds.mapY = (s16)(CTRDS_BODY_Y + ((CTRDS_BODY_H + h) / 2) + CTRDS_MAP_ART_PAD_Y);
}

// Tiny key=value reader. Only the handful of switches worth changing without a
// rebuild; anything absent keeps its default.
internal struct CtrdsOnlineConfig s_onlineConfig = {
    0,
    "127.0.0.1",
    64001,
    "CTRDS",
    -1,
};

const struct CtrdsOnlineConfig *Ctrds_OnlineConfig(void)
{
	return &s_onlineConfig;
}

void Ctrds_LoadConfig(void)
{
	char path[1024];
	char line[128];
	FILE *f;

	const char *dir = NativeAssets_GetAssetDir();

	if ((dir == NULL) || (dir[0] == '\0'))
	{
		return;
	}

	snprintf(path, sizeof(path), "%s/ctrds.cfg", dir);

	f = fopen(path, "r");
	if (f == NULL)
	{
		// Write the defaults out so there is something to edit on the device.
		// The game creates it rather than adb pushing one in: a shell-owned file
		// in the app's own files directory makes the next install fail to stage
		// its assets.
		f = fopen(path, "w");
		if (f != NULL)
		{
			fprintf(f, "# CTR-DS settings. Edit and restart the game.\n");
			fprintf(f, "# fxaa: smooth jagged edges (0/1)\n");
			fprintf(f, "fxaa=%d\n", g_ctrds.fxaa);
			fprintf(f, "# crt: CRT-Royale-style scanlines, phosphor mask and halation (0/1)\n");
			fprintf(f, "crt=%d\n", g_ctrds.crt);
			fprintf(f, "# dither: PS1-style dither pattern that hides its 15-bit colour banding (0/1)\n");
			fprintf(f, "dither=%d\n", g_ctrds.dither);
			fprintf(f, "# target_fps: frame cap -- 30, 60, 90, 120, or 480 for uncapped\n");
			fprintf(f, "target_fps=%d\n", g_ctrds.targetFps);
			fprintf(f, "# internal_scale: render resolution multiplier, 1-5\n");
			fprintf(f, "internal_scale=%d\n", g_ctrds.internalScale);
			fprintf(f, "# skip_av: start even when the disc has no intro video or XA audio\n");
			fprintf(f, "skip_av=%d\n", g_ctrds.skipAv);
			fprintf(f, "# prim_reject: drop primitives too large for the PSX GPU (0 off, 1 on, 2 on+log)\n");
			fprintf(f, "prim_reject=%d\n", g_ctrds.primReject);
			fprintf(f, "# pgxp_textures: EXPERIMENTAL perspective-correct textures on 3D geometry (0/1)\n");
			fprintf(f, "pgxp_textures=%d\n", g_ctrds.pgxpTextures);
			fprintf(f, "# hd_art: use the upscaled art in hd/ instead of the original sprites\n");
			fprintf(f, "hd_art=%d\n", g_ctrds.hdArt);
			fprintf(f, "# hd_log: report each HUD icon index as it is drawn, to author hd/ art\n");
			fprintf(f, "hd_log=%d\n", g_ctrds.hdLog);
			fprintf(f, "# hd_dump: write each HUD icon out of VRAM to hd_dump/ as a PNG\n");
			fprintf(f, "hd_dump=%d\n", g_ctrds.hdDump);
			fprintf(f, "# touch_controls: 0 auto (only with no pad), 1 always, 2 never\n");
			fprintf(f, "touch_controls=%d\n", g_ctrds.touchControls);
			fprintf(f, "# swap_face_buttons: swap A/B and X/Y (0/1)\n");
			fprintf(f, "swap_face_buttons=%d\n", g_ctrds.swapFaceButtons);
			fprintf(f, "\n# --- Gameplay ---\n");
			fprintf(f, "# increase_draw_distance: render ~3x farther (0/1)\n");
			fprintf(f, "increase_draw_distance=%d\n", g_ctrds.increaseDrawDistance);
			fprintf(f, "# speed/turn/jump/gravity_stat_multiplier: percent, 100 = default\n");
			fprintf(f, "speed_stat_multiplier=%d\n", g_ctrds.speedMultiplier);
			fprintf(f, "turn_stat_multiplier=%d\n", g_ctrds.turnMultiplier);
			fprintf(f, "jump_stat_multiplier=%d\n", g_ctrds.jumpMultiplier);
			fprintf(f, "gravity_stat_multiplier=%d\n", g_ctrds.gravityMultiplier);
			fprintf(f, "# unlock_all_characters/gates/portals (0/1)\n");
			fprintf(f, "unlock_all_characters=%d\n", g_ctrds.unlockAllCharacters);
			fprintf(f, "unlock_all_gates=%d\n", g_ctrds.unlockAllGates);
			fprintf(f, "unlock_all_portals=%d\n", g_ctrds.unlockAllPortals);
			fprintf(f, "# skip_intro: skip SCEA/copyright/ND crate intro (0/1)\n");
			fprintf(f, "skip_intro=%d\n", g_ctrds.skipIntro);
			fprintf(f, "# skip_hints: skip adventure mode mask hints (0/1)\n");
			fprintf(f, "skip_hints=%d\n", g_ctrds.skipHints);
			fprintf(f, "# aspect_mode: 0 = 4:3, 1 = follow the display, 2 = aspect_w/aspect_h below.\n");
			fprintf(f, "# The field of view widens to match rather than stretching.\n");
			fprintf(f, "aspect_mode=%d\n", g_ctrds.aspectMode);
			fprintf(f, "aspect_w=%d\n", g_ctrds.aspectW);
			fprintf(f, "aspect_h=%d\n", g_ctrds.aspectH);
			fprintf(f, "# vblank_mult: 1 = up to 60fps, 2 = up to 120fps.\n");
			fprintf(f, "# Leave commented out to follow the panel automatically.\n");
			fprintf(f, "#vblank_mult=2\n");
			fprintf(f, "\n# --- OnlineCTR ---\n");
			fprintf(f, "# online: connect to an OnlineCTR server on launch (0/1)\n");
			fprintf(f, "online=%d\n", s_onlineConfig.enabled);
			fprintf(f, "online_host=%s\n", s_onlineConfig.host);
			fprintf(f, "online_port=%d\n", s_onlineConfig.port);
			fprintf(f, "online_name=%s\n", s_onlineConfig.name);
			fprintf(f, "# online_room: room to join automatically, -1 to wait\n");
			fprintf(f, "online_room=%d\n", s_onlineConfig.room);
			fclose(f);
		}

		return;
	}

	while (fgets(line, sizeof(line), f) != NULL)
	{
		char *eq = strchr(line, '=');
		int value;

		if ((line[0] == '#') || (eq == NULL))
		{
			continue;
		}

		*eq = '\0';
		value = atoi(eq + 1);

		if (strncmp(line, "fxaa", 4) == 0)
		{
			g_ctrds.fxaa = value;
		}
		else if (strncmp(line, "online_host", 11) == 0)
		{
			// value is text, not a number
			char *v = eq + 1;
			size_t n = strlen(v);
			while ((n > 0) && ((v[n - 1] == '\n') || (v[n - 1] == '\r') || (v[n - 1] == ' ')))
			{
				v[--n] = '\0';
			}
			snprintf(s_onlineConfig.host, sizeof(s_onlineConfig.host), "%s", v);
		}
		else if (strncmp(line, "online_room", 11) == 0)
		{
			s_onlineConfig.room = value;
		}
		else if (strncmp(line, "online_port", 11) == 0)
		{
			s_onlineConfig.port = value;
		}
		else if (strncmp(line, "online_name", 11) == 0)
		{
			char *v = eq + 1;
			size_t n = strlen(v);
			while ((n > 0) && ((v[n - 1] == '\n') || (v[n - 1] == '\r') || (v[n - 1] == ' ')))
			{
				v[--n] = '\0';
			}
			snprintf(s_onlineConfig.name, sizeof(s_onlineConfig.name), "%s", v);
		}
		else if (strncmp(line, "online", 6) == 0)
		{
			s_onlineConfig.enabled = value;
		}
		else if (strncmp(line, "crt", 3) == 0)
		{
			g_ctrds.crt = value;
		}
		else if (strncmp(line, "dither", 6) == 0)
		{
			g_ctrds.dither = value;
		}
		else if (strncmp(line, "aspect_mode", 11) == 0)
		{
			g_ctrds.aspectMode = value;
		}
		else if (strncmp(line, "aspect_w", 8) == 0)
		{
			g_ctrds.aspectW = value;
		}
		else if (strncmp(line, "aspect_h", 8) == 0)
		{
			g_ctrds.aspectH = value;
		}
		else if (strncmp(line, "target_fps", 10) == 0)
		{
			g_ctrds.targetFps = value;
		}
		else if (strncmp(line, "internal_scale", 14) == 0)
		{
			g_ctrds.internalScale = value;
		}
		else if (strncmp(line, "skip_av", 7) == 0)
		{
			g_ctrds.skipAv = value;
		}
		else if (strncmp(line, "prim_reject", 11) == 0)
		{
			g_ctrds.primReject = value;
		}
		else if (strncmp(line, "pgxp_textures", 13) == 0)
		{
			g_ctrds.pgxpTextures = value;
		}
		else if (strncmp(line, "hd_art", 6) == 0)
		{
			g_ctrds.hdArt = value;
		}
		else if (strncmp(line, "perf", 4) == 0)
		{
			g_ctrds.perf = value;
		}
		else if (strncmp(line, "hd_dump", 7) == 0)
		{
			g_ctrds.hdDump = value;
		}
		else if (strncmp(line, "hd_log", 6) == 0)
		{
			g_ctrds.hdLog = value;
		}
		else if (strncmp(line, "touch_controls", 14) == 0)
		{
			g_ctrds.touchControls = value;
		}
		else if (strncmp(line, "swap_face_buttons", 17) == 0)
		{
			g_ctrds.swapFaceButtons = value;
		}
		else if (strncmp(line, "widescreen", 10) == 0)
		{
			g_ctrds.widescreen = value;
		}
		else if (strncmp(line, "increase_draw_distance", 22) == 0)
		{
			g_ctrds.increaseDrawDistance = value;
		}
		else if (strncmp(line, "speed_stat_multiplier", 21) == 0)
		{
			g_ctrds.speedMultiplier = value;
		}
		else if (strncmp(line, "turn_stat_multiplier", 20) == 0)
		{
			g_ctrds.turnMultiplier = value;
		}
		else if (strncmp(line, "jump_stat_multiplier", 20) == 0)
		{
			g_ctrds.jumpMultiplier = value;
		}
		else if (strncmp(line, "gravity_stat_multiplier", 23) == 0)
		{
			g_ctrds.gravityMultiplier = value;
		}
		else if (strncmp(line, "unlock_all_characters", 21) == 0)
		{
			g_ctrds.unlockAllCharacters = value;
		}
		else if (strncmp(line, "unlock_all_gates", 16) == 0)
		{
			g_ctrds.unlockAllGates = value;
		}
		else if (strncmp(line, "unlock_all_portals", 18) == 0)
		{
			g_ctrds.unlockAllPortals = value;
		}
		else if (strncmp(line, "skip_intro", 10) == 0)
		{
			g_ctrds.skipIntro = value;
		}
		else if (strncmp(line, "skip_hints", 10) == 0)
		{
			g_ctrds.skipHints = value;
		}
	}

	fclose(f);

	// The aspect is what the view is actually drawn at, so the flag has to
	// follow it. Toggling aspect on the panel recomputed this and loading a
	// config did not, so a saved 4:3 came back with widescreen still set: the
	// view was 4:3 while every path that asks this question was told otherwise.
	if (g_ctrds.aspectMode == 0)
	{
		g_ctrds.widescreen = 0;
	}

	Platform_Log("[CTR-DS] ctrds.cfg: fxaa=%d crt=%d swapFaceButtons=%d widescreen=%d aspectMode=%d\n", g_ctrds.fxaa,
	        g_ctrds.crt, g_ctrds.swapFaceButtons, g_ctrds.widescreen, g_ctrds.aspectMode);
}

void Ctrds_DisableSecondScreen(void)
{
	if (g_ctrds.mode == CTRDS_DISABLED)
	{
		return;
	}

	// Everything the mod does keys off this: the HUD stops being redirected to
	// the panel, the frame pacing stops treating menus specially, and the
	// presentation goes back to a single rectangle.
	g_ctrds.mode = CTRDS_DISABLED;

	Platform_Log("[CTR-DS] no second display -- running as a single-screen game\n");
}

void Ctrds_InitLayout(void)
{
	// The map's real scale and anchor come from the track's own map textures in
	// Ctrds_FitMapToRegion, which runs before it is drawn. These are only the
	// values used before the first race has loaded a map.
	g_ctrds.mapScale = CTRDS_FP_ONE;
	g_ctrds.mapX = (s16)(CTRDS_MAP_RX + CTRDS_MAP_RW);
	g_ctrds.mapY = (s16)(CTRDS_BODY_Y + CTRDS_BODY_H);
}

// Portraits are far bigger than the dot they replace, so they are drawn small
// enough that eight of them bunched at one corner stay readable.
#define CTRDS_MAP_PORTRAIT_SCALE ((CTRDS_FP_ONE * 3) / 5)

// Matches UI_RANK_DAMAGE_COLOR_NEUTRAL, the value the rank list uses when a
// driver is undamaged.
#define CTRDS_PORTRAIT_NEUTRAL_COLOR 0x808080

// Full-bright, for the pulse on the player's own marker.
#define CTRDS_PORTRAIT_BRIGHT_COLOR 0xFFFFFF

// The player's marker is drawn larger than the rest so it reads first.
#define CTRDS_MAP_PORTRAIT_PLAYER_SCALE ((CTRDS_FP_ONE * 4) / 5)

void Ctrds_DrawMapPortrait(struct UIMap *map, const s32 worldPos[3], struct Driver *d, int isPlayer)
{
	struct GameTracker *gGT = sdata->gGT;
	struct Icon *icon;
	int posX;
	int posY;
	int w;
	int h;

	icon = gGT->ptrIcons[data.MetaDataCharacters[data.characterIDs[d->driverID]].iconID];

	if (icon == NULL)
	{
		return;
	}

	posX = worldPos[0];
	posY = worldPos[2];
	UI_Map_GetIconPos(map, &posX, &posY);

	// UI_DrawDriverIcon places the icon by its top-left corner; the dot it
	// replaces was centred on the position, so centre the portrait too.
	{
		const int scale = isPlayer ? CTRDS_MAP_PORTRAIT_PLAYER_SCALE : CTRDS_MAP_PORTRAIT_SCALE;

		w = FP_Mult((int)((u16)icon->texLayout.u1 - (u16)icon->texLayout.u0), scale);
		h = FP_Mult((int)((u16)icon->texLayout.v2 - (u16)icon->texLayout.v0), scale);
	}

	// 0x808080 is the neutral vertex colour: it leaves the portrait its own
	// colours. Feeding it a palette entry instead tinted every face orange.
	//
	// The player's own portrait is drawn a little larger, pulsed brighter, and
	// put in a nearer ordering-table entry so it stays on top of the pack --
	// otherwise it disappears under the others exactly when the pack is bunched
	// together and you most need to find yourself.
	// Entry 0, the same one the map background uses. Within an entry the list is
	// last-in-drawn-first, and the dots are added before the map, so they end up
	// drawn after it. Putting them in entry 1 instead pushed the whole set
	// behind the map, because the walk reaches 1 before 0.
	//
	// Transparency 2 is additive: the portrait art carries a black backing,
	// which at the 50% blend of mode 0 showed as a grey box over the map. Added
	// rather than blended, black contributes nothing and disappears.
	if (isPlayer)
	{
		const int bright = ((gGT->timer & 0x10) != 0);

		UI_DrawDriverIcon(icon, (s16)(posX - (w / 2)), (s16)(posY - (h / 2)), &gGT->backBuffer->primMem, gGT->pushBuffer_UI.ptrOT, 2,
		        CTRDS_MAP_PORTRAIT_PLAYER_SCALE, bright ? CTRDS_PORTRAIT_BRIGHT_COLOR : CTRDS_PORTRAIT_NEUTRAL_COLOR);
		return;
	}

	UI_DrawDriverIcon(icon, (s16)(posX - (w / 2)), (s16)(posY - (h / 2)), &gGT->backBuffer->primMem, gGT->pushBuffer_UI.ptrOT, 2,
	        CTRDS_MAP_PORTRAIT_SCALE, CTRDS_PORTRAIT_NEUTRAL_COLOR);
}

s16 Ctrds_MapScale(void)
{
	return s_ctrdsMapScale;
}

s16 Ctrds_MapIconScale(void)
{
	return Ctrds_MapOnCompanion() ? g_ctrds.mapIconScale : CTRDS_FP_ONE;
}

void Ctrds_BeginMapScale(void)
{
	if (Ctrds_MapOnCompanion())
	{
		s_ctrdsMapScale = g_ctrds.mapScale;
	}
}

void Ctrds_EndMapScale(void)
{
	s_ctrdsMapScale = CTRDS_FP_ONE;
}

s16 Ctrds_ApplyScale(s16 value, s16 scale)
{
	return (s16)(((int)value * (int)scale) >> 12);
}

void Ctrds_ScaleMap(struct UIMap *dst, const struct UIMap *src)
{
	const int scale = (int)g_ctrds.mapScale;

	*dst = *src;

	if (!Ctrds_MapOnCompanion())
	{
		return;
	}

	// UI_Map_GetIconPos places a dot at
	//   x = iconStartX + (worldX * iconSizeX) / worldRangeX
	//   y = iconStartY + (worldZ * iconSizeY * 2) / worldRangeY - ICON_Y_OFFSET
	//
	// Scaling iconSize by s scales the offset term by s, so re-anchoring the
	// start point about the retail map corner reproduces
	//   p' = newAnchor + s * (p - retailAnchor)
	// exactly, and every driver, ghost and tracking dot lands on the enlarged
	// background without its own draw code changing.
	dst->iconSizeX = (s16)(((int)src->iconSizeX * scale) >> 12);
	dst->iconSizeY = (s16)(((int)src->iconSizeY * scale) >> 12);

	dst->iconStartX = (s16)((int)g_ctrds.mapX + ((((int)src->iconStartX - (int)g_ctrds.mapRetailX) * scale) >> 12));

	dst->iconStartY =
	    (s16)((int)g_ctrds.mapY + (((((int)src->iconStartY - CTRDS_MAP_ICON_Y_OFFSET) - (int)g_ctrds.mapRetailY) * scale) >> 12) + CTRDS_MAP_ICON_Y_OFFSET);
}

void Ctrds_InitFromEnv(void)
{
	Ctrds_InitLayout();

	// Desktop and Android both start with the retail HUD (CTRDS_DISABLED).
	// CTRDS=1 still selects the inline companion layout for layout tuning.
	g_ctrds.mode = CTRDS_DISABLED;

	const char *mode = getenv("CTRDS");
	const char *tall = getenv("CTRDS_TALL_FB");

	if (mode != NULL)
	{
		g_ctrds.mode = mode[0] - '0';
		if ((g_ctrds.mode < CTRDS_DISABLED) || (g_ctrds.mode > CTRDS_SECOND_SCREEN))
		{
			g_ctrds.mode = CTRDS_DISABLED;
		}
	}

	{
		const char *flip = getenv("CTRDS_VSYNCS");
		if (flip != NULL)
		{
			g_ctrds.vsyncsPerFlip = atoi(flip);
		}
	}

	{
		const char *wide = getenv("CTRDS_WIDE");
		if (wide != NULL)
		{
			g_ctrds.widescreen = (wide[0] == '1');
		}
	}

	if (tall != NULL)
	{
		g_ctrds.tallFramebuffer = (tall[0] == '1');
	}

	{
		const char *h = getenv("CTRDS_FB_H");
		const char *pitch = getenv("CTRDS_FB_P");
		if (h != NULL)
		{
			g_ctrds.fbHeight = (s16)atoi(h);
		}
		if (pitch != NULL)
		{
			g_ctrds.fbPitch = (s16)atoi(pitch);
		}
	}

	Platform_Log("[CTR-DS] mode=%d panel=%dx%d widescreen=%d vsyncsPerFlip=%d\n", g_ctrds.mode, g_ctrds.screenW,
	        g_ctrds.screenH, g_ctrds.widescreen, g_ctrds.vsyncsPerFlip);
}

// The panel needs an ordering table of its own for the idle screen, because
// outside a race the UI table belongs to the top screen and must be left alone.
#define CTRDS_IDLE_OT_LEN 16
global_variable uint32_t s_ctrdsIdleOT[CTRDS_IDLE_OT_LEN];

void Ctrds_RegisterGpuRanges(void)
{
	NativeGpuLinks_RegisterRangeChecked("ctrds idle OT", s_ctrdsIdleOT, sizeof(s_ctrdsIdleOT));
}

// Outside a race the panel shows the game's wordmark on black rather than
// mirroring menus, which stay on the top screen where they belong.
internal void Ctrds_DrawIdlePanel(void)
{
	uint32_t *head = &s_ctrdsIdleOT[CTRDS_IDLE_OT_LEN - 1];

	ClearOTagR(s_ctrdsIdleOT, CTRDS_IDLE_OT_LEN);

	DecalFont_DrawLineOT("CTR", g_ctrds.screenW / 2, (g_ctrds.screenH / 2) - 92, FONT_BIG, JUSTIFY_CENTER, head);
	DecalFont_DrawLineOT("CRASH TEAM RACING", g_ctrds.screenW / 2, (g_ctrds.screenH / 2) - 58, FONT_SMALL, JUSTIFY_CENTER, head);

	// Settings list, on the main menu only.
	if (Ctrds_OnMainMenu())
	{
		Ctrds_DrawSettingsList(head, g_ctrds.screenW / 2, (g_ctrds.screenH / 2) - 8, 0);
	}

	DrawOTag(head);
}

void Ctrds_DrawCompanionPass(struct GameTracker *gGT)
{
	// The UI region is the head of the shared ordering table: ClearOTagR builds
	// one reversed chain, pushBuffer_UI.ptrOT sits at entry 1, and the camera
	// tables start at entry 6. Entries 5..0 are therefore the UI, drawn last in
	// the normal walk. Drawing that sub-chain on its own gives the panel exactly
	// the HUD and map, with no 3D.
	// Entry 5 holds 3D world geometry, not UI -- starting the walk there draws
	// the player's kart onto the panel. 4 is the top of the UI range.
	uint32_t *uiChainHead = &gGT->pushBuffer_UI.ptrOT[4];
	uint32_t *otBase = gGT->pushBuffer_UI.ptrOT - 1;

	// Only a race moves its HUD to the panel. Menus, the adventure hub and the
	// pre-race screens keep their HUD on the top screen, so outside a race the
	// UI table is left linked and the panel shows the idle screen instead.
	const int inRace = Ctrds_InRace();

	// DrawOTag would otherwise open the frame itself and bind the game view.
	Platform_BeginScene();

	NativeRenderer_BeginCompanionTarget(CTRDS_PANEL_W, CTRDS_PANEL_H);

	// The panel is the one target the overlay composites into, so this walk is
	// the only place a replacement can be taken.
	Ctrds_HdBeginTarget();

	if (inRace)
	{
		DrawOTag(uiChainHead);
	}
	else
	{
		Ctrds_DrawIdlePanel();
	}

	// Composite replacement art into the panel before it is packed away.
	Ctrds_HdFlush(CTRDS_PANEL_W, CTRDS_PANEL_H);

	NativeRenderer_EndCompanionTarget(CTRDS_VRAM_PANEL_X, CTRDS_VRAM_PANEL_Y);

	if (inRace)
	{
		// Empty the UI buckets so the main pass renders a HUD-free game view.
		// The prims stay in frame memory, they are simply no longer linked.
		ClearOTagR(otBase, 6);
	}
}
