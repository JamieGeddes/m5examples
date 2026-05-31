// Voice-prompted Magic 8-Ball for the M5StickC Plus.
//
// Ask a question out loud: press the big front button (BtnA) and a "recording"
// indicator appears while the device watches the built-in microphone for your
// voice. When you stop talking it asks you to shake the device; a few good
// shakes and it reveals a classic Magic 8-Ball answer, colour-coded by how
// favourable it is (green = yes, amber = unsure, red = no).
//
// Uses M5Unified rather than the M5StickCPlus library because only M5Unified
// exposes the StickC Plus's SPM1423 PDM microphone (M5.Mic). Drawn in landscape
// (rotation 1 -> 240 x 135), double-buffered through an off-screen M5Canvas.

#include <Arduino.h>
#include <M5Unified.h>
#include <math.h>

// ---------------
// screen (landscape: rotation 1 -> 240 x 135)
// ---------------
#define SCREEN_W 240
#define SCREEN_H 135

// ---------------
// microphone / voice-activity detection (tune on device against serial)
// ---------------
#define MIC_RATE          16000  // sample rate, Hz
#define MIC_FRAME         512    // samples per recorded frame (~32 ms)
#define SPEECH_ON_RMS     700.0f // RMS above this means "voice present"
#define SPEECH_OFF_RMS    500.0f // RMS below this counts towards silence
#define SILENCE_MS        2200   // quiet for this long (after speech) -> done
#define MAX_LISTEN_MS     9000   // hard cap so listening never hangs
#define LEVEL_MAX_RMS     3000.0f// RMS that fills the on-screen level meter

// ---------------
// shake detection (accelerometer, values in G)
// ---------------
#define SHAKE_DELTA_G     0.9f   // |accel magnitude - 1g| above this = a jolt
#define SHAKE_DEBOUNCE_MS 150    // ignore further jolts for this long
#define SHAKE_COUNT       4      // jolts needed to trigger the reveal
#define SHAKE_PROMPT_MS   12000  // give up the shake prompt after this

// ---------------
// reveal timing
// ---------------
#define REVEAL_ANIM_MS    700    // triangle-rises animation length
#define REVEAL_HOLD_MS    8000   // auto-return to idle after this (or BtnA)

// ---------------
// colours (RGB565, built at compile time so we don't touch hardware early)
// ---------------
static constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
static constexpr uint16_t COL_BG      = rgb(18, 16, 30);    // dark backdrop
static constexpr uint16_t COL_BALL    = rgb(10, 10, 12);    // the 8-ball body
static constexpr uint16_t COL_BALLEDGE = rgb(60, 60, 80);   // ball rim
static constexpr uint16_t COL_WHITE   = rgb(245, 245, 245);
static constexpr uint16_t COL_NUMBG   = rgb(245, 245, 245); // white "8" disc
static constexpr uint16_t COL_WINDOW  = rgb(22, 32, 130);   // blue answer window
static constexpr uint16_t COL_ACCENT  = rgb(0, 220, 255);   // cyan accents
static constexpr uint16_t COL_REC     = rgb(235, 50, 60);   // recording dot
static constexpr uint16_t COL_DIM     = rgb(120, 120, 140); // hint text
static constexpr uint16_t COL_POS     = rgb(60, 210, 110);  // affirmative
static constexpr uint16_t COL_NEU     = rgb(240, 195, 50);  // non-committal
static constexpr uint16_t COL_NEG     = rgb(235, 70, 70);   // negative

// ---------------
// answers: the classic Magic 8-Ball set of 20
//   10 affirmative (POS), 5 non-committal (NEU), 5 negative (NEG)
// ---------------
enum Cat { POS, NEU, NEG };
struct Answer {
    const char* text;
    Cat cat;
};
const Answer ANSWERS[] = {
    {"It is certain", POS},
    {"It is decidedly so", POS},
    {"Without a doubt", POS},
    {"Yes definitely", POS},
    {"You may rely on it", POS},
    {"As I see it, yes", POS},
    {"Most likely", POS},
    {"Outlook good", POS},
    {"Yes", POS},
    {"Signs point to yes", POS},
    {"Reply hazy, try again", NEU},
    {"Ask again later", NEU},
    {"Better not tell you now", NEU},
    {"Cannot predict now", NEU},
    {"Concentrate and ask again", NEU},
    {"Don't count on it", NEG},
    {"My reply is no", NEG},
    {"My sources say no", NEG},
    {"Outlook not so good", NEG},
    {"Very doubtful", NEG},
};
const int ANSWER_COUNT = sizeof(ANSWERS) / sizeof(ANSWERS[0]);

static uint16_t catColour(Cat c) {
    switch (c) {
        case POS: return COL_POS;
        case NEU: return COL_NEU;
        default:  return COL_NEG;
    }
}

// ---------------
// state machine
// ---------------
enum State { IDLE, LISTENING, PROMPT_SHAKE, REVEAL, REVEAL_HOLD };
State state = IDLE;
uint32_t stateEnteredMs = 0;

// off-screen frame buffer so animation is flicker free
M5Canvas canvas(&M5.Display);

// listening / VAD working state
bool     speechLatched = false;  // have we heard voice this session?
uint32_t lastLoudMs    = 0;      // last frame the mic was above SPEECH_OFF_RMS
float    micLevel      = 0.0f;   // smoothed 0..1 level for the meter

// shake working state
int      shakeCount     = 0;
uint32_t lastShakeMs    = 0;
float    jiggle         = 0.0f;  // current screen-shake amplitude (px)

// chosen answer for the reveal
int      answerIdx      = 0;

static void enterState(State s) {
    state = s;
    stateEnteredMs = millis();
}

// ---------------
// drawing helpers
// ---------------

// the black 8-ball body with a white "8" disc; cx,cy is the centre, r the radius
static void drawBall(int cx, int cy, int r) {
    canvas.fillCircle(cx, cy, r, COL_BALL);
    canvas.drawCircle(cx, cy, r, COL_BALLEDGE);
    // soft highlight, upper-left
    canvas.fillCircle(cx - r / 3, cy - r / 3, r / 6, rgb(40, 40, 55));
    // white disc with the number 8
    int nr = r / 2;
    canvas.fillCircle(cx, cy, nr, COL_NUMBG);
    canvas.setTextColor(COL_BALL, COL_NUMBG);
    canvas.setTextDatum(middle_center);
    canvas.setTextSize(nr >= 16 ? 3 : 2);
    canvas.drawString("8", cx, cy + 1);
}

// word-wrap `text` and draw it centred within `maxW`, vertically centred on cy.
// Uses the current text size; returns nothing.
static void drawWrapped(const char* text, int cx, int cy, int maxW, uint16_t col) {
    canvas.setTextDatum(middle_center);
    canvas.setTextColor(col);

    // split into <=4 lines that each fit maxW
    char lines[4][40];
    int  nLines = 0;
    char word[40];
    char cur[40] = "";

    auto flush = [&]() {
        if (cur[0] && nLines < 4) {
            strncpy(lines[nLines], cur, sizeof(lines[0]) - 1);
            lines[nLines][sizeof(lines[0]) - 1] = '\0';
            nLines++;
            cur[0] = '\0';
        }
    };

    // tokenise on spaces, greedily packing words onto each line
    int ti = 0;
    int wi = 0;
    while (true) {
        char ch = text[ti++];
        if (ch != ' ' && ch != '\0') {
            if (wi < (int)sizeof(word) - 1) word[wi++] = ch;
            continue;
        }
        word[wi] = '\0';
        wi = 0;
        if (word[0]) {
            char trial[40];
            if (cur[0]) {
                snprintf(trial, sizeof(trial), "%s %s", cur, word);
            } else {
                snprintf(trial, sizeof(trial), "%s", word);
            }
            if (canvas.textWidth(trial) <= maxW || cur[0] == '\0') {
                strncpy(cur, trial, sizeof(cur) - 1);
                cur[sizeof(cur) - 1] = '\0';
            } else {
                flush();
                strncpy(cur, word, sizeof(cur) - 1);
                cur[sizeof(cur) - 1] = '\0';
            }
        }
        if (ch == '\0') break;
    }
    flush();

    int lineH = canvas.fontHeight() + 2;
    int total = nLines * lineH;
    int y = cy - total / 2 + lineH / 2;
    for (int i = 0; i < nLines; i++) {
        canvas.drawString(lines[i], cx, y);
        y += lineH;
    }
}

// horizontal level meter for the listening screen; level is 0..1
static void drawLevelMeter(int x, int y, int w, int h, float level) {
    canvas.drawRect(x, y, w, h, COL_DIM);
    int fill = (int)(level * (w - 4));
    if (fill < 0) fill = 0;
    if (fill > w - 4) fill = w - 4;
    // green -> amber -> red as it fills
    uint16_t c = COL_POS;
    if (level > 0.75f) c = COL_NEG;
    else if (level > 0.45f) c = COL_NEU;
    canvas.fillRect(x + 2, y + 2, fill, h - 4, c);
}

// ---------------
// screens
// ---------------
static void drawIdle() {
    canvas.fillSprite(COL_BG);

    canvas.setTextDatum(top_center);
    canvas.setTextColor(COL_ACCENT);
    canvas.setTextSize(2);
    canvas.drawString("MAGIC 8-BALL", SCREEN_W / 2, 8);

    drawBall(SCREEN_W / 2, 74, 34);

    canvas.setTextDatum(bottom_center);
    canvas.setTextColor(COL_WHITE);
    canvas.setTextSize(1);
    canvas.drawString("Press the button, then ask", SCREEN_W / 2, SCREEN_H - 6);

    canvas.pushSprite(0, 0);
}

static void drawListening() {
    canvas.fillSprite(COL_BG);

    canvas.setTextDatum(top_left);
    canvas.setTextColor(COL_WHITE);
    canvas.setTextSize(2);
    canvas.drawString("Listening", 34, 12);

    // blinking record dot (~2 Hz)
    bool on = ((millis() - stateEnteredMs) / 250) % 2 == 0;
    if (on) canvas.fillCircle(18, 22, 8, COL_REC);
    canvas.drawCircle(18, 22, 8, COL_REC);

    canvas.setTextDatum(top_center);
    canvas.setTextColor(COL_DIM);
    canvas.setTextSize(1);
    if (speechLatched) {
        canvas.drawString("Stop talking when done", SCREEN_W / 2, 44);
    } else {
        canvas.drawString("Ask your question...", SCREEN_W / 2, 44);
    }

    drawLevelMeter(30, 70, SCREEN_W - 60, 22, micLevel);

    canvas.pushSprite(0, 0);
}

static void drawShakePrompt() {
    // jiggle the whole scene a little while shakes are landing
    int dx = (jiggle > 0.2f) ? (int)(((millis() / 40) % 2) ? jiggle : -jiggle) : 0;

    canvas.fillSprite(COL_BG);

    canvas.setTextDatum(middle_center);
    canvas.setTextColor(COL_ACCENT);
    canvas.setTextSize(3);
    canvas.drawString("SHAKE!", SCREEN_W / 2 + dx, 42);

    drawBall(SCREEN_W / 2 + dx, 92, 22);

    // shake progress pips along the bottom
    int pipY = SCREEN_H - 12;
    int gap = 18;
    int startX = SCREEN_W / 2 - (SHAKE_COUNT - 1) * gap / 2;
    for (int i = 0; i < SHAKE_COUNT; i++) {
        uint16_t c = (i < shakeCount) ? COL_POS : COL_DIM;
        canvas.fillCircle(startX + i * gap, pipY, 4, c);
    }

    canvas.pushSprite(0, 0);
}

// draws the ball with the blue triangular answer window rising in.
// progress 0..1 controls how far the window has risen; show answer text
// once it's mostly up.
static void drawReveal(float progress) {
    canvas.fillSprite(COL_BG);

    int cx = SCREEN_W / 2;
    int cy = SCREEN_H / 2;
    int r  = 56;
    drawBall(cx, cy, r);

    // blue window: a downward triangle that slides up into place
    int rise = (int)((1.0f - progress) * (r + 20));
    int wy = cy + rise;          // current centre of the window
    int wr = r - 10;             // window half-size
    canvas.fillTriangle(cx, wy - wr, cx - wr, wy + wr / 2, cx + wr, wy + wr / 2,
                        COL_WINDOW);

    if (progress > 0.6f) {
        const Answer& a = ANSWERS[answerIdx];
        canvas.setTextSize(2);
        drawWrapped(a.text, cx, cy + 4, 2 * wr - 6, catColour(a.cat));
    }

    canvas.pushSprite(0, 0);
}

static void drawRevealHold() {
    canvas.fillSprite(COL_BG);
    int cx = SCREEN_W / 2;
    int cy = 62;
    int r  = 52;
    drawBall(cx, cy, r);
    int wr = r - 10;
    canvas.fillTriangle(cx, cy - wr, cx - wr, cy + wr / 2, cx + wr, cy + wr / 2,
                        COL_WINDOW);
    const Answer& a = ANSWERS[answerIdx];
    canvas.setTextSize(2);
    drawWrapped(a.text, cx, cy + 2, 2 * wr - 6, catColour(a.cat));

    canvas.setTextDatum(bottom_center);
    canvas.setTextColor(COL_DIM);
    canvas.setTextSize(1);
    canvas.drawString("Press to ask again", SCREEN_W / 2, SCREEN_H - 4);

    canvas.pushSprite(0, 0);
}

// ---------------
// sensing
// ---------------

// record one frame from the mic and return its RMS amplitude (or -1 on failure)
static float micFrameRms() {
    static int16_t buf[MIC_FRAME];
    if (!M5.Mic.record(buf, MIC_FRAME, MIC_RATE)) return -1.0f;
    while (M5.Mic.isRecording()) M5.delay(1);  // wait for the frame to fill

    double sumsq = 0.0;
    for (int i = 0; i < MIC_FRAME; i++) {
        double s = (double)buf[i];
        sumsq += s * s;
    }
    return (float)sqrt(sumsq / MIC_FRAME);
}

// read the IMU and report whether a fresh shake "jolt" just occurred
static bool detectShakeJolt() {
    float ax, ay, az;
    if (!M5.Imu.getAccel(&ax, &ay, &az)) return false;
    float mag = sqrtf(ax * ax + ay * ay + az * az);
    float delta = fabsf(mag - 1.0f);

    // feed the on-screen jiggle (decays each call)
    jiggle = max(jiggle * 0.8f, delta * 2.0f);

    uint32_t now = millis();
    if (delta > SHAKE_DELTA_G && (now - lastShakeMs) > SHAKE_DEBOUNCE_MS) {
        lastShakeMs = now;
        return true;
    }
    return false;
}

// ---------------
// per-state updates
// ---------------
static void updateIdle() {
    drawIdle();
    if (M5.BtnA.wasPressed()) {
        M5.Mic.begin();
        speechLatched = false;
        lastLoudMs    = millis();
        micLevel      = 0.0f;
        enterState(LISTENING);
    }
}

static void updateListening() {
    float rms = micFrameRms();
    if (rms >= 0.0f) {
        // smooth the level meter; print raw RMS for threshold tuning
        float norm = rms / LEVEL_MAX_RMS;
        if (norm > 1.0f) norm = 1.0f;
        micLevel = micLevel * 0.6f + norm * 0.4f;
        Serial.printf("rms=%.0f\n", rms);

        uint32_t now = millis();
        if (rms > SPEECH_ON_RMS) speechLatched = true;
        if (rms > SPEECH_OFF_RMS) lastLoudMs = now;

        bool silentLongEnough =
            speechLatched && (now - lastLoudMs) > SILENCE_MS;
        bool timedOut = (now - stateEnteredMs) > MAX_LISTEN_MS;

        if (silentLongEnough || timedOut) {
            M5.Mic.end();
            shakeCount  = 0;
            lastShakeMs = 0;
            jiggle      = 0.0f;
            enterState(PROMPT_SHAKE);
        }
    }
    drawListening();
}

static void updateShakePrompt() {
    if (detectShakeJolt()) {
        shakeCount++;
        if (shakeCount >= SHAKE_COUNT) {
            answerIdx = random(0, ANSWER_COUNT);
            Serial.printf("answer=%s\n", ANSWERS[answerIdx].text);
            enterState(REVEAL);
            return;
        }
    }
    // bail back to idle if nobody shakes for a while
    if (millis() - stateEnteredMs > SHAKE_PROMPT_MS) {
        enterState(IDLE);
        return;
    }
    drawShakePrompt();
    M5.delay(8);
}

static void updateReveal() {
    float progress = (float)(millis() - stateEnteredMs) / REVEAL_ANIM_MS;
    if (progress >= 1.0f) {
        enterState(REVEAL_HOLD);
        return;
    }
    drawReveal(progress);
    M5.delay(8);
}

static void updateRevealHold() {
    drawRevealHold();
    if (M5.BtnA.wasPressed() || (millis() - stateEnteredMs) > REVEAL_HOLD_MS) {
        enterState(IDLE);
    }
}

// ---------------
// Arduino entry points
// ---------------
void setup() {
    auto cfg = M5.config();
    cfg.internal_imu = true;
    cfg.internal_mic = true;
    cfg.internal_spk = false;  // mic and speaker can't run together; keep it off
    M5.begin(cfg);

    M5.Display.setRotation(1);  // landscape, 240 x 135
    canvas.setColorDepth(16);
    canvas.createSprite(SCREEN_W, SCREEN_H);

    randomSeed(esp_random());

    enterState(IDLE);
}

void loop() {
    M5.update();  // refresh button state

    switch (state) {
        case IDLE:         updateIdle();        break;
        case LISTENING:    updateListening();   break;
        case PROMPT_SHAKE: updateShakePrompt(); break;
        case REVEAL:       updateReveal();      break;
        case REVEAL_HOLD:  updateRevealHold();  break;
    }
}
