/*
  ESP32 ARCADE HUB  v2
  Board: ESP32-WROOM-32 DevKit
  Display: 1.8" ST7735 SPI TFT, 128x160
  Libraries: Adafruit GFX + Adafruit ST7735 (Preferences is built into the ESP32 core)

  TFT wiring:
    VCC -> 3V3       LED -> 3V3
    GND -> GND       SCK -> GPIO18
    SDA/MOSI -> GPIO23
    CS -> GPIO5      DC/A0 -> GPIO2
    RESET -> GPIO4

  Joystick:
    VRx -> GPIO34    VRy -> GPIO35
    SW  -> GPIO27    VCC -> 3V3, GND -> GND

  BOOT (GPIO0) returns to the menu. (Do not use EN as an input, it is the reset line.)

  Controls
    Menu     : joystick up/down to browse, click to launch
    Pac-Man  : joystick steers (turn is buffered until the next junction)
    Snake    : joystick steers
    Invaders : left/right move, click fires
    Tetris   : left/right move, down soft drop, UP hard drop, click rotates
    Flappy   : click flaps

  What's new in v2
    - Up/down fixed (see UD_SIGN / LR_SIGN below if you ever need to flip an axis)
    - Whole frame is drawn off-screen and pushed at once -> no more flicker
    - Fixed 30 FPS game loop, smooth motion, particle effects, starfields
    - High scores saved to flash (survive power off)
    - Pac-Man: real ghosts, power pellets, lives, levels, tunnel
    - Invaders: animated aliens, shields, UFO, waves
    - Tetris: ghost piece, next preview, hard drop, wall kicks, levels, line-clear flash
    - Flappy: pipes with caps, parallax clouds, scrolling ground, ready screen
*/

#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>
#include <Preferences.h>
#include <math.h>
#include <string.h>
#include <stdio.h>

#define TFT_CS   5
#define TFT_DC   2
#define TFT_RST  4
#define JOY_X    34
#define JOY_Y    35
#define JOY_SW   27
#define BOOT_PIN 0

#define SCR_W    128
#define SCR_H    160
#define FRAME_MS 33

// ---------- Joystick orientation ----------
// Your stick is mounted rotated: VRx is the vertical axis, VRy the horizontal one.
// UD_SIGN fixes the inverted up/down. If it ever feels reversed again, set it to -1.
// If left/right feel reversed, set LR_SIGN to 1.
const int UD_SIGN = 1;
const int LR_SIGN = -1;
const int JOY_THRESH = 900;

Adafruit_ST7735 tft(TFT_CS, TFT_DC, TFT_RST);
GFXcanvas16 *cv;
#define gfx (*cv)
Preferences prefs;

// ---------- Colours ----------
#define C565(r,g,b) ((uint16_t)((((r)&0xF8)<<8)|(((g)&0xFC)<<3)|(((b)&0xF8)>>3)))
const uint16_t BLACK   = 0x0000;
const uint16_t WHITE   = 0xFFFF;
const uint16_t YELLOW  = 0xFFE0;
const uint16_t CYAN    = 0x07FF;
const uint16_t GREEN   = 0x07E0;
const uint16_t RED     = 0xF800;
const uint16_t BLUE    = 0x001F;
const uint16_t MAGENTA = 0xF81F;
const uint16_t ORANGE  = 0xFD20;
const uint16_t GRAY    = 0x8410;
const uint16_t NAVY    = 0x000F;

uint16_t mix565(uint16_t a, uint16_t b, uint8_t t) {   // t: 0 = a, 255 = b
  int ar = (a >> 11) & 31, ag = (a >> 5) & 63, ab = a & 31;
  int br = (b >> 11) & 31, bg = (b >> 5) & 63, bb = b & 31;
  int r  = ar + ((br - ar) * t) / 255;
  int g2 = ag + ((bg - ag) * t) / 255;
  int b2 = ab + ((bb - ab) * t) / 255;
  return (uint16_t)((r << 11) | (g2 << 5) | b2);
}

static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }
static inline int iclamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline float fclamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ---------- Global state ----------
enum Screen { MENU, PLAYING, OVER };
Screen screen = MENU;
const char* gameNames[5] = {"PAC-MAN", "SNAKE", "INVADERS", "TETRIS", "FLAPPY"};
const uint16_t gameCols[5] = {YELLOW, GREEN, CYAN, MAGENTA, ORANGE};
int sel = 0, cur = 0;
uint32_t best[5] = {0, 0, 0, 0, 0};
int score = 0, lives = 3;
bool gameEnded = false, newBest = false, overShown = false;
unsigned long tNow = 0, lastFrame = 0, overAt = 0;
float dt = 0.033f;

// ---------- Input ----------
int cenX = 2048, cenY = 2048;
int jx = 0, jy = 0, prevJy = 0;          // 4-way: jx -1 left / +1 right, jy -1 up / +1 down
bool clickNow = false, clickPrev = false, clickEvent = false, upEdge = false, bootPrev = false;
unsigned long lastClick = 0;

void readControls() {
  int rx = (analogRead(JOY_X) + analogRead(JOY_X)) / 2;
  int ry = (analogRead(JOY_Y) + analogRead(JOY_Y)) / 2;
  int v = UD_SIGN * (rx - cenX);          // + = down
  int h = LR_SIGN * (ry - cenY);          // + = right
  int av = abs(v), ah = abs(h);
  jx = 0; jy = 0;
  if (av > JOY_THRESH || ah > JOY_THRESH) {
    if (ah >= av) jx = (h > 0) ? 1 : -1;
    else          jy = (v > 0) ? 1 : -1;
  }
  upEdge = (jy < 0 && prevJy >= 0);
  prevJy = jy;
  clickNow = digitalRead(JOY_SW) == LOW;
  clickEvent = clickNow && !clickPrev && (tNow - lastClick > 150);
  if (clickEvent) lastClick = tNow;
  clickPrev = clickNow;
}

bool bootPressed() {
  bool n = digitalRead(BOOT_PIN) == LOW;
  bool e = n && !bootPrev;
  bootPrev = n;
  return e;
}

struct Rep { int last; unsigned long next; };
bool repStep(int &last, unsigned long &next,
             int v, unsigned long first, unsigned long rate) {

  if (v == 0) {
    last = 0;
    return false;
  }

  if (v != last) {
    last = v;
    next = tNow + first;
    return true;
  }

  if (tNow >= next) {
    next = tNow + rate;
    return true;
  }

  return false;
}

// ---------- Drawing helpers ----------
void pushFrame() {
  tft.startWrite();
  tft.setAddrWindow(0, 0, SCR_W, SCR_H);
  tft.writePixels(cv->getBuffer(), (uint32_t)SCR_W * SCR_H);
  tft.endWrite();
}

void centerText(const char* s, int y, uint16_t c, uint8_t size = 1) {
  gfx.setTextSize(size);
  gfx.setTextColor(c);
  int16_t x1, y1; uint16_t w, h;
  gfx.getTextBounds(s, 0, y, &x1, &y1, &w, &h);
  gfx.setCursor((SCR_W - w) / 2, y);
  gfx.print(s);
}

void outlinedText(const char* s, int x, int y, uint8_t size, uint16_t c, uint16_t o) {
  gfx.setTextSize(size);
  gfx.setTextColor(o);
  for (int dx = -1; dx <= 1; dx++)
    for (int dy = -1; dy <= 1; dy++)
      if (dx || dy) { gfx.setCursor(x + dx, y + dy); gfx.print(s); }
  gfx.setTextColor(c);
  gfx.setCursor(x, y);
  gfx.print(s);
}

void outlinedCenter(const char* s, int y, uint8_t size, uint16_t c, uint16_t o) {
  int w = strlen(s) * 6 * size;
  outlinedText(s, (SCR_W - w) / 2, y, size, c, o);
}

void hud(const char* title, uint16_t col) {
  gfx.fillRect(0, 0, SCR_W, 13, mix565(col, BLACK, 200));
  gfx.drawFastHLine(0, 13, SCR_W, col);
  gfx.setTextSize(1);
  gfx.setTextColor(WHITE);
  gfx.setCursor(4, 3);
  gfx.print(title);
  char b[16]; snprintf(b, sizeof b, "%d", score);
  gfx.setCursor(124 - (int)strlen(b) * 6, 3);
  gfx.print(b);
}

void drawSprite(const char* const* rows, int w, int h, int x, int y, uint16_t c) {
  for (int r = 0; r < h; r++) {
    const char* s = rows[r];
    for (int k = 0; k < w; k++) if (s[k] == '#') gfx.drawPixel(x + k, y + r, c);
  }
}

// ---------- Particles ----------
struct Particle { float x, y, vx, vy; int life; uint16_t c; };
Particle parts[40];

void clearParticles() { for (int i = 0; i < 40; i++) parts[i].life = 0; }
void burst(float x, float y, uint16_t c, int n) {
  for (int k = 0; k < n; k++) {
    for (int i = 0; i < 40; i++) if (parts[i].life <= 0) {
      parts[i].x = x; parts[i].y = y;
      parts[i].vx = random(-100, 101) / 45.0f;
      parts[i].vy = random(-100, 101) / 45.0f - 0.5f;
      parts[i].life = random(10, 22);
      parts[i].c = c;
      break;
    }
  }
}
void updateParticles() {
  for (int i = 0; i < 40; i++) if (parts[i].life > 0) {
    parts[i].x += parts[i].vx; parts[i].y += parts[i].vy;
    parts[i].vy += 0.07f; parts[i].life--;
  }
}
void drawParticles() {
  for (int i = 0; i < 40; i++) if (parts[i].life > 0) {
    uint16_t c = parts[i].life < 6 ? mix565(parts[i].c, BLACK, 150) : parts[i].c;
    gfx.fillRect((int)parts[i].x, (int)parts[i].y, 2, 2, c);
  }
}

// ---------- Starfield ----------
struct Star { float x, y, s; };
Star stars[30];
void initStars() {
  for (int i = 0; i < 30; i++) { stars[i].x = random(SCR_W); stars[i].y = random(SCR_H); stars[i].s = 0.4f + random(0, 100) / 70.0f; }
}
void drawStars(bool move) {
  for (int i = 0; i < 30; i++) {
    if (move) { stars[i].y += stars[i].s * 0.6f; if (stars[i].y >= SCR_H) { stars[i].y = 0; stars[i].x = random(SCR_W); } }
    float s = stars[i].s;
    uint16_t c = s > 1.2f ? WHITE : (s > 0.8f ? C565(150, 150, 190) : C565(70, 70, 110));
    gfx.drawPixel((int)stars[i].x, (int)stars[i].y, c);
  }
}

// =====================================================================
//                              PAC-MAN
// =====================================================================
const int MZW = 15, MZH = 15;
const char* const mazeT[MZH] = {
  "###############",
  "#o...........o#",
  "#.###.#.#.###.#",
  "#.............#",
  "#.##.#...#.##.#",
  "#....#...#....#",
  "###.##...##.###",
  ".....#...#.....",
  "###.##...##.###",
  "#....#...#....#",
  "#.##.#...#.##.#",
  "#.............#",
  "#.###.#.#.###.#",
  "#o...........o#",
  "###############"
};
char maze[MZH][MZW + 1];
int pmx, pmy, pmpx, pmpy, pmdx, pmdy, pmwx, pmwy, pmfx = 1, pmfy = 0;
struct Ghost { int x, y, px, py, dx, dy; bool fright; unsigned long release; };
Ghost gh[3];
const int gsx[3] = {7, 6, 8};
const int gsy[3] = {6, 7, 7};
const int chaseP[3] = {85, 60, 72};
const unsigned long relDelay[3] = {500, 3000, 6000};
unsigned long pmTick = 0, pmInterval = 150, pmPauseUntil = 0, frightUntil = 0;
int pelletsLeft = 0, pmLevel = 1, pmTickCount = 0, ghostCombo = 0;
bool pmNeedReset = false;

bool pmWall(int x, int y) { return x < 0 || y < 0 || x >= MZW || y >= MZH || maze[y][x] == '#'; }
bool pmCan(int x, int y, int dx, int dy) {
  int nx = (x + dx + MZW) % MZW, ny = y + dy;
  if (ny < 0 || ny >= MZH) return false;
  return maze[ny][nx] != '#';
}

void pmLoadMaze() {
  pelletsLeft = 0;
  for (int y = 0; y < MZH; y++) {
    strcpy(maze[y], mazeT[y]);
    for (int x = 0; x < MZW; x++) if (maze[y][x] == '.' || maze[y][x] == 'o') pelletsLeft++;
  }
  maze[13][7] = ' '; pelletsLeft--;   // Pac-Man's start tile has no pellet
}

void pmResetGhost(int i, unsigned long delayMs) {
  gh[i].x = gh[i].px = gsx[i]; gh[i].y = gh[i].py = gsy[i];
  gh[i].dx = 0; gh[i].dy = 0; gh[i].fright = false; gh[i].release = tNow + delayMs;
}
void pmReset() {
  pmx = pmpx = 7; pmy = pmpy = 13;
  pmdx = pmdy = pmwx = pmwy = 0; pmfx = 1; pmfy = 0;
  for (int i = 0; i < 3; i++) pmResetGhost(i, relDelay[i]);
  frightUntil = 0;
}

void initPacman() {
  pmLevel = 1; pmInterval = 150; pmTickCount = 0; pmNeedReset = false;
  pmLoadMaze(); pmReset();
  pmPauseUntil = tNow + 1300; pmTick = tNow;
}

void pmEat() {
  char c = maze[pmy][pmx];
  if (c == '.') { maze[pmy][pmx] = ' '; score += 10; pelletsLeft--; }
  else if (c == 'o') {
    maze[pmy][pmx] = ' '; score += 50; pelletsLeft--;
    unsigned long dur = 6500 - (unsigned long)imin(pmLevel - 1, 6) * 600;
    frightUntil = tNow + dur; ghostCombo = 0;
    for (int i = 0; i < 3; i++) if (tNow >= gh[i].release) { gh[i].fright = true; gh[i].dx = -gh[i].dx; gh[i].dy = -gh[i].dy; }
  }
}

void ghostStep(int i) {
  Ghost &g = gh[i];
  g.px = g.x; g.py = g.y;
  if (tNow < g.release) return;
  if (g.fright && (pmTickCount & 1)) return;           // frightened ghosts are slow
  if (!g.fright && (pmTickCount % 6 == 5)) return;     // normal ghosts are a touch slower than Pac-Man
  static const int DX[4] = {1, -1, 0, 0};
  static const int DY[4] = {0, 0, 1, -1};
  int ox[4], oy[4], odx[4], ody[4], n = 0;
  for (int d = 0; d < 4; d++) {
    if ((g.dx || g.dy) && DX[d] == -g.dx && DY[d] == -g.dy) continue;   // no U-turns
    int nx = (g.x + DX[d] + MZW) % MZW, ny = g.y + DY[d];
    if (ny < 0 || ny >= MZH || maze[ny][nx] == '#') continue;
    ox[n] = nx; oy[n] = ny; odx[n] = DX[d]; ody[n] = DY[d]; n++;
  }
  if (n == 0) { ox[0] = (g.x - g.dx + MZW) % MZW; oy[0] = g.y - g.dy; odx[0] = -g.dx; ody[0] = -g.dy; n = 1; }
  int pick = 0;
  if (g.fright || random(100) >= chaseP[i]) pick = random(n);
  else {
    int tx = pmx, ty = pmy;
    if (i == 2) { tx += pmdx * 3; ty += pmdy * 3; }
    long bd = 1000000;
    for (int k = 0; k < n; k++) {
      long dd = abs(ox[k] - tx) + abs(oy[k] - ty);
      if (dd < bd) { bd = dd; pick = k; }
    }
  }
  g.x = ox[pick]; g.y = oy[pick]; g.dx = odx[pick]; g.dy = ody[pick];
}

void pmCollisions() {
  for (int i = 0; i < 3; i++) {
    Ghost &g = gh[i];
    bool same = (g.x == pmx && g.y == pmy);
    bool swap = (g.x == pmpx && g.y == pmpy && g.px == pmx && g.py == pmy);
    if (!(same || swap)) continue;
    if (g.fright) {
      score += 200 << imin(ghostCombo, 3); ghostCombo++;
      burst(4 + pmx * 8 + 4, 18 + pmy * 8 + 4, CYAN, 12);
      pmResetGhost(i, 2500);
    } else {
      burst(4 + pmx * 8 + 4, 18 + pmy * 8 + 4, YELLOW, 22);
      lives--;
      if (lives <= 0) gameEnded = true;
      else { pmNeedReset = true; pmPauseUntil = tNow + 1200; }
      return;
    }
  }
}

void pmNextLevel() {
  pmLevel++; score += 200;
  if (pmInterval > 100) pmInterval -= 8;
  pmLoadMaze(); pmReset(); pmPauseUntil = tNow + 1200; pmTick = tNow;
}

void pmStep() {
  pmTickCount++;
  pmpx = pmx; pmpy = pmy;
  if ((pmwx || pmwy) && pmCan(pmx, pmy, pmwx, pmwy)) { pmdx = pmwx; pmdy = pmwy; }
  if (pmdx || pmdy) {
    pmfx = pmdx; pmfy = pmdy;
    if (pmCan(pmx, pmy, pmdx, pmdy)) { pmx = (pmx + pmdx + MZW) % MZW; pmy += pmdy; pmEat(); }
    else { pmdx = 0; pmdy = 0; }
  }
  if (frightUntil && tNow >= frightUntil) { frightUntil = 0; for (int i = 0; i < 3; i++) gh[i].fright = false; }
  for (int i = 0; i < 3; i++) ghostStep(i);
  pmCollisions();
  if (!gameEnded && !pmNeedReset && pelletsLeft <= 0) pmNextLevel();
}

void updatePacman() {
  if (jx || jy) { pmwx = jx; pmwy = jy; }
  if (tNow < pmPauseUntil) { pmTick = tNow; return; }
  if (pmNeedReset) { pmNeedReset = false; pmReset(); pmPauseUntil = tNow + 800; pmTick = tNow; return; }
  if (tNow - pmTick >= pmInterval) { pmTick = tNow; pmStep(); }
}

float lerpTile(int a, int b, float t) { if (abs(a - b) > 1) return (float)b; return a + (b - a) * t; }

void drawGhostSprite(int cx, int cy, uint16_t col, bool fright, bool flash, int dx, int dy) {
  uint16_t c = fright ? (flash ? WHITE : C565(40, 60, 255)) : col;
  gfx.fillCircle(cx, cy - 1, 3, c);
  gfx.fillRect(cx - 3, cy - 1, 7, 4, c);
  int ph = (tNow / 150) & 1;
  if (ph) { gfx.drawPixel(cx - 2, cy + 2, BLACK); gfx.drawPixel(cx, cy + 2, BLACK); gfx.drawPixel(cx + 2, cy + 2, BLACK); }
  else    { gfx.drawPixel(cx - 1, cy + 2, BLACK); gfx.drawPixel(cx + 1, cy + 2, BLACK); }
  if (fright) {
    uint16_t f = flash ? RED : WHITE;
    gfx.drawPixel(cx - 2, cy - 2, f); gfx.drawPixel(cx + 1, cy - 2, f);
    gfx.drawFastHLine(cx - 2, cy + 1, 5, f);
  } else {
    gfx.fillRect(cx - 2, cy - 2, 2, 2, WHITE);
    gfx.fillRect(cx + 1, cy - 2, 2, 2, WHITE);
    gfx.drawPixel(cx - 2 + (dx > 0 ? 1 : 0), cy - 2 + (dy > 0 ? 1 : 0), BLUE);
    gfx.drawPixel(cx + 1 + (dx > 0 ? 1 : 0), cy - 2 + (dy > 0 ? 1 : 0), BLUE);
  }
}

void drawPacSprite(int cx, int cy, int fx, int fy, bool open) {
  gfx.fillCircle(cx, cy, 3, YELLOW);
  if (open) {
    float a = atan2f((float)fy, (float)fx);
    gfx.fillTriangle(cx, cy,
                     cx + (int)lroundf(4.5f * cosf(a - 0.7f)), cy + (int)lroundf(4.5f * sinf(a - 0.7f)),
                     cx + (int)lroundf(4.5f * cosf(a + 0.7f)), cy + (int)lroundf(4.5f * sinf(a + 0.7f)), BLACK);
  }
}

void drawPacman() {
  const int ox = 4, oy = 18;
  gfx.fillScreen(BLACK);
  uint16_t wallF = C565(5, 5, 38), wallE = C565(50, 90, 255), dotC = C565(255, 214, 170);
  for (int y = 0; y < MZH; y++) for (int x = 0; x < MZW; x++) {
    char c = maze[y][x]; int px = ox + x * 8, py = oy + y * 8;
    if (c == '#') {
      gfx.fillRect(px, py, 8, 8, wallF);
      if (!pmWall(x, y - 1)) gfx.drawFastHLine(px, py, 8, wallE);
      if (!pmWall(x, y + 1)) gfx.drawFastHLine(px, py + 7, 8, wallE);
      if (!pmWall(x - 1, y)) gfx.drawFastVLine(px, py, 8, wallE);
      if (!pmWall(x + 1, y)) gfx.drawFastVLine(px + 7, py, 8, wallE);
    } else if (c == '.') gfx.fillRect(px + 3, py + 3, 2, 2, dotC);
    else if (c == 'o') {
      if ((tNow / 250) & 1) gfx.fillCircle(px + 4, py + 4, 3, WHITE);
      else gfx.fillCircle(px + 4, py + 4, 2, dotC);
    }
  }
  float t = (tNow < pmPauseUntil) ? 0.0f : fclamp((tNow - pmTick) / (float)pmInterval, 0.0f, 1.0f);
  bool flash = frightUntil && (frightUntil - tNow < 1600) && ((tNow / 160) & 1);
  const uint16_t gcol[3] = {RED, C565(255, 140, 200), CYAN};
  for (int i = 0; i < 3; i++) {
    Ghost &g = gh[i];
    int cx = ox + (int)(lerpTile(g.px, g.x, t) * 8 + 4.5f);
    int cy = oy + (int)(lerpTile(g.py, g.y, t) * 8 + 4.5f);
    if (tNow < g.release) cy += ((tNow / 200) & 1) ? 1 : -1;
    drawGhostSprite(cx, cy, gcol[i], g.fright, flash, g.dx, g.dy);
  }
  if (!pmNeedReset) {
    int cx = ox + (int)(lerpTile(pmpx, pmx, t) * 8 + 4.5f);
    int cy = oy + (int)(lerpTile(pmpy, pmy, t) * 8 + 4.5f);
    drawPacSprite(cx, cy, pmfx, pmfy, ((tNow / 110) & 1) != 0);
  }
  if (tNow < pmPauseUntil && !pmNeedReset) {
    gfx.fillRect(38, 88, 52, 12, BLACK);
    centerText("READY!", 90, YELLOW, 1);
  }
  drawParticles();
  hud("PAC-MAN", YELLOW);
  for (int i = 0; i < lives; i++) drawPacSprite(9 + i * 11, 149, 1, 0, true);
  char b[12]; snprintf(b, sizeof b, "LV %d", pmLevel);
  gfx.setTextSize(1); gfx.setTextColor(GRAY);
  gfx.setCursor(124 - (int)strlen(b) * 6, 146); gfx.print(b);
}

// =====================================================================
//                               SNAKE
// =====================================================================
const int SN_W = 16, SN_H = 18, SN_OY = 15;
int sx[SN_W * SN_H], sy[SN_W * SN_H];
int snLen, sdx, sdy, ndx, ndy, foodX, foodY;
unsigned long snTick;

void snPlaceFood() {
  for (int tries = 0; tries < 200; tries++) {
    int fx = random(SN_W), fy = random(SN_H); bool ok = true;
    for (int i = 0; i < snLen; i++) if (sx[i] == fx && sy[i] == fy) { ok = false; break; }
    if (ok) { foodX = fx; foodY = fy; return; }
  }
}
void initSnake() {
  snLen = 4; sdx = ndx = 1; sdy = ndy = 0;
  for (int i = 0; i < snLen; i++) { sx[i] = 7 - i; sy[i] = 9; }
  snPlaceFood(); snTick = tNow;
}
void updateSnake() {
  if (jx != 0 && sdx == 0) { ndx = jx; ndy = 0; }
  else if (jy != 0 && sdy == 0) { ndx = 0; ndy = jy; }
  unsigned long interval = (unsigned long)imax(70, 175 - (snLen - 4) * 3);
  if (tNow - snTick < interval) return;
  snTick = tNow;
  sdx = ndx; sdy = ndy;
  int nx = sx[0] + sdx, ny = sy[0] + sdy;
  bool ate = (nx == foodX && ny == foodY);
  bool dead = (nx < 0 || nx >= SN_W || ny < 0 || ny >= SN_H);
  if (!dead) {
    int lim = ate ? snLen : snLen - 1;
    for (int i = 0; i < lim; i++) if (sx[i] == nx && sy[i] == ny) { dead = true; break; }
  }
  if (dead) { burst(sx[0] * 8 + 4, SN_OY + sy[0] * 8 + 4, GREEN, 20); gameEnded = true; return; }
  if (ate) snLen++;
  for (int i = snLen - 1; i > 0; i--) { sx[i] = sx[i - 1]; sy[i] = sy[i - 1]; }
  sx[0] = nx; sy[0] = ny;
  if (ate) {
    score += 10;
    burst(nx * 8 + 4, SN_OY + ny * 8 + 4, RED, 10);
    if (snLen >= SN_W * SN_H) gameEnded = true; else snPlaceFood();
  }
}
void drawSnake() {
  gfx.fillScreen(BLACK);
  for (int y = 0; y < SN_H; y++) for (int x = 0; x < SN_W; x++)
    gfx.fillRect(x * 8, SN_OY + y * 8, 8, 8, ((x + y) & 1) ? C565(14, 36, 18) : C565(10, 28, 14));
  gfx.drawRect(0, 14, SCR_W, 146, C565(40, 160, 70));
  // food (apple)
  int fx = foodX * 8 + 4, fy = SN_OY + foodY * 8 + 4;
  int pr = ((tNow / 300) & 1) ? 3 : 2;
  gfx.fillCircle(fx, fy + 1, pr + 1, RED);
  gfx.drawPixel(fx - 1, fy, C565(255, 190, 190));
  gfx.drawPixel(fx + 1, fy - 3, GREEN); gfx.drawPixel(fx + 2, fy - 3, GREEN);
  // snake
  for (int i = snLen - 1; i >= 0; i--) {
    uint16_t c = (i == 0) ? C565(190, 255, 60) : mix565(C565(80, 235, 90), C565(10, 110, 40), (uint8_t)(i * 200 / snLen));
    gfx.fillRoundRect(sx[i] * 8 + 1, SN_OY + sy[i] * 8 + 1, 6, 6, 2, c);
    if (i > 0 && i < snLen) {   // bridge between segments so the body looks continuous
      int mx = (sx[i] + sx[i - 1]) * 4 + 3, my = SN_OY + (sy[i] + sy[i - 1]) * 4 + 3;
      if (sx[i] != sx[i - 1] || sy[i] != sy[i - 1]) gfx.fillRect(mx, my, 2, 2, c);
    }
  }
  int hx = sx[0] * 8, hy = SN_OY + sy[0] * 8;
  if (sdx != 0) { int ex = hx + (sdx > 0 ? 5 : 2); gfx.drawPixel(ex, hy + 2, BLACK); gfx.drawPixel(ex, hy + 5, BLACK); }
  else          { int ey = hy + (sdy > 0 ? 5 : 2); gfx.drawPixel(hx + 2, ey, BLACK); gfx.drawPixel(hx + 5, ey, BLACK); }
  drawParticles();
  hud("SNAKE", GREEN);
}

// =====================================================================
//                          SPACE INVADERS
// =====================================================================
const char* const SPR_A[8] = {"..#.....#..", "...#...#...", "..#######..", ".##.###.##.", "###########", "#.#######.#", "#.#.....#.#", "...##.##..."};
const char* const SPR_B[8] = {"..#.....#..", "#..#...#..#", "#.#######.#", "###.###.###", "###########", ".#########.", "..#.....#..", ".#.......#."};
const char* const SPR_SHIP[7] = {"......#......", ".....###.....", ".....###.....", ".###########.", "#############", "#############", "#############"};
const int INV_R = 4, INV_C = 6, PY = 140;
bool invAlive[INV_R][INV_C];
bool shield[3][3][5];
const int shieldX[3] = {12, 54, 96};
int invX, invY, invDir, invFrame, invCount, invWave, plX;
unsigned long invStepAt, nextFire, nextUfo, invUntil;
struct Bullet { int x, y; bool on; };
Bullet pb, eb[3];
bool ufoOn; float ufoX; int ufoDir;

bool rectHit(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh) {
  return ax < bx + bw && ax + aw > bx && ay < by + bh && ay + ah > by;
}
bool hitShield(int rx, int ry, int rw, int rh) {
  for (int s = 0; s < 3; s++) for (int r = 0; r < 3; r++) for (int c = 0; c < 5; c++) if (shield[s][r][c]) {
    if (rectHit(rx, ry, rw, rh, shieldX[s] + c * 4, 118 + r * 4, 4, 4)) { shield[s][r][c] = false; return true; }
  }
  return false;
}
void invSpawnWave() {
  for (int r = 0; r < INV_R; r++) for (int c = 0; c < INV_C; c++) invAlive[r][c] = true;
  invCount = INV_R * INV_C; invX = 10; invY = imin(24 + (invWave - 1) * 6, 46);
  invDir = 1; invFrame = 0; invStepAt = tNow + 500; nextFire = tNow + 1200;
  pb.on = false; for (int i = 0; i < 3; i++) eb[i].on = false;
}
void initInvaders() {
  invWave = 1; plX = 57; invUntil = 0; ufoOn = false; nextUfo = tNow + 9000;
  for (int s = 0; s < 3; s++) for (int r = 0; r < 3; r++) for (int c = 0; c < 5; c++)
    shield[s][r][c] = !((r == 2 && c >= 1 && c <= 3) || (r == 0 && (c == 0 || c == 4)));
  invSpawnWave();
}
void invStep() {
  int minC = INV_C, maxC = -1;
  for (int r = 0; r < INV_R; r++) for (int c = 0; c < INV_C; c++) if (invAlive[r][c]) { if (c < minC) minC = c; if (c > maxC) maxC = c; }
  if (maxC < 0) return;
  int left = invX + minC * 17, right = invX + maxC * 17 + 11;
  bool edge = (invDir > 0 && right + 2 > 125) || (invDir < 0 && left - 2 < 3);
  if (edge) { invDir = -invDir; invY += 6; } else invX += invDir * 2;
  invFrame ^= 1;
  for (int r = INV_R - 1; r >= 0; r--) {
    bool any = false;
    for (int c = 0; c < INV_C; c++) if (invAlive[r][c]) any = true;
    if (any) { if (invY + r * 12 + 8 >= 114) gameEnded = true; break; }
  }
}
void invPlayerHit() {
  lives--;
  burst(plX + 6, PY + 3, CYAN, 18);
  invUntil = tNow + 1600;
  for (int i = 0; i < 3; i++) eb[i].on = false;
  if (lives <= 0) gameEnded = true;
}
void updateInvaders() {
  plX = iclamp(plX + jx * 3, 2, 113);
  if (clickEvent && !pb.on) { pb.on = true; pb.x = plX + 6; pb.y = PY - 4; }
  if (pb.on) {
    pb.y -= 6;
    if (pb.y < 15) pb.on = false;
    else if (hitShield(pb.x, pb.y, 2, 5)) pb.on = false;
    else if (ufoOn && rectHit(pb.x, pb.y, 2, 5, (int)ufoX, 18, 16, 7)) {
      score += 50 * (1 + random(3)); burst(ufoX + 8, 21, RED, 16); ufoOn = false; pb.on = false;
      nextUfo = tNow + random(12000, 22000);
    } else {
      for (int r = 0; r < INV_R && pb.on; r++) for (int c = 0; c < INV_C; c++) if (invAlive[r][c]) {
        int ex = invX + c * 17, ey = invY + r * 12;
        if (rectHit(pb.x, pb.y, 2, 5, ex, ey, 11, 8)) {
          invAlive[r][c] = false; invCount--; pb.on = false;
          score += (r == 0) ? 30 : (r == 1 ? 20 : 10);
          const uint16_t rc[4] = {C565(255, 80, 200), C565(80, 200, 255), GREEN, YELLOW};
          burst(ex + 5, ey + 4, rc[r], 9);
          break;
        }
      }
    }
  }
  if (tNow >= invStepAt) {
    invStep();
    invStepAt = tNow + (unsigned long)imax(30, 30 + invCount * 15 - (invWave - 1) * 8);
  }
  if (tNow >= nextFire && invCount > 0) {
    nextFire = tNow + random(450, 1100);
    int c0 = random(INV_C);
    for (int k = 0; k < INV_C; k++) {
      int c = (c0 + k) % INV_C, lr = -1;
      for (int r = INV_R - 1; r >= 0; r--) if (invAlive[r][c]) { lr = r; break; }
      if (lr < 0) continue;
      for (int i = 0; i < 3; i++) if (!eb[i].on) { eb[i].on = true; eb[i].x = invX + c * 17 + 5; eb[i].y = invY + lr * 12 + 8; break; }
      break;
    }
  }
  for (int i = 0; i < 3; i++) if (eb[i].on) {
    eb[i].y += 3;
    if (eb[i].y > 148) eb[i].on = false;
    else if (hitShield(eb[i].x, eb[i].y, 2, 5)) eb[i].on = false;
    else if (tNow >= invUntil && rectHit(eb[i].x, eb[i].y, 2, 5, plX, PY, 13, 7)) { eb[i].on = false; invPlayerHit(); }
  }
  if (!ufoOn && tNow >= nextUfo) { ufoOn = true; ufoDir = random(2) ? 1 : -1; ufoX = ufoDir > 0 ? -16 : 128; }
  if (ufoOn) {
    ufoX += ufoDir * 1.5f;
    if (ufoX < -18 || ufoX > 130) { ufoOn = false; nextUfo = tNow + random(12000, 22000); }
  }
  if (!gameEnded && invCount == 0) { score += 100; invWave++; invSpawnWave(); }
}
void drawInvaders() {
  gfx.fillScreen(C565(2, 2, 14));
  drawStars(true);
  const uint16_t rc[4] = {C565(255, 80, 200), C565(80, 200, 255), GREEN, YELLOW};
  for (int r = 0; r < INV_R; r++) for (int c = 0; c < INV_C; c++) if (invAlive[r][c])
    drawSprite(invFrame ? SPR_B : SPR_A, 11, 8, invX + c * 17, invY + r * 12, rc[r]);
  if (ufoOn) {
    int ux = (int)ufoX;
    gfx.fillRoundRect(ux + 4, 17, 8, 4, 2, C565(255, 110, 110));
    gfx.fillRoundRect(ux, 20, 16, 5, 2, RED);
    gfx.drawPixel(ux + 3, 22, YELLOW); gfx.drawPixel(ux + 8, 22, YELLOW); gfx.drawPixel(ux + 12, 22, YELLOW);
  }
  for (int s = 0; s < 3; s++) for (int r = 0; r < 3; r++) for (int c = 0; c < 5; c++) if (shield[s][r][c]) {
    gfx.fillRect(shieldX[s] + c * 4, 118 + r * 4, 4, 4, C565(40, 220, 70));
    gfx.drawPixel(shieldX[s] + c * 4, 118 + r * 4, C565(150, 255, 160));
  }
  if (pb.on) gfx.fillRect(pb.x, pb.y, 2, 5, YELLOW);
  for (int i = 0; i < 3; i++) if (eb[i].on) {
    gfx.drawFastVLine(eb[i].x, eb[i].y, 5, RED);
    gfx.drawFastVLine(eb[i].x + 1, eb[i].y + 1, 4, ORANGE);
  }
  if (!(tNow < invUntil && ((tNow / 100) & 1))) drawSprite(SPR_SHIP, 13, 7, plX, PY, CYAN);
  gfx.drawFastHLine(0, 150, SCR_W, C565(30, 160, 60));
  for (int i = 0; i < lives; i++) { gfx.fillRect(4 + i * 12, 155, 9, 3, CYAN); gfx.fillRect(7 + i * 12, 153, 3, 2, CYAN); }
  char b[12]; snprintf(b, sizeof b, "WAVE %d", invWave);
  gfx.setTextSize(1); gfx.setTextColor(GRAY);
  gfx.setCursor(124 - (int)strlen(b) * 6, 152); gfx.print(b);
  drawParticles();
  hud("INVADERS", CYAN);
}

// =====================================================================
//                               TETRIS
// =====================================================================
const uint8_t tShape[7][4][4] = {
  {{0,0,0,0},{1,1,1,1},{0,0,0,0},{0,0,0,0}},   // I
  {{1,0,0,0},{1,1,1,0},{0,0,0,0},{0,0,0,0}},   // J
  {{0,0,1,0},{1,1,1,0},{0,0,0,0},{0,0,0,0}},   // L
  {{0,1,1,0},{0,1,1,0},{0,0,0,0},{0,0,0,0}},   // O
  {{0,1,1,0},{1,1,0,0},{0,0,0,0},{0,0,0,0}},   // S
  {{0,1,0,0},{1,1,1,0},{0,0,0,0},{0,0,0,0}},   // T
  {{1,1,0,0},{0,1,1,0},{0,0,0,0},{0,0,0,0}}    // Z
};
const uint16_t tColors[7] = {CYAN, C565(60, 90, 255), ORANGE, YELLOW, GREEN, MAGENTA, RED};
const int TX0 = 4, TY0 = 15;
uint8_t tBoard[10][18];
uint8_t piece[4][4];
int ptx, pty, pid, nextId, tLines, tLevel;
unsigned long tDropAt, tClearUntil;
bool tClearing, rowFull[18];
Rep repL, repD;

unsigned long tInterval() { return (unsigned long)imax(90, 650 - (tLevel - 1) * 55); }
bool tValid(int nx, int ny, const uint8_t p[4][4]) {
  for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) if (p[y][x]) {
    int bx = nx + x, by = ny + y;
    if (bx < 0 || bx >= 10 || by >= 18) return false;
    if (by >= 0 && tBoard[bx][by]) return false;
  }
  return true;
}
void tNewPiece() {
  pid = nextId; nextId = random(7);
  memcpy(piece, tShape[pid], sizeof(piece));
  ptx = 3; pty = 0;
  tDropAt = tNow + tInterval();
  if (!tValid(ptx, pty, piece)) gameEnded = true;
}
void initTetris() {
  memset(tBoard, 0, sizeof(tBoard));
  tLines = 0; tLevel = 1; tClearing = false;
  repL.last = 0; repD.last = 0;
  nextId = random(7);
  tNewPiece();
}
void tRotate() {
  if (pid == 3) return;
  int n = (pid == 0) ? 4 : 3;
  uint8_t r[4][4]; memset(r, 0, sizeof(r));
  for (int y = 0; y < n; y++) for (int x = 0; x < n; x++) r[x][n - 1 - y] = piece[y][x];
  const int kicks[5] = {0, -1, 1, -2, 2};
  for (int k = 0; k < 5; k++) if (tValid(ptx + kicks[k], pty, r)) { memcpy(piece, r, sizeof(piece)); ptx += kicks[k]; return; }
}
void tLock() {
  for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) if (piece[y][x] && pty + y >= 0) tBoard[ptx + x][pty + y] = pid + 1;
  int n = 0;
  for (int y = 0; y < 18; y++) {
    bool full = true;
    for (int x = 0; x < 10; x++) if (!tBoard[x][y]) { full = false; break; }
    rowFull[y] = full; if (full) n++;
  }
  if (n > 0) {
    tClearing = true; tClearUntil = tNow + 240;
    for (int y = 0; y < 18; y++) if (rowFull[y]) burst(TX0 + 40, TY0 + y * 8 + 4, WHITE, 3);
  } else tNewPiece();
}
void tFinishClear() {
  int n = 0, dst = 17;
  for (int y = 17; y >= 0; y--) {
    if (rowFull[y]) { n++; continue; }
    if (dst != y) for (int x = 0; x < 10; x++) tBoard[x][dst] = tBoard[x][y];
    dst--;
  }
  for (int y = dst; y >= 0; y--) for (int x = 0; x < 10; x++) tBoard[x][y] = 0;
  const int pts[5] = {0, 100, 300, 500, 800};
  score += pts[imin(n, 4)] * tLevel;
  tLines += n; tLevel = 1 + tLines / 10;
  tClearing = false;
  tNewPiece();
}
void updateTetris() {
  if (tClearing) { if (tNow >= tClearUntil) tFinishClear(); return; }
  if (repStep(repL.last, repL.next, jx, 170, 85)) { if (tValid(ptx + jx, pty, piece)) ptx += jx; }
  if (clickEvent) tRotate();
  if (upEdge) {
    int gy = pty; while (tValid(ptx, gy + 1, piece)) gy++;
    score += 2 * (gy - pty); pty = gy; tLock(); return;
  }
  if (repStep(repD.last, repD.next, jy > 0 ? 1 : 0, 140, 55)) {
    if (tValid(ptx, pty + 1, piece)) { pty++; score++; tDropAt = tNow + tInterval(); }
    else { tLock(); return; }
  }
  if (tNow >= tDropAt) {
    tDropAt = tNow + tInterval();
    if (tValid(ptx, pty + 1, piece)) pty++; else { tLock(); return; }
  }
}
void tCell(int x, int y, uint16_t c) {
  uint16_t lt = mix565(c, WHITE, 120), dk = mix565(c, BLACK, 130);
  gfx.fillRect(x, y, 8, 8, c);
  gfx.drawFastHLine(x, y, 8, lt); gfx.drawFastVLine(x, y, 8, lt);
  gfx.drawFastHLine(x, y + 7, 8, dk); gfx.drawFastVLine(x + 7, y, 8, dk);
}
void drawTetris() {
  gfx.fillScreen(C565(4, 4, 16));
  gfx.fillRect(TX0, TY0, 80, 144, C565(8, 8, 24));
  for (int i = 1; i < 10; i++) gfx.drawFastVLine(TX0 + i * 8, TY0, 144, C565(16, 16, 38));
  for (int j = 1; j < 18; j++) gfx.drawFastHLine(TX0, TY0 + j * 8, 80, C565(16, 16, 38));
  for (int y = 0; y < 18; y++) for (int x = 0; x < 10; x++) if (tBoard[x][y]) {
    uint16_t c = tColors[tBoard[x][y] - 1];
    if (tClearing && rowFull[y]) c = ((tNow / 60) & 1) ? WHITE : mix565(c, WHITE, 160);
    tCell(TX0 + x * 8, TY0 + y * 8, c);
  }
  if (!tClearing) {
    int gy = pty; while (tValid(ptx, gy + 1, piece)) gy++;
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) if (piece[y][x] && gy + y >= 0)
      gfx.drawRect(TX0 + (ptx + x) * 8, TY0 + (gy + y) * 8, 8, 8, mix565(tColors[pid], BLACK, 110));
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) if (piece[y][x] && pty + y >= 0)
      tCell(TX0 + (ptx + x) * 8, TY0 + (pty + y) * 8, tColors[pid]);
  }
  gfx.drawRect(TX0 - 1, 14, 82, 146, C565(90, 90, 150));
  // side panel
  gfx.setTextSize(1);
  gfx.setTextColor(GRAY); gfx.setCursor(92, 18); gfx.print("NEXT");
  gfx.drawRoundRect(90, 28, 36, 28, 4, C565(90, 90, 150));
  int minx = 4, maxx = -1, miny = 4, maxy = -1;
  for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) if (tShape[nextId][y][x]) {
    if (x < minx) minx = x; if (x > maxx) maxx = x; if (y < miny) miny = y; if (y > maxy) maxy = y;
  }
  int pw = (maxx - minx + 1) * 6, ph = (maxy - miny + 1) * 6;
  int nx0 = 90 + (36 - pw) / 2, ny0 = 28 + (28 - ph) / 2;
  for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) if (tShape[nextId][y][x]) {
    int px = nx0 + (x - minx) * 6, py = ny0 + (y - miny) * 6;
    gfx.fillRect(px, py, 6, 6, tColors[nextId]);
    gfx.drawFastHLine(px, py, 6, mix565(tColors[nextId], WHITE, 120));
    gfx.drawFastHLine(px, py + 5, 6, mix565(tColors[nextId], BLACK, 130));
  }
  char b[16];
  gfx.setTextColor(GRAY); gfx.setCursor(90, 66); gfx.print("LINES");
  gfx.setTextColor(WHITE); snprintf(b, sizeof b, "%d", tLines); gfx.setCursor(90, 76); gfx.print(b);
  gfx.setTextColor(GRAY); gfx.setCursor(90, 94); gfx.print("LEVEL");
  gfx.setTextColor(WHITE); snprintf(b, sizeof b, "%d", tLevel); gfx.setCursor(90, 104); gfx.print(b);
  gfx.setTextColor(GRAY); gfx.setCursor(90, 122); gfx.print("BEST");
  gfx.setTextColor(YELLOW); snprintf(b, sizeof b, "%d", (int)imax((int)best[3], score)); gfx.setCursor(90, 132); gfx.print(b);
  drawParticles();
  hud("TETRIS", MAGENTA);
}

// =====================================================================
//                            FLAPPY BIRD
// =====================================================================
float fbY, fbV, pipeXf[2], groundOff, cloudX[3];
int pipeGap[2], fState;
bool pipePassed[2];
const int CLOUDY[3] = {38, 62, 92};
unsigned long wingAt;

void initFlappy() {
  fbY = 80; fbV = 0; fState = 0; groundOff = 0; wingAt = 0;
  pipeXf[0] = 150; pipeXf[1] = 226;
  for (int i = 0; i < 2; i++) { pipeGap[i] = random(52, 112); pipePassed[i] = false; }
  for (int i = 0; i < 3; i++) cloudX[i] = random(0, 160);
}
void flDie() {
  burst(32, fbY, YELLOW, 18);
  gameEnded = true;
}
void updateFlappy() {
  groundOff += 60.0f * dt;
  for (int i = 0; i < 3; i++) { cloudX[i] -= (8.0f + i * 4.0f) * dt; if (cloudX[i] < -30) cloudX[i] = 150; }
  if (fState == 0) {
    fbY = 80 + sinf(tNow / 220.0f) * 4.0f;
    if (clickEvent) { fState = 1; fbV = -150; wingAt = tNow; }
    return;
  }
  if (clickEvent) { fbV = -150; wingAt = tNow; }
  fbV += 520.0f * dt; if (fbV > 260) fbV = 260;
  fbY += fbV * dt;
  if (fbY < 6) { fbY = 6; if (fbV < 0) fbV = 0; }
  for (int i = 0; i < 2; i++) {
    pipeXf[i] -= 60.0f * dt;
    if (pipeXf[i] < -26) {
      pipeXf[i] = (pipeXf[0] > pipeXf[1] ? pipeXf[0] : pipeXf[1]) + 76;
      pipeGap[i] = random(52, 112); pipePassed[i] = false;
    }
    if (!pipePassed[i] && pipeXf[i] + 20 < 27) { pipePassed[i] = true; score++; }
    if (32 + 4 > pipeXf[i] - 2 && 32 - 4 < pipeXf[i] + 22) {
      if (fbY - 4 < pipeGap[i] - 23 || fbY + 4 > pipeGap[i] + 23) { flDie(); return; }
    }
  }
  if (fbY + 5 >= 148) { fbY = 143; flDie(); }
}
void drawFlappy() {
  for (int i = 0; i < 10; i++)
    gfx.fillRect(0, i * 16, SCR_W, 16, mix565(C565(90, 170, 255), C565(200, 238, 255), (uint8_t)(i * 255 / 9)));
  for (int i = 0; i < 3; i++) {
    int cx = (int)cloudX[i], cy = CLOUDY[i];
    gfx.fillCircle(cx, cy, 5, WHITE); gfx.fillCircle(cx + 7, cy - 2, 6, WHITE);
    gfx.fillCircle(cx + 14, cy, 5, WHITE); gfx.fillRect(cx, cy, 15, 5, WHITE);
  }
  const uint16_t PIPE = C565(80, 200, 60), PIPE_L = C565(160, 240, 120), PIPE_D = C565(40, 130, 30), PIPE_O = C565(20, 70, 15);
  for (int i = 0; i < 2; i++) {
    int px = (int)pipeXf[i], gt = pipeGap[i] - 23, gb = pipeGap[i] + 23;
    gfx.fillRect(px, 0, 20, gt - 8, PIPE);
    gfx.fillRect(px + 2, 0, 3, gt - 8, PIPE_L); gfx.fillRect(px + 14, 0, 5, gt - 8, PIPE_D);
    gfx.drawRect(px, -1, 20, gt - 7, PIPE_O);
    gfx.fillRect(px - 2, gt - 8, 24, 8, PIPE); gfx.fillRect(px, gt - 8, 3, 8, PIPE_L); gfx.fillRect(px + 17, gt - 8, 5, 8, PIPE_D);
    gfx.drawRect(px - 2, gt - 8, 24, 8, PIPE_O);
    gfx.fillRect(px, gb + 8, 20, 148 - (gb + 8), PIPE);
    gfx.fillRect(px + 2, gb + 8, 3, 148 - (gb + 8), PIPE_L); gfx.fillRect(px + 14, gb + 8, 5, 148 - (gb + 8), PIPE_D);
    gfx.drawRect(px, gb + 8, 20, 148 - (gb + 8), PIPE_O);
    gfx.fillRect(px - 2, gb, 24, 8, PIPE); gfx.fillRect(px, gb, 3, 8, PIPE_L); gfx.fillRect(px + 17, gb, 5, 8, PIPE_D);
    gfx.drawRect(px - 2, gb, 24, 8, PIPE_O);
  }
  gfx.fillRect(0, 148, SCR_W, 12, C565(222, 200, 120));
  gfx.fillRect(0, 148, SCR_W, 3, C565(90, 200, 60));
  gfx.drawFastHLine(0, 151, SCR_W, C565(40, 120, 30));
  int off = (int)groundOff % 16;
  for (int x = -off; x < SCR_W; x += 16) gfx.fillRect(x, 154, 8, 2, C565(200, 170, 90));
  // bird
  int by = (int)fbY;
  gfx.fillCircle(32, by, 5, YELLOW);
  gfx.drawCircle(32, by, 5, C565(210, 150, 20));
  int wy = (fbV < 0 || (tNow - wingAt < 120)) ? -1 : 2;
  gfx.fillRoundRect(26, by + wy, 6, 3, 1, C565(255, 190, 40));
  gfx.fillCircle(35, by - 2, 2, WHITE); gfx.drawPixel(36, by - 2, BLACK);
  gfx.fillTriangle(37, by, 42, by + 1, 37, by + 3, ORANGE);
  drawParticles();
  char b[12]; snprintf(b, sizeof b, "%d", score);
  outlinedCenter(b, 8, 2, WHITE, BLACK);
  if (fState == 0) {
    outlinedCenter("CLICK TO FLAP", 108, 1, WHITE, BLACK);
    char h[24]; snprintf(h, sizeof h, "BEST %d", (int)best[4]);
    outlinedCenter(h, 122, 1, YELLOW, BLACK);
  }
}

// =====================================================================
//                         GAME FLOW AND MENU
// =====================================================================
void initGame() {
  clearParticles();
  switch (cur) {
    case 0: initPacman(); break;
    case 1: initSnake(); break;
    case 2: initInvaders(); break;
    case 3: initTetris(); break;
    case 4: initFlappy(); break;
  }
}
void startGame(int g) {
  cur = g; screen = PLAYING; score = 0; lives = 3; gameEnded = false; overShown = false;
  initStars(); initGame();
}
void updateGame() {
  switch (cur) {
    case 0: updatePacman(); break;
    case 1: updateSnake(); break;
    case 2: updateInvaders(); break;
    case 3: updateTetris(); break;
    case 4: updateFlappy(); break;
  }
  updateParticles();
}
void drawGame() {
  switch (cur) {
    case 0: drawPacman(); break;
    case 1: drawSnake(); break;
    case 2: drawInvaders(); break;
    case 3: drawTetris(); break;
    case 4: drawFlappy(); break;
  }
}
void endGame() {
  newBest = false;
  if ((uint32_t)score > best[cur]) {
    best[cur] = score; newBest = true;
    char k[8]; snprintf(k, sizeof k, "hs%d", cur);
    prefs.putUInt(k, best[cur]);
  }
  screen = OVER; overShown = false; overAt = tNow + 750;
}
void drawOverlay() {
  uint16_t *buf = cv->getBuffer();
  for (int i = 0; i < SCR_W * SCR_H; i++) buf[i] = (buf[i] >> 1) & 0x7BEF;   // dim the frozen frame
  uint16_t col = gameCols[cur];
  gfx.fillRoundRect(6, 36, 116, 98, 8, BLACK);
  gfx.drawRoundRect(6, 36, 116, 98, 8, col);
  gfx.drawRoundRect(7, 37, 114, 96, 7, mix565(col, BLACK, 140));
  centerText("GAME OVER", 46, RED, 2);
  char b[24];
  snprintf(b, sizeof b, "SCORE  %d", score); centerText(b, 74, WHITE, 1);
  if (newBest) centerText("NEW BEST!", 88, YELLOW, 1);
  else { snprintf(b, sizeof b, "BEST   %d", (int)best[cur]); centerText(b, 88, GRAY, 1); }
  centerText("CLICK: PLAY AGAIN", 106, CYAN, 1);
  centerText("BOOT : MENU", 118, GRAY, 1);
}

// ----- Menu -----
void drawMenuIcon(int i, int x, int y) {          // 14x14 area, drawn on a black tile
  gfx.fillRoundRect(x - 1, y - 1, 16, 16, 3, BLACK);
  switch (i) {
    case 0:
      gfx.fillCircle(x + 7, y + 7, 6, YELLOW);
      gfx.fillTriangle(x + 7, y + 7, x + 14, y + 2, x + 14, y + 12, BLACK);
      break;
    case 1:
      gfx.fillRect(x + 1, y + 2, 4, 4, GREEN); gfx.fillRect(x + 5, y + 2, 4, 4, GREEN);
      gfx.fillRect(x + 5, y + 6, 4, 4, GREEN); gfx.fillRect(x + 5, y + 10, 4, 3, GREEN);
      gfx.fillRect(x + 9, y + 10, 4, 3, YELLOW);
      break;
    case 2:
      drawSprite(SPR_A, 11, 8, x + 1, y + 3, CYAN);
      break;
    case 3:
      gfx.fillRect(x + 5, y + 2, 4, 4, MAGENTA);
      gfx.fillRect(x + 1, y + 6, 4, 4, MAGENTA); gfx.fillRect(x + 5, y + 6, 4, 4, MAGENTA); gfx.fillRect(x + 9, y + 6, 4, 4, MAGENTA);
      break;
    case 4:
      gfx.fillCircle(x + 6, y + 7, 5, YELLOW);
      gfx.fillCircle(x + 8, y + 5, 1, BLACK);
      gfx.fillTriangle(x + 10, y + 7, x + 14, y + 8, x + 10, y + 10, ORANGE);
      break;
  }
}
Rep repMenu;
void updateMenu() {
  if (repStep(repMenu.last, repMenu.next, jy, 350, 170)) sel = (sel + jy + 5) % 5;
  if (clickEvent) startGame(sel);
}
void drawMenu() {
  gfx.fillScreen(C565(2, 4, 28));
  drawStars(true);
  const uint16_t tc[6] = {RED, ORANGE, YELLOW, GREEN, CYAN, MAGENTA};
  gfx.setTextSize(2);
  const char* title = "ARCADE";
  for (int i = 0; i < 6; i++) {
    int x = 28 + i * 12, y = 5 + (int)(sinf(tNow / 250.0f + i * 0.8f) * 1.5f);
    gfx.setTextColor(C565(20, 20, 20)); gfx.setCursor(x + 1, y + 1); gfx.print(title[i]);
    gfx.setTextColor(tc[(i + (tNow / 300)) % 6]); gfx.setCursor(x, y); gfx.print(title[i]);
  }
  centerText("ESP32 GAME HUB", 25, CYAN, 1);
  gfx.drawFastHLine(10, 36, 108, mix565(CYAN, BLACK, 140));
  for (int i = 0; i < 5; i++) {
    int y = 42 + i * 21;
    uint16_t col = gameCols[i];
    bool s = (i == sel);
    if (s) {
      gfx.fillRoundRect(6, y, 116, 19, 5, col);
      if ((tNow / 350) & 1) gfx.drawRoundRect(5, y - 1, 118, 21, 6, WHITE);
    } else {
      gfx.fillRoundRect(6, y, 116, 19, 5, mix565(col, BLACK, 210));
      gfx.drawRoundRect(6, y, 116, 19, 5, mix565(col, BLACK, 140));
    }
    drawMenuIcon(i, 10, y + 2);
    gfx.setTextSize(1);
    gfx.setTextColor(s ? BLACK : WHITE);
    gfx.setCursor(30, y + 6); gfx.print(gameNames[i]);
    char b[12]; snprintf(b, sizeof b, "%d", (int)best[i]);
    gfx.setTextColor(s ? C565(40, 40, 40) : GRAY);
    gfx.setCursor(116 - (int)strlen(b) * 6, y + 6); gfx.print(b);
  }
  centerText("CLICK TO PLAY", 149, GRAY, 1);
}

// ---------- setup / loop ----------
void setup() {
  pinMode(JOY_SW, INPUT_PULLUP);
  pinMode(BOOT_PIN, INPUT_PULLUP);
  analogReadResolution(12);
  Serial.begin(115200);
  randomSeed(esp_random());
  tft.initR(INITR_BLACKTAB);
  tft.setRotation(0);
  tft.fillScreen(BLACK);
  cv = new GFXcanvas16(SCR_W, SCR_H);
  prefs.begin("arcade", false);
  for (int i = 0; i < 5; i++) { char k[8]; snprintf(k, sizeof k, "hs%d", i); best[i] = prefs.getUInt(k, 0); }
  initStars();
  // Splash
  for (int i = 0; i <= 20; i++) {
    tNow = millis();
    gfx.fillScreen(C565(2, 4, 28));
    drawStars(true);
    centerText("ESP32", 40, YELLOW, 3);
    centerText("ARCADE", 70, CYAN, 2);
    gfx.drawRoundRect(24, 110, 80, 8, 3, WHITE);
    gfx.fillRoundRect(26, 112, i * 76 / 20, 4, 1, GREEN);
    pushFrame();
    if (i == 10) {   // calibrate the joystick while it is released
      long ax = 0, ay = 0;
      for (int k = 0; k < 20; k++) { ax += analogRead(JOY_X); ay += analogRead(JOY_Y); delay(3); }
      cenX = ax / 20; cenY = ay / 20;
    }
    delay(35);
  }
  lastFrame = millis();
}

void loop() {
  unsigned long t = millis();
  if (t - lastFrame < FRAME_MS) { delay(1); return; }
  dt = (t - lastFrame) / 1000.0f; if (dt > 0.06f) dt = 0.06f;
  lastFrame = t; tNow = t;
  readControls();
  bool boot = bootPressed();

  if (screen == MENU) {
    updateMenu();
    if (screen == MENU) { drawMenu(); pushFrame(); }
    return;
  }
  if (boot) { screen = MENU; return; }

  if (screen == PLAYING) {
    updateGame();
    drawGame();
    pushFrame();
    if (gameEnded) endGame();
    return;
  }

  // OVER
  if (!overShown) {
    updateParticles();
    drawGame();
    if (tNow >= overAt) { drawOverlay(); overShown = true; }
    pushFrame();
  } else if (clickEvent) {
    startGame(cur);
  }
}
