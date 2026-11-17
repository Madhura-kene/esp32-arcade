/*
  ESP32 ARCADE HUB
  Board: ESP32-WROOM-32 DevKit
  Display: 1.8" ST7735 SPI TFT, 128x160, V1.1
  Library: Adafruit GFX + Adafruit ST7735 and ST7789

  TFT wiring:
    VCC -> 3V3       LED -> 3V3
    GND -> GND       SCK -> GPIO18
    SDA/MOSI -> GPIO23
    CS -> GPIO5      DC/A0 -> GPIO2
    RESET -> GPIO4

  Joystick:
    VRx -> GPIO34    VRy -> GPIO35
    SW  -> GPIO27    VCC -> 3V3, GND -> GND

  BOOT (GPIO0) returns to menu. Do not use EN as an input:
  EN is the ESP32 reset line.

  Menu: joystick up/down to browse, click SW to launch.
  In-game: joystick directions control; click is action.
*/

#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>
#include <math.h>
#include <string.h>

#define TFT_CS   5
#define TFT_DC   2
#define TFT_RST  4
#define JOY_X    34
#define JOY_Y    35
#define JOY_SW   27
#define BOOT_PIN 0

Adafruit_ST7735 tft(TFT_CS, TFT_DC, TFT_RST);

const uint16_t BLACK = ST77XX_BLACK;
const uint16_t WHITE = ST77XX_WHITE;
const uint16_t YELLOW = ST77XX_YELLOW;
const uint16_t CYAN = ST77XX_CYAN;
const uint16_t GREEN = ST77XX_GREEN;
const uint16_t RED = ST77XX_RED;
const uint16_t BLUE = ST77XX_BLUE;
const uint16_t MAGENTA = ST77XX_MAGENTA;
const uint16_t ORANGE = 0xFD20;
const uint16_t NAVY = 0x000F;
const uint16_t DARKBLUE = 0x0015;
const uint16_t PINK = 0xF81F;

enum Screen { MENU, PACMAN, SNAKE, INVADERS, TETRIS, FLAPPY, GAME_OVER };
Screen screen = MENU;
const char* gameNames[5] = {"PAC-MAN", "SNAKE", "SPACE INVADERS", "TETRIS", "FLAPPY BIRD"};
uint16_t gameColors[5] = {YELLOW, GREEN, CYAN, MAGENTA, ORANGE};
int selected = 0;

int joyCenterX = 2048, joyCenterY = 2048;
int joyX = 2048, joyY = 2048;
bool clickNow = false, clickPrev = false, clickEvent = false;
bool bootPrev = false;
unsigned long lastMenuMove = 0, lastGameTick = 0, lastClick = 0;
bool gameEnded = false;
int score = 0, lives = 3;

int axisDir(int v, int center) {
  if (v < center - 900) return -1;
  if (v > center + 900) return 1;
  return 0;
}
int dirX() { return -axisDir(joyY, joyCenterY); }
int dirY() { return -axisDir(joyX, joyCenterX); }

void readControls() {
  joyX = analogRead(JOY_X);
  joyY = analogRead(JOY_Y);
  clickNow = digitalRead(JOY_SW) == LOW;
  clickEvent = clickNow && !clickPrev && millis() - lastClick > 180;
  if (clickEvent) lastClick = millis();
  clickPrev = clickNow;
}

bool bootPressed() {
  bool now = digitalRead(BOOT_PIN) == LOW;
  bool event = now && !bootPrev;
  bootPrev = now;
  return event;
}

void centerText(const String &s, int y, uint16_t color, uint8_t size=1) {
  tft.setTextSize(size);
  tft.setTextColor(color);
  int16_t x1, y1; uint16_t w, h;
  tft.getTextBounds(s, 0, y, &x1, &y1, &w, &h);
  tft.setCursor((128 - w) / 2, y);
  tft.print(s);
}

void drawMenu() {
  tft.fillScreen(NAVY);
  tft.drawRoundRect(3, 3, 122, 154, 8, CYAN);
  centerText("ARCADE HUB", 10, YELLOW, 2);
  centerText("5 GAMES / 1 ESP32", 31, WHITE, 1);
  for (int i=0; i<5; i++) {
    int y = 48 + i*20;
    if (i == selected) {
      tft.fillRoundRect(12, y-2, 104, 17, 4, gameColors[i]);
      tft.setTextColor(BLACK);
    } else {
      tft.drawRoundRect(12, y-2, 104, 17, 4, DARKBLUE);
      tft.setTextColor(WHITE);
    }
    tft.setTextSize(1);
    tft.setCursor(19, y+2);
    tft.print(i+1); tft.print(". "); tft.print(gameNames[i]);
  }
  centerText("JOY UP/DOWN: BROWSE", 151, CYAN, 1);
}

// Game setup functions are defined below.
void initPacman();
void initSnake();
void initInvaders();
void initTetris();
void initFlappy();

void startGame(Screen s) {
  screen = s; score = 0; lives = 3; gameEnded = false;
  lastGameTick = millis();
  if (s == PACMAN) initPacman();
  if (s == SNAKE) initSnake();
  if (s == INVADERS) initInvaders();
  if (s == TETRIS) initTetris();
  if (s == FLAPPY) initFlappy();
}
void returnToMenu() {
  screen = MENU;
  drawMenu();
}

// ---------------- PAC-MAN ----------------
const uint8_t mazeW=15, mazeH=13;
const char mazeTemplate[13][16] = {
 "###############",
 "#.............#",
 "#.###.###.###.#",
 "#.............#",
 "#.###.#.#.###.#",
 "#.....#.#.....#",
 "###.#.....#.###",
 "#...#.....#...#",
 "#.###.#.#.###.#",
 "#.............#",
 "#.###.###.###.#",
 "#.............#",
 "###############"
};
char maze[13][16];
int pmX=1, pmY=1, pmDirX=0, pmDirY=0;
bool mouth=true;
void initPacman() {
  for(int y=0;y<13;y++) strcpy(maze[y], mazeTemplate[y]);
  pmX=1; pmY=1; pmDirX=0; pmDirY=0; mouth=true;
}
void drawPacman() {
  tft.fillScreen(BLACK);
  tft.setTextColor(WHITE); tft.setTextSize(1); tft.setCursor(3,2);
  tft.print("PAC-MAN  SCORE:"); tft.print(score);
  int ox=4, oy=19, cell=8;
  for(int y=0;y<13;y++) for(int x=0;x<15;x++) {
    int px=ox+x*cell, py=oy+y*cell;
    if(maze[y][x]=='#') tft.fillRect(px,py,cell-1,cell-1,BLUE);
    else if(maze[y][x]=='.') tft.fillCircle(px+4,py+4,1,WHITE);
  }
  int px=ox+pmX*cell+4, py=oy+pmY*cell+4;
  tft.fillCircle(px,py,4,YELLOW);
  if(mouth) {
    int dx=pmDirX, dy=pmDirY;
    if(dx==0 && dy==0) dx=1;
    float a=atan2((float)dy,(float)dx);
    tft.fillTriangle(px,py,px+5*cos(a-0.45),py+5*sin(a-0.45),
                     px+5*cos(a+0.45),py+5*sin(a+0.45),BLACK);
  }
  // simple ghost
  tft.fillCircle(ox+13*cell+4,oy+11*cell+3,4,RED);
  tft.fillRect(ox+13*cell,oy+11*cell+3,8,4,RED);
  tft.fillCircle(ox+13*cell+2,oy+11*cell+3,1,WHITE);
  tft.fillCircle(ox+13*cell+6,oy+11*cell+3,1,WHITE);
}
void updatePacman() {
  int dx=dirX(), dy=dirY();
  if(dx || dy) { pmDirX=dx; pmDirY=dy; }
  int nx=pmX+dx, ny=pmY+dy;
  if((dx||dy) && nx>=0 && nx<15 && ny>=0 && ny<13 && maze[ny][nx]!='#') {
    pmX=nx; pmY=ny;
    if(maze[ny][nx]=='.') { maze[ny][nx]=' '; score+=10; }
  }
  mouth=!mouth;
  if(pmX==13 && pmY==11) { gameEnded=true; }
  drawPacman();
}

// ---------------- SNAKE ----------------
const int SW=16, SH=16;
int sx[SW*SH], sy[SW*SH], snakeLen=4, sdx=1, sdy=0, foodX=8, foodY=8;
void placeFood() {
  foodX=random(1,15); foodY=random(1,15);
}
void initSnake() {
  snakeLen=4; sdx=1; sdy=0;
  for(int i=0;i<snakeLen;i++){sx[i]=6-i; sy[i]=8;}
  placeFood();
}
void drawSnake() {
  tft.fillScreen(BLACK);
  tft.setTextColor(WHITE); tft.setCursor(3,3); tft.print("SNAKE  SCORE:"); tft.print(score);
  tft.drawRect(1,17,126,140,BLUE);
  tft.fillCircle(4+foodX*7,20+foodY*7,3,RED);
  for(int i=0;i<snakeLen;i++) tft.fillRoundRect(2+sx[i]*7,18+sy[i]*7,6,6,2,i==0?YELLOW:GREEN);
}
void updateSnake() {
  int dx=dirX(),dy=dirY();
  if(dx && !(dx==-sdx)) {sdx=dx;sdy=0;}
  if(dy && !(dy==-sdy)) {sdy=dy;sdx=0;}
  int nx=sx[0]+sdx, ny=sy[0]+sdy;
  if(nx<0||nx>=SW||ny<0||ny>=SH){gameEnded=true;return;}
  for(int i=0;i<snakeLen;i++) if(sx[i]==nx&&sy[i]==ny){gameEnded=true;return;}
  bool ate=(nx==foodX&&ny==foodY);
  for(int i=snakeLen-1+(ate?1:0);i>0;i--){sx[i]=sx[i-1];sy[i]=sy[i-1];}
  if(ate){snakeLen++;score+=10;placeFood();}
  sx[0]=nx;sy[0]=ny;
  drawSnake();
}

// ---------------- SPACE INVADERS ----------------
struct Enemy {int x,y; bool alive;};
Enemy enemies[10];
int playerX=58, bulletX=-1, bulletY=-1, enemyDir=1;
void initInvaders() {
  playerX=58; bulletX=-1; bulletY=-1; enemyDir=1;
  for(int i=0;i<10;i++){enemies[i]={10+(i%5)*22,28+(i/5)*20,true};}
}
void drawInvaders() {
  tft.fillScreen(BLACK);
  tft.setTextColor(WHITE);tft.setCursor(3,3);tft.print("INVADERS SCORE:");tft.print(score);
  for(int i=0;i<10;i++) if(enemies[i].alive) {
    tft.fillRect(enemies[i].x,enemies[i].y,10,7,GREEN);
    tft.fillRect(enemies[i].x+2,enemies[i].y-3,2,3,GREEN);
    tft.fillRect(enemies[i].x+6,enemies[i].y-3,2,3,GREEN);
  }
  tft.fillTriangle(playerX,148,playerX+8,134,playerX+16,148,CYAN);
  if(bulletY>=15) tft.fillRect(bulletX,bulletY,2,5,YELLOW);
}
void updateInvaders() {
  playerX+=dirX()*4; playerX=constrain(playerX,2,110);
  if(clickEvent && bulletY<0){bulletX=playerX+7;bulletY=132;}
  if(bulletY>=0) {
    bulletY-=7;
    for(int i=0;i<10;i++) if(enemies[i].alive && bulletX>=enemies[i].x-2 && bulletX<=enemies[i].x+12 && bulletY>=enemies[i].y-4 && bulletY<=enemies[i].y+9) {
      enemies[i].alive=false; bulletY=-1; score+=10; break;
    }
    if(bulletY<15) bulletY=-1;
  }
  bool edge=false;
  for(int i=0;i<10;i++) if(enemies[i].alive) {
    enemies[i].x+=enemyDir*2;
    if(enemies[i].x<2||enemies[i].x>116) edge=true;
    if(enemies[i].y>122) gameEnded=true;
  }
  if(edge){enemyDir=-enemyDir;for(int i=0;i<10;i++)enemies[i].y+=5;}
  int alive=0;for(int i=0;i<10;i++)if(enemies[i].alive)alive++;
  if(alive==0) {score+=50;initInvaders();}
  drawInvaders();
}

// ---------------- TETRIS ----------------
const uint8_t shapes[7][4][4] = {
 {{0,0,0,0},{1,1,1,1},{0,0,0,0},{0,0,0,0}},
 {{1,0,0,0},{1,1,1,0},{0,0,0,0},{0,0,0,0}},
 {{0,0,1,0},{1,1,1,0},{0,0,0,0},{0,0,0,0}},
 {{1,1,0,0},{1,1,0,0},{0,0,0,0},{0,0,0,0}},
 {{0,1,1,0},{1,1,0,0},{0,0,0,0},{0,0,0,0}},
 {{0,1,0,0},{1,1,1,0},{0,0,0,0},{0,0,0,0}},
 {{1,1,0,0},{0,1,1,0},{0,0,0,0},{0,0,0,0}}
};
uint8_t board[10][18], piece[4][4]; int tx=3,ty=0,shapeId=0;
uint16_t pieceColor=MAGENTA;
void newPiece() {
  shapeId=random(0,7);
  const uint16_t colors[7]={CYAN,BLUE,ORANGE,YELLOW,GREEN,MAGENTA,RED};
  pieceColor=colors[shapeId];
  for(int y=0;y<4;y++)for(int x=0;x<4;x++)piece[y][x]=shapes[shapeId][y][x];
  tx=3;ty=0;
}
bool validPiece(int nx,int ny,uint8_t p[4][4]) {
  for(int y=0;y<4;y++)for(int x=0;x<4;x++)if(p[y][x]){
    int bx=nx+x,by=ny+y;
    if(bx<0||bx>=10||by>=18)return false;
    if(by>=0&&board[bx][by])return false;
  }
  return true;
}
void initTetris(){memset(board,0,sizeof(board));score=0;newPiece();}
void rotatePiece(){
  uint8_t r[4][4];for(int y=0;y<4;y++)for(int x=0;x<4;x++)r[x][3-y]=piece[y][x];
  if(validPiece(tx,ty,r))memcpy(piece,r,sizeof(piece));
}
void lockPiece(){
  for(int y=0;y<4;y++)for(int x=0;x<4;x++)if(piece[y][x]&&ty+y>=0)board[tx+x][ty+y]=1;
  for(int y=17;y>=0;y--){
    bool full=true;for(int x=0;x<10;x++)if(!board[x][y])full=false;
    if(full){for(int yy=y;yy>0;yy--)for(int x=0;x<10;x++)board[x][yy]=board[x][yy-1];for(int x=0;x<10;x++)board[x][0]=0;score+=100;y++;}
  }
  newPiece();if(!validPiece(tx,ty,piece))gameEnded=true;
}
void drawTetris(){
  tft.fillScreen(BLACK);tft.setTextColor(WHITE);tft.setCursor(3,3);tft.print("TETRIS ");tft.print(score);
  for(int y=0;y<18;y++)for(int x=0;x<10;x++)if(board[x][y])tft.fillRect(14+x*10,20+y*7,9,6,CYAN);
  for(int y=0;y<4;y++)for(int x=0;x<4;x++)if(piece[y][x])tft.fillRect(14+(tx+x)*10,20+(ty+y)*7,9,6,pieceColor);
  tft.drawRect(13,19,102,128,WHITE);
}
void updateTetris(){
  if(dirX()!=0){int nx=tx+dirX();if(validPiece(nx,ty,piece))tx=nx;}
  if(clickEvent)rotatePiece();
  if(dirY()>0){if(validPiece(tx,ty+1,piece))ty++;else lockPiece();}
  else if(millis()-lastGameTick>500){lastGameTick=millis();if(validPiece(tx,ty+1,piece))ty++;else lockPiece();}
  drawTetris();
}

// ---------------- FLAPPY BIRD ----------------
int birdY=75, birdV=0, pipeX=128, gapY=75; bool passedPipe=false;
void initFlappy(){birdY=75;birdV=0;pipeX=128;gapY=random(48,105);passedPipe=false;}
void drawFlappy(){
  tft.fillScreen(0x7DFF);
  tft.fillRect(0,145,128,15,GREEN);
  tft.fillCircle(31,birdY,6,YELLOW);tft.fillCircle(33,birdY-2,1,BLACK);
  tft.fillRect(pipeX,0,18,gapY-24,GREEN);
  tft.fillRect(pipeX,gapY+24,18,145-(gapY+24),GREEN);
  tft.setTextColor(BLACK);tft.setCursor(4,4);tft.print("FLAPPY  ");tft.print(score);
}
void updateFlappy(){
  if(clickEvent)birdV=-5;
  birdV+=1;birdV=constrain(birdV,-6,6);birdY+=birdV;
  pipeX-=3;
  if(pipeX<-18){pipeX=128;gapY=random(45,108);passedPipe=false;}
  if(!passedPipe&&pipeX<31){score+=1;passedPipe=true;}
  if(birdY<5||birdY>139||(pipeX<38&&pipeX>18&&(birdY<gapY-18||birdY>gapY+18)))gameEnded=true;
  drawFlappy();
}

// ---------------- GAME FLOW ----------------
void drawGameOver() {
  tft.fillScreen(BLACK);
  centerText("GAME OVER",45,RED,2);
  centerText("SCORE: "+String(score),78,WHITE,1);
  centerText("CLICK: PLAY AGAIN",104,CYAN,1);
  centerText("BOOT: MENU",120,YELLOW,1);
}
void launchSelected() {
  switch(selected) {
    case 0:startGame(PACMAN);break;
    case 1:startGame(SNAKE);break;
    case 2:startGame(INVADERS);break;
    case 3:startGame(TETRIS);break;
    case 4:startGame(FLAPPY);break;
  }
}

Screen screenForSelection();

void setup() {
  pinMode(JOY_SW,INPUT_PULLUP);
  pinMode(BOOT_PIN,INPUT_PULLUP);
  analogReadResolution(12);
  Serial.begin(115200);
  randomSeed(analogRead(32));
  tft.initR(INITR_BLACKTAB);
  tft.setRotation(0);
  tft.fillScreen(BLACK);
  centerText("ESP32",48,YELLOW,2);
  centerText("ARCADE HUB",76,CYAN,2);
  delay(900);
  // Calibrate joystick at startup while it is released.
  long ax=0,ay=0;
  for(int i=0;i<20;i++){ax+=analogRead(JOY_X);ay+=analogRead(JOY_Y);delay(5);}
  joyCenterX=ax/20;joyCenterY=ay/20;
  drawMenu();
}

void loop() {
  readControls();
  if(screen==MENU) {
    int d=dirY();
    if(millis()-lastMenuMove>220) {
      if(d<0){selected=(selected+4)%5;lastMenuMove=millis();drawMenu();}
      else if(d>0){selected=(selected+1)%5;lastMenuMove=millis();drawMenu();}
    }
    if(clickEvent) launchSelected();
    bootPressed();
    delay(15);
    return;
  }

  if(bootPressed()) {returnToMenu();delay(180);return;}

  if(screen==GAME_OVER) {
    if(clickEvent) startGame(screenForSelection());
    delay(20);return;
  }

  unsigned long interval=150;
  if(screen==PACMAN) interval=180;
  if(screen==SNAKE) interval=170;
  if(screen==INVADERS) interval=45;
  if(screen==TETRIS) interval=100;
  if(screen==FLAPPY) interval=45;

  if(millis()-lastGameTick>=interval) {
    lastGameTick=millis();
    switch(screen) {
      case PACMAN:updatePacman();break;
      case SNAKE:updateSnake();break;
      case INVADERS:updateInvaders();break;
      case TETRIS:updateTetris();break;
      case FLAPPY:updateFlappy();break;
      default:break;
    }
    if(gameEnded){screen=GAME_OVER;drawGameOver();}
  }
  delay(5);
}

// Map current game screen for quick restart from game-over.
Screen screenForSelection() {
  switch(selected) {
    case 0:return PACMAN; case 1:return SNAKE; case 2:return INVADERS;
    case 3:return TETRIS; default:return FLAPPY;
  }
}
