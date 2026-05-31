// Geometry Dash inspired mini-game for the M5StickC Plus.
// One button (the big front button, BtnA) makes the cube jump over spikes
// that scroll in from the right. Touch a spike and it's game over.
//
// Drawn in landscape (rotation 1) like the FlappyBird example: the physical
// top of the screen is the new left, so the device is held sideways.

#include <M5StickCPlus.h>
#include <EEPROM.h>

// ---------------
// screen (landscape: rotation 1 -> 240 x 135)
// ---------------
#define SCREEN_W 240
#define SCREEN_H 135

// ground (a solid band along the bottom; GROUND_Y is its top edge)
#define GROUND_H 24
#define GROUND_Y (SCREEN_H - GROUND_H)  // 111

// the player "cube"
#define PLAYER_SIZE 14
#define PLAYER_X    36  // fixed horizontal position

// physics, applied once per frame
#define GRAVITY   1.1f
#define JUMP_VEL  -11.5f  // initial upward velocity on a jump

// world scroll speed (pixels per frame, moving left)
#define SCROLL_SPEED 3

// spikes (drawn as triangles standing on the ground)
#define SPIKE_W       16
#define SPIKE_H       16
#define MAX_OBSTACLES 4
#define GAP_MIN       82   // min horizontal gap between obstacles
#define GAP_MAX       150  // max horizontal gap between obstacles

// EEPROM slot for the high score
#define EEPROM_ADDR 0

// ---------------
// colours
// ---------------
const unsigned int COL_BG       = M5.Lcd.color565(18, 18, 28);    // dark night
const unsigned int COL_GROUND   = M5.Lcd.color565(40, 40, 70);    // ground band
const unsigned int COL_GROUNDTOP = M5.Lcd.color565(0, 230, 255);  // neon top line
const unsigned int COL_PLAYER   = M5.Lcd.color565(255, 235, 0);   // bright cube
const unsigned int COL_PLAYEROUT = M5.Lcd.color565(255, 120, 0);  // cube outline
const unsigned int COL_SPIKE    = M5.Lcd.color565(255, 40, 90);   // danger
const unsigned int COL_SPIKEOUT = M5.Lcd.color565(120, 0, 30);    // spike outline

// ---------------
// state
// ---------------
// off-screen frame buffer so animation is flicker free
TFT_eSprite spr = TFT_eSprite(&M5.Lcd);

struct Player {
    float y;       // top of the cube
    float vel_y;   // vertical velocity
    bool on_ground;
} player;

struct Obstacle {
    int x;       // left edge
    int spikes;  // 1 or 2 spikes wide
};
Obstacle obstacles[MAX_OBSTACLES];

int score;
int maxScore;

// ---------------
// helpers
// ---------------
static inline int obstacleWidth(const Obstacle &o) {
    return o.spikes * SPIKE_W;
}

// find the right-most edge currently occupied by any obstacle
static int rightmostEdge() {
    int edge = 0;
    for (int i = 0; i < MAX_OBSTACLES; i++) {
        int e = obstacles[i].x + obstacleWidth(obstacles[i]);
        if (e > edge) edge = e;
    }
    return edge;
}

// place an obstacle just past the current right-most one, with a random gap
static void respawn(Obstacle &o) {
    int gap = random(GAP_MIN, GAP_MAX);
    o.x      = max(rightmostEdge(), SCREEN_W) + gap;
    o.spikes = random(0, 10) < 3 ? 2 : 1;  // ~30% are double spikes
}

// axis-aligned overlap test, with a small forgiving inset on the spike
static bool hitsPlayer(const Obstacle &o) {
    int ox = o.x + 2;
    int ow = obstacleWidth(o) - 4;
    int oy = GROUND_Y - SPIKE_H + 2;
    int oh = SPIKE_H - 2;

    int px = PLAYER_X;
    int py = (int)player.y;
    return px < ox + ow && px + PLAYER_SIZE > ox && py < oy + oh &&
           py + PLAYER_SIZE > oy;
}

// ---------------
// drawing
// ---------------
static void drawGround() {
    spr.fillRect(0, GROUND_Y, SCREEN_W, GROUND_H, COL_GROUND);
    spr.drawFastHLine(0, GROUND_Y, SCREEN_W, COL_GROUNDTOP);
}

static void drawPlayer() {
    int px = PLAYER_X;
    int py = (int)player.y;
    spr.fillRect(px, py, PLAYER_SIZE, PLAYER_SIZE, COL_PLAYER);
    spr.drawRect(px, py, PLAYER_SIZE, PLAYER_SIZE, COL_PLAYEROUT);
    // small inner square so the cube reads like Geometry Dash
    spr.drawRect(px + 4, py + 4, PLAYER_SIZE - 8, PLAYER_SIZE - 8, COL_PLAYEROUT);
}

static void drawObstacle(const Obstacle &o) {
    for (int s = 0; s < o.spikes; s++) {
        int x0 = o.x + s * SPIKE_W;
        int xm = x0 + SPIKE_W / 2;
        int x1 = x0 + SPIKE_W;
        spr.fillTriangle(x0, GROUND_Y, x1, GROUND_Y, xm, GROUND_Y - SPIKE_H,
                         COL_SPIKE);
        spr.drawTriangle(x0, GROUND_Y, x1, GROUND_Y, xm, GROUND_Y - SPIKE_H,
                         COL_SPIKEOUT);
    }
}

static void drawScore() {
    spr.setTextColor(TFT_WHITE);
    spr.setTextSize(1);
    spr.setCursor(4, 4);
    spr.printf("SCORE %d", score);
    spr.setCursor(4, 16);
    spr.setTextColor(COL_GROUNDTOP);
    spr.printf("BEST  %d", maxScore);
}

// ---------------
// reset for a fresh run
// ---------------
static void resetGame() {
    score             = 0;
    player.y          = GROUND_Y - PLAYER_SIZE;
    player.vel_y      = 0;
    player.on_ground  = true;

    // lay obstacles out across (and beyond) the screen with random gaps
    int x = SCREEN_W + 30;
    for (int i = 0; i < MAX_OBSTACLES; i++) {
        obstacles[i].spikes = random(0, 10) < 3 ? 2 : 1;
        obstacles[i].x      = x;
        x += obstacleWidth(obstacles[i]) + random(GAP_MIN, GAP_MAX);
    }
}

// ---------------
// title screen
// ---------------
static void titleScreen() {
    spr.fillSprite(COL_BG);
    drawGround();

    spr.setTextColor(COL_PLAYER);
    spr.setTextSize(3);
    spr.setCursor(40, 28);
    spr.print("GEOMETRY");
    spr.setCursor(70, 56);
    spr.print("DASH");

    spr.setTextColor(TFT_WHITE);
    spr.setTextSize(1);
    spr.setCursor(46, 90);
    spr.print("Press the button to jump");
    spr.pushSprite(0, 0);

    // wait for a button press to start
    while (true) {
        M5.update();
        if (M5.BtnA.wasPressed()) break;
        delay(10);
    }
}

// ---------------
// main play loop; returns when the player crashes
// ---------------
static void playLoop() {
    resetGame();

    while (true) {
        M5.update();

        // jump only when standing on the ground
        if (M5.BtnA.wasPressed() && player.on_ground) {
            player.vel_y     = JUMP_VEL;
            player.on_ground = false;
        }

        // physics
        player.vel_y += GRAVITY;
        player.y += player.vel_y;
        float floor_y = GROUND_Y - PLAYER_SIZE;
        if (player.y >= floor_y) {
            player.y         = floor_y;
            player.vel_y     = 0;
            player.on_ground = true;
        }

        // scroll obstacles; recycle and score when one leaves the screen
        for (int i = 0; i < MAX_OBSTACLES; i++) {
            obstacles[i].x -= SCROLL_SPEED;
            if (obstacles[i].x + obstacleWidth(obstacles[i]) < 0) {
                respawn(obstacles[i]);
                score++;
            }
        }

        // collision
        bool dead = false;
        for (int i = 0; i < MAX_OBSTACLES; i++) {
            if (hitsPlayer(obstacles[i])) {
                dead = true;
                break;
            }
        }

        // render the frame
        spr.fillSprite(COL_BG);
        drawGround();
        for (int i = 0; i < MAX_OBSTACLES; i++) drawObstacle(obstacles[i]);
        drawPlayer();
        drawScore();
        spr.pushSprite(0, 0);

        if (dead) return;

        delay(12);  // ~frame pacing
    }
}

// ---------------
// game over screen
// ---------------
static void gameOver() {
    bool newBest = false;
    if (score > maxScore) {
        maxScore = score;
        EEPROM.writeInt(EEPROM_ADDR, maxScore);
        EEPROM.commit();
        newBest = true;
    }

    spr.fillSprite(COL_BG);
    spr.setTextColor(COL_SPIKE);
    spr.setTextSize(3);
    spr.setCursor(34, 24);
    spr.print("GAME OVER");

    spr.setTextColor(TFT_WHITE);
    spr.setTextSize(2);
    spr.setCursor(70, 60);
    spr.printf("Score %d", score);

    spr.setTextSize(1);
    if (newBest) {
        spr.setTextColor(COL_PLAYER);
        spr.setCursor(80, 84);
        spr.print("NEW BEST!");
    }
    spr.setTextColor(TFT_WHITE);
    spr.setCursor(54, 100);
    spr.print("Press button to retry");
    spr.pushSprite(0, 0);

    // small guard so the crashing press doesn't instantly restart
    delay(400);
    while (true) {
        M5.update();
        if (M5.BtnA.wasPressed()) break;
        delay(10);
    }
}

// ---------------
// Arduino entry points
// ---------------
void setup() {
    M5.begin();
    // landscape: physical top -> left (matches the FlappyBird change)
    M5.Lcd.setRotation(1);
    EEPROM.begin(1000);
    randomSeed(esp_random());
    maxScore = EEPROM.readInt(EEPROM_ADDR);
    if (maxScore < 0 || maxScore > 100000) maxScore = 0;  // sanitise EEPROM

    spr.setColorDepth(16);
    spr.createSprite(SCREEN_W, SCREEN_H);
}

void loop() {
    titleScreen();
    playLoop();
    gameOver();
}
