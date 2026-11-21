// Missile Defender for Nintendo DS.
//
// A port of the iOS game (github.com/HenriVSL/Missile-Defender) to two screens.
// The world is one tall column: the top screen is the upper atmosphere where
// warheads appear and can only be watched, the bottom screen is the lower
// atmosphere where the stylus fires counter-missiles. Both screens are 8bpp
// bitmaps drawn in software into RAM back buffers and DMA'd to VRAM on vblank.

#include <nds.h>
#include <fat.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

// ---------------------------------------------------------------------------
// Layout (pixels, world coordinates: y grows downward across both screens)

#define SW 256
#define SH 192
#define GAP 32                      // invisible rows between the two screens
#define BOT0 (SH + GAP)             // world y of bottom screen row 0
#define WORLD_H (BOT0 + SH)         // 416
#define GROUND_H 10
#define GROUND_Y (WORLD_H - GROUND_H)
#define GRAD_START 140              // world y where the sky starts turning blue

#define BASE_W 22
#define BASE_H 13
#define CITY_W 15
#define CITY_H 12
#define NUM_BASES 3
#define NUM_CITIES 6

static const float BASE_X_FRACS[NUM_BASES] = {0.10f, 0.50f, 0.90f};
static const float CITY_X_FRACS[NUM_CITIES] = {0.22f, 0.30f, 0.38f, 0.62f, 0.70f, 0.78f};

// ---------------------------------------------------------------------------
// Tuning (mirrors Config.swift, scaled to DS pixels; times in frames at 60 Hz)

#define AMMO_PER_BASE 10
#define ABM_SPEED 320.0f            // px/s, side batteries
#define ABM_SPEED_CENTER 470.0f     // centre battery fires faster
#define INCOMING_BASE_SPEED 44.0f
#define INCOMING_SPEED_PER_WAVE 6.5f
#define INCOMING_SPEED_MAX 160.0f

#define EXPLOSION_RADIUS 16
#define GROUND_BURST_RADIUS 10
#define EXP_EXPAND 13
#define EXP_HOLD 33
#define EXP_CONTRACT 20
#define EXP_COLOR_PERIOD 4
#define FIRE_COOLDOWN 2

#define BASE_INCOMING_COUNT 6
#define INCOMING_COUNT_PER_WAVE 2
#define INCOMING_COUNT_MAX 26
#define SPAWN_INTERVAL_START 84.0f
#define SPAWN_INTERVAL_MIN 21.0f
#define SPAWN_INTERVAL_DECAY 4.2f
#define FIRST_SPAWN_DELAY 54.0f
#define INTER_WAVE_DELAY 144

#define MIRV_FIRST_WAVE 3
#define MIRV_CHANCE_START 0.15f
#define MIRV_CHANCE_PER_WAVE 0.04f
#define MIRV_CHANCE_MAX 0.5f
#define MIRV_SPLIT_MIN 2
#define MIRV_SPLIT_MAX 3
#define MIRV_SPLIT_FRAC_MIN 0.45f   // split altitude, fraction of world height from the bottom
#define MIRV_SPLIT_FRAC_MAX 0.62f

#define POINTS_PER_MISSILE 25
#define POINTS_PER_UNUSED_ABM 5
#define POINTS_PER_SAVED_CITY 100
#define BONUS_CITY_EVERY 10000
#define MAX_SCORES 5

#define TITLE_IDLE_DEMO 1200        // 20 s on the title screen starts the demo
#define TAP_GUARD 30                // ignore panic taps right after an overlay appears

#define MAX_INCOMING 128
#define MAX_ABMS 32
#define MAX_EXPLOSIONS 64

// ---------------------------------------------------------------------------
// Palette (one shared 256-colour palette for both engines; mirrors Palette.swift)

enum {
	C_BLACK = 0,
	C_GROUND, C_BASE, C_BASE_DEAD, C_CITY, C_CITY_DEAD,
	C_INC, C_INC_DIM, C_ABM, C_TARGET, C_HUD, C_HUD_DIM,
	C_STAR, C_STAR_DIM,
	C_SKY0 = 16,                    // 16 sky shades, 16..31 (shade 0 is black)
	C_EXP0 = 32,                    // 4 explosion shimmer colours
};

#define RGBF(r, g, b) RGB15((int)((r) * 31 + 0.5f), (int)((g) * 31 + 0.5f), (int)((b) * 31 + 0.5f))

static void setupPalette(u16 *pal) {
	pal[C_BLACK] = RGB15(0, 0, 0);
	pal[C_GROUND] = RGBF(0.16f, 0.42f, 1.00f);
	pal[C_BASE] = RGBF(0.30f, 0.60f, 1.00f);
	pal[C_BASE_DEAD] = RGBF(0.24f, 0.24f, 0.24f);
	pal[C_CITY] = RGBF(0.25f, 0.95f, 0.95f);
	pal[C_CITY_DEAD] = RGBF(0.20f, 0.20f, 0.20f);
	pal[C_INC] = RGBF(1.00f, 0.24f, 0.24f);
	pal[C_INC_DIM] = RGBF(0.50f, 0.12f, 0.12f);
	pal[C_ABM] = RGBF(0.65f, 1.00f, 1.00f);
	pal[C_TARGET] = RGBF(1.00f, 0.88f, 0.30f);
	pal[C_HUD] = RGBF(0.82f, 0.90f, 1.00f);
	pal[C_HUD_DIM] = RGBF(0.45f, 0.55f, 0.72f);
	pal[C_STAR] = RGBF(0.75f, 0.80f, 0.95f);
	pal[C_STAR_DIM] = RGBF(0.32f, 0.35f, 0.48f);
	for (int i = 0; i < 16; i++) {
		float t = i / 15.0f;  // black -> horizon blue
		pal[C_SKY0 + i] = RGBF(0.05f * t, 0.12f * t, 0.32f * t);
	}
	pal[C_EXP0 + 0] = RGBF(1.00f, 0.95f, 0.42f);
	pal[C_EXP0 + 1] = RGBF(1.00f, 0.60f, 0.16f);
	pal[C_EXP0 + 2] = RGBF(1.00f, 0.26f, 0.20f);
	pal[C_EXP0 + 3] = RGB15(31, 31, 31);
}

// ---------------------------------------------------------------------------
// Game state for tools/ndsemu.py: `peekw g_debug N` (ints only).

typedef struct {
	u32 magic;
	s32 frame;
	s32 state;
	s32 wave;
	s32 score;
	s32 high;
	s32 cities;      // cities alive
	s32 ammo;        // ammo left over all live bases
	s32 incoming;    // warheads in flight
	s32 abms;
	s32 explosions;
	s32 kills;       // warheads destroyed this game
	s32 lowestX;     // bottom-screen pixel of the lowest warhead on the bottom screen, or -1
	s32 lowestY;
	s32 fired;       // counter-missiles launched this game
	s32 fatOK;
	s32 demo;
	s32 cpuLines;    // scanlines spent on update+render last frame (263 = one frame)
	s32 cpuLinesMax;
} DebugState;
volatile DebugState g_debug;

// ---------------------------------------------------------------------------
// Entities

typedef enum { ST_TITLE, ST_PLAYING, ST_WAVE_CLEARED, ST_GAME_OVER, ST_PAUSED } GameState;

#define FX 8                        // positions are 24.8 fixed point
#define TOFX(v) ((s32)(v) << FX)

typedef struct {
	bool alive, claimed;
	s32 x, y, vx, vy, tx, ty, speed; // fixed point; speed is per frame
	s16 trailX, trailY;              // start of the moving trail segment
	s16 trunkX0, trunkY0, trunkX1, trunkY1;
	bool hasTrunk;
	s16 splitY;                      // world y where it MIRVs, or -1
	u8 splitCount;
	s8 targetKind, targetIdx;        // 0 = bare ground, 1 = city, 2 = base
} Incoming;

typedef struct {
	bool alive;
	s32 x, y, vx, vy, tx, ty;
	s16 sx, sy;
} Abm;

typedef struct {
	bool alive, damaging;
	s16 cx, cy, maxR, r;
	u8 age, phase;
} Explosion;

typedef struct {
	bool alive;
	int x, ammo, cooldown;
} Base;

typedef struct {
	bool alive;
	int x;
} City;

static Incoming g_inc[MAX_INCOMING];
static Abm g_abm[MAX_ABMS];
static Explosion g_exp[MAX_EXPLOSIONS];
static Base g_bases[NUM_BASES];
static City g_cities[NUM_CITIES];

static GameState g_state = ST_TITLE;
static bool g_demo;
static int g_frame, g_stateFrames, g_shake;
static int g_score, g_nextBonusCity, g_kills, g_fired;
static int g_best[MAX_SCORES], g_bestCount;
static int g_lastRank;               // rank of the last game, 0 = none
static bool g_fatOK;

// Wave manager
static int g_wave, g_remainingToSpawn;
static float g_spawnInterval, g_spawnTimer, g_incomingSpeed;
static int g_clearedCities, g_clearedAmmo, g_clearedBonus;

// ---------------------------------------------------------------------------
// Random

static u32 g_rng = 0x2545F491;
static u32 rnd(void) {
	u32 x = g_rng;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	return g_rng = x;
}
static float rndf(void) { return (rnd() >> 8) * (1.0f / 16777216.0f); }
static int rndi(int lo, int hi) { return lo + (int)(rnd() % (u32)(hi - lo + 1)); }

// ---------------------------------------------------------------------------
// Sound: tiny PSG/noise blips standing in for the iOS haptics

typedef struct { int id, frames, freq, dfreq; } Sfx;
static Sfx g_sfx[6];

static void sfx(bool noise, int freq, int dfreq, int vol, int frames) {
	Sfx *slot = &g_sfx[0];
	for (int i = 0; i < 6; i++) {
		if (g_sfx[i].frames <= 0) { slot = &g_sfx[i]; break; }
		if (g_sfx[i].frames < slot->frames) slot = &g_sfx[i];
	}
	if (slot->frames > 0) soundKill(slot->id);
	slot->id = noise ? soundPlayNoise(freq, vol, 64) : soundPlayPSG(DutyCycle_25, freq, vol, 64);
	slot->frames = slot->id >= 0 ? frames : 0;
	slot->freq = freq;
	slot->dfreq = dfreq;
}

static void sfxTick(void) {
	for (int i = 0; i < 6; i++) {
		Sfx *s = &g_sfx[i];
		if (s->frames <= 0) continue;
		if (--s->frames == 0) { soundKill(s->id); continue; }
		s->freq += s->dfreq;
		if (s->freq < 60) s->freq = 60;
		soundSetFreq(s->id, s->freq);
	}
}

static void sfxFire(void) { sfx(false, 1400, -60, 28, 6); }
static void sfxKill(void) { sfx(true, 2600, -90, 70, 10); }
static void sfxLost(void) { sfx(true, 700, -12, 127, 36); }

// ---------------------------------------------------------------------------
// Rendering primitives (8bpp back buffers in main RAM; index 0 is transparent
// and shows the static backdrop layer underneath)

#define MAX_R 24

static u8 fbTop[SW * SH] ALIGN(32);
static u8 fbBot[SW * SH] ALIGN(32);
static u8 g_circW[MAX_R + 1][MAX_R + 1];  // half-widths of filled circles
static const u8 *g_font;

static inline u8 *rowW(int y) {
	if (y < 0) return NULL;
	if (y < SH) return fbTop + y * SW;
	if (y >= BOT0 && y < WORLD_H) return fbBot + (y - BOT0) * SW;
	return NULL;
}

static inline void plotW(int x, int y, u8 c) {
	if ((unsigned)x >= SW) return;
	u8 *row = rowW(y);
	if (row) row[x] = c;
}

static void hlineW(int x0, int x1, int y, u8 c) {
	if (x0 < 0) x0 = 0;
	if (x1 > SW - 1) x1 = SW - 1;
	if (x0 > x1) return;
	u8 *row = rowW(y);
	if (row) memset(row + x0, c, x1 - x0 + 1);
}

static void rectW(int x, int y, int w, int h, u8 c) {
	for (int j = 0; j < h; j++) hlineW(x, x + w - 1, y + j, c);
}

// Bresenham clipped to the world rows [ylo, yhi] of one screen buffer (y0 <= y1).
static void lineBand(u8 *fb, int ylo, int yhi, int x0, int y0, int x1, int y1, u8 c) {
	if (y1 < ylo || y0 > yhi) return;
	if (y0 < ylo) { x0 += (x1 - x0) * (ylo - y0) / (y1 - y0); y0 = ylo; }
	if (y1 > yhi) { x1 = x0 + (x1 - x0) * (yhi - y0) / (y1 - y0); y1 = yhi; }
	u8 *row = fb + (y0 - ylo) * SW;
	int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
	int dy = y0 - y1;
	int err = dx + dy;
	for (;;) {
		if ((unsigned)x0 < SW) row[x0] = c;
		if (x0 == x1 && y0 == y1) break;
		int e2 = 2 * err;
		if (e2 >= dy) { err += dy; x0 += sx; }
		if (e2 <= dx) { err += dx; y0++; row += SW; }
	}
}

static void lineW(int x0, int y0, int x1, int y1, u8 c) {
	if (y0 > y1) {
		int t = x0; x0 = x1; x1 = t;
		t = y0; y0 = y1; y1 = t;
	}
	lineBand(fbTop, 0, SH - 1, x0, y0, x1, y1, c);
	lineBand(fbBot, BOT0, WORLD_H - 1, x0, y0, x1, y1, c);
}

static void circleW(int cx, int cy, int r, u8 c) {
	if (r <= 0) return;
	if (r > MAX_R) r = MAX_R;
	for (int dy = -r; dy <= r; dy++) {
		int w = g_circW[r][dy < 0 ? -dy : dy];
		hlineW(cx - w, cx + w, cy + dy, c);
	}
}

// Text from the libnds 8x8 console font, drawn into the bitmaps with a 1px shadow.
static void textW(int x, int y, const char *s, u8 c, int scale) {
	int n = strlen(s);
	for (int pass = 0; pass < 2; pass++) {
		int off = 1 - pass;
		u8 col = pass ? c : C_BLACK;
		for (int r = 0; r < 8 * scale; r++) {
			u8 *row = rowW(y + r + off);
			if (!row) continue;
			int gy = r / scale;
			for (int i = 0; i < n; i++) {
				u8 bits = g_font[(u8)s[i] * 8 + gy];
				for (int px = x + i * 8 * scale + off; bits; bits >>= 1, px += scale)
					if (bits & 1)
						for (int k = 0; k < scale; k++)
							if ((unsigned)(px + k) < SW) row[px + k] = col;
			}
		}
	}
}

static void textC(int y, const char *s, u8 c, int scale) {
	textW((SW - (int)strlen(s) * 8 * scale) / 2, y, s, c, scale);
}

// Static backdrops: starfield on top, the sky turning blue toward the ground.
// Drawn once through the back buffers and copied into the backdrop layers.
static void buildBackdrops(u16 *vramTop, u16 *vramBot) {
	for (int r = 0; r <= MAX_R; r++)
		for (int d = 0; d <= r; d++)
			g_circW[r][d] = (u8)sqrtf((float)(r * r - d * d) + 0.5f);

	static const u8 bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
	memset(fbTop, 0, sizeof fbTop);
	memset(fbBot, 0, sizeof fbBot);
	for (int y = GRAD_START; y < GROUND_Y; y++) {
		u8 *row = rowW(y);
		if (!row) continue;
		float level = (y - GRAD_START) * 15.0f / (GROUND_Y - GRAD_START);
		for (int x = 0; x < SW; x++) {
			int shade = (int)(level + bayer[y & 3][x & 3] / 16.0f);
			if (shade > 15) shade = 15;
			row[x] = shade ? C_SKY0 + shade : C_BLACK;
		}
	}
	rectW(0, GROUND_Y, SW, GROUND_H, C_GROUND);
	u32 saved = g_rng;
	for (int i = 0; i < 90; i++) {
		int x = rndi(0, SW - 1), y = rndi(14, GRAD_START + 20);
		plotW(x, y, (i & 3) ? C_STAR_DIM : C_STAR);
	}
	g_rng = saved;
	DC_FlushRange(fbTop, sizeof fbTop);
	DC_FlushRange(fbBot, sizeof fbBot);
	dmaCopyWords(3, fbTop, vramTop, sizeof fbTop);
	dmaCopyWords(3, fbBot, vramBot, sizeof fbBot);
}

// ---------------------------------------------------------------------------
// High scores (top 5 on the SD card next to the ROM, if FAT is available)

#define SAVE_PATH "missiledefender.sav"
#define SAVE_MAGIC 0x4D445331  // "MDS1"

static void loadScores(void) {
	g_bestCount = 0;
	if (!g_fatOK) return;
	FILE *f = fopen(SAVE_PATH, "rb");
	if (!f) return;
	u32 magic = 0;
	s32 n = 0;
	if (fread(&magic, 4, 1, f) == 1 && magic == SAVE_MAGIC && fread(&n, 4, 1, f) == 1 &&
	    n >= 0 && n <= MAX_SCORES && fread(g_best, 4, n, f) == (size_t)n)
		g_bestCount = n;
	fclose(f);
}

static void saveScores(void) {
	if (!g_fatOK) return;
	FILE *f = fopen(SAVE_PATH, "wb");
	if (!f) return;
	u32 magic = SAVE_MAGIC;
	s32 n = g_bestCount;
	fwrite(&magic, 4, 1, f);
	fwrite(&n, 4, 1, f);
	fwrite(g_best, 4, n, f);
	fclose(f);
}

static int highScore(void) { return g_bestCount ? g_best[0] : 0; }

// Commits the score to the table. Returns its 1-based rank if it made the top 5, else 0.
static int recordScore(int score) {
	if (score <= 0) return 0;
	bool qualifies = g_bestCount < MAX_SCORES || score > g_best[g_bestCount - 1];
	if (!qualifies) return 0;
	int i = g_bestCount < MAX_SCORES ? g_bestCount++ : MAX_SCORES - 1;
	while (i > 0 && g_best[i - 1] < score) { g_best[i] = g_best[i - 1]; i--; }
	g_best[i] = score;
	saveScores();
	int rank = 1;
	for (int j = 0; j < g_bestCount; j++)
		if (g_best[j] > score) rank++;
	return rank;
}

// ---------------------------------------------------------------------------
// Waves

static void startWave(int n) {
	g_wave = n;
	int total = BASE_INCOMING_COUNT + INCOMING_COUNT_PER_WAVE * (n - 1);
	g_remainingToSpawn = total < INCOMING_COUNT_MAX ? total : INCOMING_COUNT_MAX;
	g_spawnInterval = fmaxf(SPAWN_INTERVAL_MIN, SPAWN_INTERVAL_START - SPAWN_INTERVAL_DECAY * (n - 1));
	g_spawnTimer = FIRST_SPAWN_DELAY;
	g_incomingSpeed = fminf(INCOMING_SPEED_MAX, INCOMING_BASE_SPEED + INCOMING_SPEED_PER_WAVE * (n - 1));
}

static int multiplier(void) {
	if (g_wave < 3) return 1;
	if (g_wave < 5) return 2;
	if (g_wave < 7) return 3;
	if (g_wave < 9) return 4;
	if (g_wave < 11) return 5;
	return 6;
}

static float mirvChance(void) {
	if (g_wave < MIRV_FIRST_WAVE) return 0;
	return fminf(MIRV_CHANCE_MAX, MIRV_CHANCE_START + MIRV_CHANCE_PER_WAVE * (g_wave - MIRV_FIRST_WAVE));
}

// ---------------------------------------------------------------------------
// Installations

static int launchY(void) { return GROUND_Y - BASE_H; }
static bool canFire(const Base *b) { return b->alive && b->ammo > 0 && b->cooldown <= 0; }

static int citiesAlive(void) {
	int n = 0;
	for (int i = 0; i < NUM_CITIES; i++) n += g_cities[i].alive;
	return n;
}

static int ammoLeft(void) {
	int n = 0;
	for (int i = 0; i < NUM_BASES; i++)
		if (g_bases[i].alive) n += g_bases[i].ammo;
	return n;
}

static void reloadBases(void) {
	for (int i = 0; i < NUM_BASES; i++) {
		g_bases[i].alive = true;
		g_bases[i].ammo = AMMO_PER_BASE;
		g_bases[i].cooldown = 0;
	}
}

static void resetInstallations(void) {
	for (int i = 0; i < NUM_BASES; i++) g_bases[i].x = (int)(SW * BASE_X_FRACS[i]);
	for (int i = 0; i < NUM_CITIES; i++) {
		g_cities[i].x = (int)(SW * CITY_X_FRACS[i]);
		g_cities[i].alive = true;
	}
	reloadBases();
}

static void clearTransients(void) {
	memset(g_inc, 0, sizeof g_inc);
	memset(g_abm, 0, sizeof g_abm);
	memset(g_exp, 0, sizeof g_exp);
}

// ---------------------------------------------------------------------------
// Spawning

static void setCourse(Incoming *m, s32 tx, s32 ty) {
	m->tx = tx;
	m->ty = ty;
	float dx = (float)(tx - m->x), dy = (float)(ty - m->y);
	float len = fmaxf(1.0f, sqrtf(dx * dx + dy * dy));
	m->vx = (s32)(dx / len * m->speed);
	m->vy = (s32)(dy / len * m->speed);
}

static void randomGroundTarget(Incoming *m) {
	int kinds[NUM_CITIES + NUM_BASES], idx[NUM_CITIES + NUM_BASES], xs[NUM_CITIES + NUM_BASES];
	int n = 0;
	for (int i = 0; i < NUM_CITIES; i++)
		if (g_cities[i].alive) { kinds[n] = 1; idx[n] = i; xs[n++] = g_cities[i].x; }
	for (int i = 0; i < NUM_BASES; i++)
		if (g_bases[i].alive) { kinds[n] = 2; idx[n] = i; xs[n++] = g_bases[i].x; }
	int x;
	if (n > 0) {
		int k = rndi(0, n - 1);
		m->targetKind = kinds[k];
		m->targetIdx = idx[k];
		x = xs[k];
	} else {
		// Nothing left to aim at: target the ground so the missile still lands.
		m->targetKind = 0;
		m->targetIdx = 0;
		x = rndi(0, SW - 1);
	}
	setCourse(m, TOFX(x), TOFX(GROUND_Y));
}

static Incoming *newIncoming(s32 x, s32 y, s32 speed) {
	for (int i = 0; i < MAX_INCOMING; i++) {
		Incoming *m = &g_inc[i];
		if (m->alive) continue;
		memset(m, 0, sizeof *m);
		m->alive = true;
		m->x = x;
		m->y = y;
		m->speed = speed;
		m->trailX = x >> FX;
		m->trailY = y >> FX;
		m->splitY = -1;
		randomGroundTarget(m);
		return m;
	}
	return NULL;
}

static void spawnIncoming(void) {
	int x = (int)(SW * (0.05f + 0.9f * rndf()));
	Incoming *m = newIncoming(TOFX(x), TOFX(-4), (s32)(g_incomingSpeed * 256.0f / 60.0f));
	if (!m) return;
	if (g_wave >= MIRV_FIRST_WAVE && rndf() < mirvChance()) {
		float frac = MIRV_SPLIT_FRAC_MIN + (MIRV_SPLIT_FRAC_MAX - MIRV_SPLIT_FRAC_MIN) * rndf();
		m->splitY = (s16)(WORLD_H * (1.0f - frac));
		m->splitCount = rndi(MIRV_SPLIT_MIN, MIRV_SPLIT_MAX);
	}
}

static void splitMissile(Incoming *m) {
	for (int i = 0; i < m->splitCount - 1; i++) newIncoming(m->x, m->y, m->speed);
	// Freeze the descending trunk and start a fresh trail from the split point.
	m->hasTrunk = true;
	m->trunkX0 = m->trailX;
	m->trunkY0 = m->trailY;
	m->trunkX1 = m->x >> FX;
	m->trunkY1 = m->y >> FX;
	m->trailX = m->trunkX1;
	m->trailY = m->trunkY1;
	m->splitY = -1;
	randomGroundTarget(m);
}

static void spawnExplosion(int x, int y, int radius, bool damaging) {
	for (int i = 0; i < MAX_EXPLOSIONS; i++) {
		Explosion *e = &g_exp[i];
		if (e->alive) continue;
		e->alive = true;
		e->damaging = damaging;
		e->cx = x;
		e->cy = y;
		e->maxR = radius;
		e->r = 0;
		e->age = 0;
		e->phase = rnd() & 3;
		return;
	}
}

// ---------------------------------------------------------------------------
// Firing

static bool fireAt(int tx, int ty) {
	// Never aim below the battery tops, so every shot climbs.
	if (ty > launchY() - 2) ty = launchY() - 2;
	Base *best = NULL;
	int bestD = 0x7FFFFFFF;
	for (int i = 0; i < NUM_BASES; i++) {
		Base *b = &g_bases[i];
		if (!canFire(b)) continue;
		int dx = b->x - tx, dy = launchY() - ty, d = dx * dx + dy * dy;
		if (d < bestD) { bestD = d; best = b; }
	}
	if (!best) return false;  // out of ammo everywhere
	for (int i = 0; i < MAX_ABMS; i++) {
		Abm *a = &g_abm[i];
		if (a->alive) continue;
		best->ammo--;
		best->cooldown = FIRE_COOLDOWN;
		float speed = (best == &g_bases[1] ? ABM_SPEED_CENTER : ABM_SPEED) * 256.0f / 60.0f;
		float dx = (float)(tx - best->x), dy = (float)(ty - launchY());
		float len = fmaxf(1.0f, sqrtf(dx * dx + dy * dy));
		a->alive = true;
		a->sx = best->x;
		a->sy = launchY();
		a->x = TOFX(a->sx);
		a->y = TOFX(a->sy);
		a->tx = TOFX(tx);
		a->ty = TOFX(ty);
		a->vx = (s32)(dx / len * speed);
		a->vy = (s32)(dy / len * speed);
		g_fired++;
		sfxFire();
		return true;
	}
	return false;
}

// Demo mode autopilot: lead the lowest unclaimed warhead on the bottom screen.
static void demoThink(void) {
	if (g_frame % 18) return;
	Incoming *pick = NULL;
	for (int i = 0; i < MAX_INCOMING; i++) {
		Incoming *m = &g_inc[i];
		if (!m->alive || m->claimed || (m->y >> FX) < BOT0 + 10) continue;
		if (!pick || m->y > pick->y) pick = m;
	}
	if (!pick) return;
	s32 px = pick->x, py = pick->y;
	for (int iter = 0; iter < 2; iter++) {
		int dx = (px >> FX) - SW / 2, dy = launchY() - (py >> FX);
		float t = sqrtf((float)(dx * dx + dy * dy)) / (ABM_SPEED / 60.0f) + EXP_EXPAND / 2;
		px = pick->x + (s32)(pick->vx * t);
		py = pick->y + (s32)(pick->vy * t);
	}
	if ((py >> FX) < launchY() - 6 && fireAt(px >> FX, py >> FX)) pick->claimed = true;
}

// ---------------------------------------------------------------------------
// Game flow

static void showTitle(void) {
	clearTransients();
	resetInstallations();
	g_state = ST_TITLE;
	g_stateFrames = 0;
	g_demo = false;
	g_score = 0;
	g_wave = 1;
}

static void startGame(bool demo) {
	g_rng ^= (u32)g_frame * 2654435761u;
	if (!g_rng) g_rng = 1;
	clearTransients();
	resetInstallations();
	g_score = 0;
	g_kills = 0;
	g_fired = 0;
	g_nextBonusCity = BONUS_CITY_EVERY;
	g_demo = demo;
	g_state = ST_PLAYING;
	g_stateFrames = 0;
	startWave(1);
}

static void startNextWave(void) {
	clearTransients();
	reloadBases();  // cities persist between waves
	g_state = ST_PLAYING;
	g_stateFrames = 0;
	startWave(g_wave + 1);
}

static void endWave(void) {
	g_state = ST_WAVE_CLEARED;
	g_stateFrames = 0;
	int mult = multiplier();
	g_clearedAmmo = ammoLeft();
	g_clearedCities = citiesAlive();
	g_clearedBonus = g_clearedAmmo * POINTS_PER_UNUSED_ABM * mult + g_clearedCities * POINTS_PER_SAVED_CITY * mult;
	g_score += g_clearedBonus;
	// Every 10,000 points rebuilds one destroyed city.
	while (g_score >= g_nextBonusCity) {
		g_nextBonusCity += BONUS_CITY_EVERY;
		for (int i = 0; i < NUM_CITIES; i++)
			if (!g_cities[i].alive) { g_cities[i].alive = true; break; }
	}
}

static void gameOver(void) {
	g_state = ST_GAME_OVER;
	g_stateFrames = 0;
	g_lastRank = g_demo ? 0 : recordScore(g_score);
}

static void installationLost(void) {
	g_shake = 8;
	sfxLost();
}

static void hitGround(const Incoming *m) {
	if (m->targetKind == 1 && g_cities[m->targetIdx].alive) {
		g_cities[m->targetIdx].alive = false;
		installationLost();
		if (citiesAlive() == 0 && g_state == ST_PLAYING) gameOver();
	} else if (m->targetKind == 2 && g_bases[m->targetIdx].alive) {
		g_bases[m->targetIdx].alive = false;
		g_bases[m->targetIdx].ammo = 0;
		installationLost();
	}
}

static bool passed(s32 x, s32 y, s32 tx, s32 ty, s32 vx, s32 vy) {
	return (s64)(tx - x) * vx + (s64)(ty - y) * vy <= 0;
}

// One simulation frame. After game over the scene keeps moving, but nothing scores.
static void step(void) {
	bool live = g_state == ST_PLAYING;
	for (int i = 0; i < NUM_BASES; i++)
		if (g_bases[i].cooldown > 0) g_bases[i].cooldown--;

	if (live && g_remainingToSpawn > 0 && (g_spawnTimer -= 1.0f) <= 0) {
		g_spawnTimer += g_spawnInterval;
		g_remainingToSpawn--;
		spawnIncoming();
	}

	if (live && g_demo) demoThink();

	for (int i = 0; i < MAX_ABMS; i++) {
		Abm *a = &g_abm[i];
		if (!a->alive) continue;
		a->x += a->vx;
		a->y += a->vy;
		if (passed(a->x, a->y, a->tx, a->ty, a->vx, a->vy)) {
			a->alive = false;
			spawnExplosion(a->tx >> FX, a->ty >> FX, EXPLOSION_RADIUS, true);
		}
	}

	for (int i = 0; i < MAX_EXPLOSIONS; i++) {
		Explosion *e = &g_exp[i];
		if (!e->alive) continue;
		int age = ++e->age;
		if (age < EXP_EXPAND) e->r = e->maxR * age / EXP_EXPAND;
		else if (age < EXP_EXPAND + EXP_HOLD) e->r = e->maxR;
		else if (age < EXP_EXPAND + EXP_HOLD + EXP_CONTRACT)
			e->r = e->maxR * (EXP_EXPAND + EXP_HOLD + EXP_CONTRACT - age) / EXP_CONTRACT;
		else e->alive = false;
	}

	for (int i = 0; i < MAX_INCOMING; i++) {
		Incoming *m = &g_inc[i];
		if (!m->alive) continue;
		m->x += m->vx;
		m->y += m->vy;
		if (m->splitY >= 0 && (m->y >> FX) >= m->splitY) splitMissile(m);
	}

	// Blasts destroy warheads.
	bool killed = false;
	for (int j = 0; j < MAX_EXPLOSIONS; j++) {
		Explosion *e = &g_exp[j];
		if (!e->alive || !e->damaging || e->r <= 0) continue;
		int r2 = e->r * e->r;
		for (int i = 0; i < MAX_INCOMING; i++) {
			Incoming *m = &g_inc[i];
			if (!m->alive) continue;
			int dx = (m->x >> FX) - e->cx, dy = (m->y >> FX) - e->cy;
			if (dx * dx + dy * dy <= r2) {
				m->alive = false;
				killed = true;
				if (live) {
					g_score += POINTS_PER_MISSILE * multiplier();
					g_kills++;
				}
			}
		}
	}
	if (killed) sfxKill();

	// Landed warheads hit the ground.
	for (int i = 0; i < MAX_INCOMING; i++) {
		Incoming *m = &g_inc[i];
		if (!m->alive || !passed(m->x, m->y, m->tx, m->ty, m->vx, m->vy)) continue;
		m->alive = false;
		spawnExplosion(m->tx >> FX, m->ty >> FX, GROUND_BURST_RADIUS, false);
		hitGround(m);
	}

	if (g_state == ST_PLAYING && g_remainingToSpawn == 0) {
		bool any = false;
		for (int i = 0; i < MAX_INCOMING && !any; i++) any = g_inc[i].alive;
		if (!any) endWave();
	}
}

// ---------------------------------------------------------------------------
// Drawing

static void drawBase(const Base *b) {
	u8 c = b->alive ? C_BASE : C_BASE_DEAD;
	int h = b->alive ? BASE_H : BASE_H / 3;
	for (int i = 0; i < h; i++) {
		int half = (BASE_W / 2) * (i + 1) / h;
		hlineW(b->x - half, b->x + half, GROUND_Y - h + i, c);
	}
	// Remaining ammo as a small pyramid above the battery, filled from the bottom row up.
	static const int rows[4] = {4, 3, 2, 1};
	int placed = 0;
	for (int r = 0; r < 4 && placed < b->ammo && b->alive; r++)
		for (int col = 0; col < rows[r] && placed < b->ammo; col++, placed++) {
			int x = b->x + col * 4 - (rows[r] - 1) * 2;
			rectW(x, GROUND_Y - BASE_H - 4 - r * 3, 2, 2, C_ABM);
		}
}

static void drawCity(const City *c) {
	u8 col = c->alive ? C_CITY : C_CITY_DEAD;
	int s = c->alive ? 100 : 35;  // flatten the silhouette when destroyed (rubble)
	int left = c->x - CITY_W / 2;
	int h1 = CITY_H * 55 * s / 10000, h2 = CITY_H * s / 100, h3 = CITY_H * 78 * s / 10000;
	if (h1 < 1) h1 = 1;
	rectW(left, GROUND_Y - h1, CITY_W, h1, col);
	rectW(left, GROUND_Y - h2, 5, h2, col);
	rectW(c->x + 3, GROUND_Y - h3, 5, h3, col);
}

static void drawWorld(void) {
	for (int i = 0; i < NUM_CITIES; i++) drawCity(&g_cities[i]);
	for (int i = 0; i < NUM_BASES; i++) drawBase(&g_bases[i]);

	for (int i = 0; i < MAX_INCOMING; i++) {
		const Incoming *m = &g_inc[i];
		if (!m->alive) continue;
		int x = m->x >> FX, y = m->y >> FX;
		if (m->hasTrunk) lineW(m->trunkX0, m->trunkY0, m->trunkX1, m->trunkY1, C_INC);
		lineW(m->trailX, m->trailY, x, y, C_INC);
		plotW(x - 1, y, C_INC);
		plotW(x + 1, y, C_INC);
		plotW(x, y - 1, C_INC);
		plotW(x, y + 1, C_INC);
		plotW(x, y, C_HUD);
		// Entry marker on the top edge of the bottom screen: where it will come through.
		if (y < BOT0 && m->vy > 0) {
			int ex = x + (int)((s64)m->vx * (TOFX(BOT0) - m->y) / ((s64)m->vy << FX));
			u8 c = y >= SH - 40 ? C_INC : C_INC_DIM;
			hlineW(ex - 2, ex + 2, BOT0, c);
			hlineW(ex - 1, ex + 1, BOT0 + 1, c);
			plotW(ex, BOT0 + 2, c);
		}
	}

	for (int i = 0; i < MAX_ABMS; i++) {
		const Abm *a = &g_abm[i];
		if (!a->alive) continue;
		int tx = a->tx >> FX, ty = a->ty >> FX;
		lineW(tx - 3, ty - 3, tx + 3, ty + 3, C_TARGET);
		lineW(tx - 3, ty + 3, tx + 3, ty - 3, C_TARGET);
		lineW(a->sx, a->sy, a->x >> FX, a->y >> FX, C_ABM);
		rectW((a->x >> FX) - 1, (a->y >> FX) - 1, 2, 2, C_ABM);
	}

	for (int i = 0; i < MAX_EXPLOSIONS; i++) {
		const Explosion *e = &g_exp[i];
		if (!e->alive) continue;
		circleW(e->cx, e->cy, e->r, C_EXP0 + ((e->age / EXP_COLOR_PERIOD + e->phase) & 3));
	}
}

static void drawHud(void) {
	char buf[32];
	sprintf(buf, "HI %06d", g_score > highScore() && !g_demo ? g_score : highScore());
	textW(4, 4, buf, C_HUD_DIM, 1);
	sprintf(buf, "%06d", g_score);
	textC(1, buf, C_HUD, 2);
	sprintf(buf, "WAVE %d", g_wave);
	textW(SW - 4 - (int)strlen(buf) * 8, 4, buf, C_HUD_DIM, 1);
}

#define BOT(y) (BOT0 + (y))

static void drawOverlays(void) {
	char buf[40];
	bool blink = (g_frame / 20) & 1;
	switch (g_state) {
	case ST_TITLE:
		textC(44, "MISSILE", C_CITY, 3);
		textC(72, "DEFENDER", C_CITY, 3);
		textC(112, "THEY WILL KEEP COMING.", C_HUD, 1);
		textC(124, "YOU CANNOT WIN.", C_HUD, 1);
		textC(136, "ONLY DELAY THE END.", C_HUD, 1);
		if (blink || g_stateFrames < 40) textC(BOT(30), "TAP TO HOLD THE LINE", C_TARGET, 1);
		textC(BOT(56), "- FINAL STANDS -", C_HUD_DIM, 1);
		if (!g_bestCount) textC(BOT(72), "NO ONE HAS STOOD YET", C_HUD_DIM, 1);
		for (int i = 0; i < g_bestCount; i++) {
			sprintf(buf, "%d.  %06d", i + 1, g_best[i]);
			textC(BOT(70 + i * 12), buf, i == 0 ? C_HUD : C_HUD_DIM, 1);
		}
		textC(BOT(136), "STYLUS FIRE   START PAUSE", C_HUD_DIM, 1);
		break;
	case ST_PLAYING:
		if (g_demo) {
			textC(SH - 12, "DEMO", C_HUD_DIM, 1);
			if (blink) textC(BOT(40), "TAP TO PLAY", C_TARGET, 1);
		}
		break;
	case ST_WAVE_CLEARED:
		sprintf(buf, "WAVE %d CLEARED", g_wave);
		textC(BOT(28), buf, C_CITY, 2);
		sprintf(buf, "CITIES  %d x %d x%d", g_clearedCities, POINTS_PER_SAVED_CITY, multiplier());
		textC(BOT(58), buf, C_HUD, 1);
		sprintf(buf, "AMMO    %d x %d x%d", g_clearedAmmo, POINTS_PER_UNUSED_ABM, multiplier());
		textC(BOT(72), buf, C_HUD, 1);
		sprintf(buf, "BONUS   %d", g_clearedBonus);
		textC(BOT(88), buf, C_TARGET, 2);
		if (!g_demo) textC(BOT(118), "TAP TO CONTINUE", C_HUD_DIM, 1);
		break;
	case ST_GAME_OVER:
		textC(64, "THE END", C_INC, 4);
		sprintf(buf, "YOU REACHED WAVE %d", g_wave);
		textC(BOT(24), buf, C_HUD, 1);
		sprintf(buf, "SCORE  %06d", g_score);
		textC(BOT(40), buf, C_HUD, 1);
		if (g_lastRank == 1) {
			textC(BOT(64), "THE LONGEST ANYONE", C_TARGET, 1);
			textC(BOT(76), "HAS EVER STOOD.", C_TARGET, 1);
		} else if (g_lastRank > 1) {
			sprintf(buf, "A STAND WORTH REMEMBERING #%d", g_lastRank);
			textC(BOT(64), buf, C_CITY, 1);
		} else {
			textC(BOT(64), "THE CITIES ARE ASH.", C_HUD_DIM, 1);
			textC(BOT(76), "AS THEY ALWAYS ARE.", C_HUD_DIM, 1);
		}
		if (g_stateFrames > 60) textC(BOT(108), "TAP TO CONTINUE", C_HUD_DIM, 1);
		break;
	case ST_PAUSED:
		textC(BOT(50), "PAUSED", C_HUD, 3);
		textC(BOT(90), "TAP TO RESUME", C_HUD_DIM, 1);
		break;
	}
}

static void render(void) {
	memset(fbTop, 0, sizeof fbTop);
	memset(fbBot, 0, sizeof fbBot);
	drawWorld();
	drawHud();
	drawOverlays();
}

// ---------------------------------------------------------------------------

static void update(u32 down, bool tapped, int tx, int ty) {
	g_stateFrames++;
	bool any = tapped || (down & (KEY_A | KEY_B | KEY_X | KEY_Y | KEY_START));
	switch (g_state) {
	case ST_TITLE:
		if (any) startGame(false);
		else if (g_stateFrames > TITLE_IDLE_DEMO) startGame(true);
		break;
	case ST_PLAYING:
		if (g_demo) {
			if (any) { showTitle(); break; }
		} else if (down & KEY_START) {
			g_state = ST_PAUSED;
			break;
		} else if (tapped) {
			fireAt(tx, ty);
		}
		step();
		break;
	case ST_WAVE_CLEARED:
		step();  // let blasts and stray counter-missiles finish
		if (g_demo && any) showTitle();
		else if (g_stateFrames > INTER_WAVE_DELAY || (any && g_stateFrames > TAP_GUARD)) startNextWave();
		break;
	case ST_GAME_OVER:
		step();
		if (g_demo ? (any || g_stateFrames > 300) : (any && g_stateFrames > 60)) showTitle();
		break;
	case ST_PAUSED:
		if (any) g_state = ST_PLAYING;
		break;
	}
}

static void updateDebug(void) {
	g_debug.magic = 0x21474244;  // "DBG!"
	g_debug.frame = g_frame;
	g_debug.state = g_state;
	g_debug.wave = g_wave;
	g_debug.score = g_score;
	g_debug.high = highScore();
	g_debug.cities = citiesAlive();
	g_debug.ammo = ammoLeft();
	int inc = 0, abms = 0, exps = 0, lowY = -1, lowX = -1;
	for (int i = 0; i < MAX_INCOMING; i++) {
		if (!g_inc[i].alive) continue;
		inc++;
		int y = (g_inc[i].y >> FX) - BOT0;
		if (y >= 0 && y > lowY) { lowY = y; lowX = g_inc[i].x >> FX; }
	}
	for (int i = 0; i < MAX_ABMS; i++) abms += g_abm[i].alive;
	for (int i = 0; i < MAX_EXPLOSIONS; i++) exps += g_exp[i].alive;
	g_debug.incoming = inc;
	g_debug.abms = abms;
	g_debug.explosions = exps;
	g_debug.kills = g_kills;
	g_debug.lowestX = lowX;
	g_debug.lowestY = lowY;
	g_debug.fired = g_fired;
	g_debug.fatOK = g_fatOK;
	g_debug.demo = g_demo;
}

int main(void) {
	// Main engine on the top screen, sub engine on the touch screen. Each has a
	// static 8bpp backdrop (BG2) under an 8bpp sprite-like scene layer (BG3).
	videoSetMode(MODE_5_2D);
	videoSetModeSub(MODE_5_2D);
	vramSetBankA(VRAM_A_MAIN_BG);
	vramSetBankC(VRAM_C_SUB_BG);
	int bgT2 = bgInit(2, BgType_Bmp8, BgSize_B8_256x256, 0, 0);
	int bgT = bgInit(3, BgType_Bmp8, BgSize_B8_256x256, 4, 0);
	int bgB2 = bgInitSub(2, BgType_Bmp8, BgSize_B8_256x256, 0, 0);
	int bgB = bgInitSub(3, BgType_Bmp8, BgSize_B8_256x256, 4, 0);
	bgSetPriority(bgT, 0);
	bgSetPriority(bgT2, 1);
	bgSetPriority(bgB, 0);
	bgSetPriority(bgB2, 1);
	TIMER_CR(0) = TIMER_ENABLE | TIMER_DIV_1024;  // frame-cost probe
	setupPalette(BG_PALETTE);
	setupPalette(BG_PALETTE_SUB);
	g_font = (const u8 *)consoleGetDefault()->font.gfx;

	soundEnable();
	g_fatOK = fatInitDefault();
	loadScores();

	buildBackdrops(bgGetGfxPtr(bgT2), bgGetGfxPtr(bgB2));
	memset(fbTop, 0, sizeof fbTop);
	memset(fbBot, 0, sizeof fbBot);
	showTitle();

	while (pmMainLoop()) {
		u16 tStart = TIMER_DATA(0);
		scanKeys();
		u32 down = keysDown();
		touchPosition tp = {0};
		bool tapped = false;
		if (down & KEY_TOUCH) {
			touchRead(&tp);
			tapped = true;
		}

		update(down, tapped, tp.px, BOT0 + tp.py);
		sfxTick();
		render();
		int lines = (u16)(TIMER_DATA(0) - tStart) * 263 / 546;
		g_debug.cpuLines = lines;
		if (lines > g_debug.cpuLinesMax) g_debug.cpuLinesMax = lines;
		updateDebug();
		g_frame++;

		swiWaitForVBlank();
		DC_FlushRange(fbTop, sizeof fbTop);
		DC_FlushRange(fbBot, sizeof fbBot);
		dmaCopyWords(3, fbTop, bgGetGfxPtr(bgT), sizeof fbTop);
		dmaCopyWords(3, fbBot, bgGetGfxPtr(bgB), sizeof fbBot);
		// Screen shake on the bottom screen when a city or battery is lost.
		static const s8 shake[8] = {0, -2, -4, -2, 0, 2, 4, 2};
		int sx = g_shake > 0 ? shake[--g_shake] : 0;
		bgSetScroll(bgB, sx, 0);
		bgSetScroll(bgB2, sx, 0);
		bgUpdate();
	}
	return 0;
}
