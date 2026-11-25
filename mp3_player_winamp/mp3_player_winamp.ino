/*
 * M5Mp3 Winamp-style MP3 Player for M5Stack Cardputer-Adv
 */

#include <SPI.h>
#include <FS.h>
#include <SD.h>
#include <M5Cardputer.h>

#include <AudioOutput.h>
#include <AudioFileSourceSD.h>
#include <AudioFileSourceID3.h>
#include <AudioGeneratorMP3.h>

#include "font.h"

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
#define SD_SCK   40
#define SD_MISO  39
#define SD_MOSI  14
#define SD_CS    12

#define MAX_FILES      100
#define VOL_MAX        21
#define VOL_STEP       3

static constexpr size_t TRI_BUF_SIZE = 1536;

// ---------------------------------------------------------------------------
// Custom AudioOutput for M5Cardputer.Speaker
// ---------------------------------------------------------------------------
class AudioOutputM5CardputerSpeaker : public AudioOutput {
public:
    explicit AudioOutputM5CardputerSpeaker(m5::Speaker_Class* m5sound)
        : _m5sound(m5sound) {}

    bool begin(void) override { return true; }

    bool ConsumeSample(int16_t sample[2]) override {
        if (_tri_buffer_index >= TRI_BUF_SIZE) {
            flush();
        }
        int32_t sum = (int32_t)sample[0] + (int32_t)sample[1];
        _tri_buffer[_tri_index][_tri_buffer_index++] = (int16_t)(sum / 2);
        return true;
    }

    void flush(void) override {
        if (_tri_buffer_index == 0) return;
        uint32_t start = millis();
        while (_m5sound->isPlaying() && (millis() - start) < 200) {
            vTaskDelay(1);
        }
        _m5sound->playRaw(_tri_buffer[_tri_index], _tri_buffer_index, hertz, false);
        _tri_index = (_tri_index + 1) % 3;
        _tri_buffer_index = 0;
    }

    bool stop(void) override {
        flush();
        while (_m5sound->isPlaying()) {
            vTaskDelay(1);
        }
        return true;
    }

private:
    m5::Speaker_Class* _m5sound;
    int16_t _tri_buffer[3][TRI_BUF_SIZE];
    size_t  _tri_buffer_index = 0;
    size_t  _tri_index        = 0;
};

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
M5Canvas sprite(&M5Cardputer.Display);
M5Canvas spr(&M5Cardputer.Display);

String audioFiles[MAX_FILES];      
String displayNames[MAX_FILES];    
int    fileCount = 0;

// Core state (volatile for cross-core safety)
volatile bool g_stopped       = false;   // true => paused
volatile bool g_playRequested = true;    // true => resume playback
volatile bool g_nextTrack     = false;   // true => load track
volatile bool g_volChanged    = false;   
volatile int  g_currentTrack  = 0;
volatile int  g_volume        = 10;      
volatile int  g_textPos       = 60;

// UI state
unsigned short grays[18];
unsigned short gray;
unsigned short light;
int  bri = 0;
const int brightness[5] = {50, 100, 150, 200, 250};
int  g_eq[14]   = {0};
bool s_dirty    = true;

unsigned long trackStartTime     = 0;
unsigned long trackElapsedSecs   = 0;

TaskHandle_t handleAudioTask = nullptr;
AudioFileSourceSD*               audioFile = nullptr;
AudioFileSourceID3*              audioId3  = nullptr;
AudioGeneratorMP3*               audioMp3  = nullptr;
AudioOutputM5CardputerSpeaker*   audioOut  = nullptr;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
void resetClock() {
    trackStartTime   = millis();
    trackElapsedSecs = 0;
}

String getTimeString() {
    unsigned long elapsed = g_stopped ? trackElapsedSecs : (millis() - trackStartTime) / 1000;
    char buf[6];
    snprintf(buf, sizeof(buf), "%02d:%02d", (int)((elapsed / 60) % 100), (int)(elapsed % 60));
    return String(buf);
}

String truncateToWidth(M5Canvas& canvas, const String& text, int maxWidth) {
    if (canvas.textWidth(text) <= maxWidth) return text;
    String truncated = text;
    while (truncated.length() > 0 && canvas.textWidth(truncated + "...") > maxWidth) {
        truncated.remove(truncated.length() - 1);
    }
    return truncated + "...";
}

// ---------------------------------------------------------------------------
// File Listing
// ---------------------------------------------------------------------------
void listFiles(fs::FS& fs, const char* dirname, uint8_t levels) {
    File root = fs.open(dirname);
    if (!root || !root.isDirectory()) return;

    sprite.setTextFont(0);

    File file = root.openNextFile();
    while (file && fileCount < MAX_FILES) {
        if (file.isDirectory()) {
            if (levels) listFiles(fs, file.name(), levels - 1);
        } else {
            String raw = file.name();
            String lower = raw;
            lower.toLowerCase();
            if (lower.endsWith(".mp3")) {
                audioFiles[fileCount] = raw.startsWith("/") ? raw : ("/" + raw);
                int slash = raw.lastIndexOf('/');
                String baseName = (slash >= 0) ? raw.substring(slash + 1) : raw;
                displayNames[fileCount] = truncateToWidth(sprite, baseName, 120);
                fileCount++;
            }
        }
        file = root.openNextFile();
    }
}

// ---------------------------------------------------------------------------
// Audio Management
// ---------------------------------------------------------------------------
void releaseTrack() {
    if (audioMp3) {
        if (audioMp3->isRunning()) audioMp3->stop();
        delete audioMp3;
        audioMp3 = nullptr;
    }
    if (audioId3) { delete audioId3; audioId3 = nullptr; }
    if (audioFile){ delete audioFile; audioFile = nullptr; }
}

bool loadTrack(int idx) {
    if (idx < 0 || idx >= fileCount) return false;
    releaseTrack();

    audioFile = new AudioFileSourceSD(audioFiles[idx].c_str());
    if (!audioFile) return false;

    audioId3 = new AudioFileSourceID3(audioFile);
    audioMp3 = new AudioGeneratorMP3();
    if (!audioMp3->begin(audioId3, audioOut)) {
        releaseTrack();
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(300);
    resetClock();

    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.setBrightness(brightness[bri]);

    sprite.createSprite(240, 135);
    spr.createSprite(86, 16);

    if (!SD.begin(SD_CS, SPI, 25000000)) {
        M5Cardputer.Display.fillScreen(BLACK);
        M5Cardputer.Display.setTextColor(RED);
        M5Cardputer.Display.setCursor(10, 60);
        M5Cardputer.Display.println("SD Card Error!");
        while (true) delay(1000);
    }

    listFiles(SD, "/", MAX_FILES);

    if (fileCount == 0) {
        M5Cardputer.Display.fillScreen(BLACK);
        M5Cardputer.Display.setTextColor(YELLOW);
        M5Cardputer.Display.setCursor(10, 60);
        M5Cardputer.Display.println("No MP3 files!");
        while (true) delay(1000);
    }

    M5Cardputer.Speaker.begin();
    M5Cardputer.Speaker.setVolume(map((int)g_volume, 0, VOL_MAX, 0, 255));
    audioOut = new AudioOutputM5CardputerSpeaker(&M5Cardputer.Speaker);
    audioOut->SetGain((float)g_volume / (float)VOL_MAX);

    loadTrack(0);

    int co = 214;
    for (int i = 0; i < 18; i++) {
        grays[i] = M5Cardputer.Display.color565(co, co, co + 40);
        co -= 13;
    }

    xTaskCreatePinnedToCore(Task_TFT,   "Task_TFT",   20480, nullptr, 2, nullptr,          0);
    xTaskCreatePinnedToCore(Task_Audio, "Task_Audio", 10240, nullptr, 3, &handleAudioTask, 1);
}

void loop() {}

// ---------------------------------------------------------------------------
// Drawing (TFT)
// ---------------------------------------------------------------------------
void draw() {
    const bool animate = !g_stopped;
    if (!s_dirty) {
        if (animate) { } else { return; }
    }

    gray  = grays[15];
    light = grays[11];

    sprite.fillRect(0, 0, 240, 135, gray);
    sprite.fillRect(4, 8, 130, 122, BLACK);
    sprite.fillRect(129, 8, 5, 122, 0x0841);

    int curTrack = (int)g_currentTrack;
    int sliderPos = map(curTrack, 0, fileCount > 1 ? fileCount - 1 : 1, 8, 110);
    sprite.fillRect(129, sliderPos, 5, 20, grays[2]);
    sprite.fillRect(131, sliderPos + 4, 1, 12, grays[16]);

    sprite.fillRect(4,   2, 50, 2, ORANGE);
    sprite.fillRect(84,  2, 50, 2, ORANGE);
    sprite.fillRect(190, 2, 45, 2, ORANGE);
    sprite.fillRect(190, 6, 45, 3, grays[4]);
    sprite.drawFastVLine(3,   9, 120, light);
    sprite.drawFastVLine(134, 9, 120, light);
    sprite.drawFastHLine(3, 129, 130, light);
    sprite.drawFastHLine(0,   0, 240, light);
    sprite.drawFastHLine(0, 134, 240, light);

    sprite.fillRect(139,  0,   3, 135, BLACK);
    sprite.fillRect(148, 14,  86,  42, BLACK);
    sprite.fillRect(148, 59,  86,  16, BLACK);

    sprite.fillTriangle(162, 18, 162, 26, 168, 22, GREEN);
    sprite.fillRect(162, 30, 6, 6, RED);

    sprite.drawFastVLine(143,  0, 135, light);
    sprite.drawFastVLine(238,  0, 135, light);
    sprite.drawFastVLine(138,  0, 135, light);
    sprite.drawFastVLine(148, 14,  42, light);
    sprite.drawFastHLine(148, 14,  86, light);

    for (int i = 0; i < 4; i++)
        sprite.fillRoundRect(148 + i * 22, 94, 18, 18, 3, grays[4]);

    sprite.fillRect(220, 104, 8, 2, grays[13]);
    sprite.fillRect(220, 108, 8, 2, grays[13]);
    sprite.fillTriangle(228, 102, 228, 106, 231, 105, grays[13]);
    sprite.fillTriangle(220, 106, 220, 110, 217, 109, grays[13]);

    if (!g_stopped) {
        sprite.fillRect(152, 104, 3, 6, grays[13]);
        sprite.fillRect(157, 104, 3, 6, grays[13]);
    } else {
        sprite.fillTriangle(156, 102, 156, 110, 160, 106, grays[13]);
    }

    int volPx = ((int)g_volume * 60) / VOL_MAX;
    sprite.fillRoundRect(172, 82, 60, 3, 2, YELLOW);
    int kx = 155 + volPx;
    sprite.fillRoundRect(kx,     80, 10, 8, 2, grays[2]);
    sprite.fillRoundRect(kx + 2, 82,  6, 4, 2, grays[10]);

    sprite.fillRoundRect(172, 124, 30, 3, 2, MAGENTA);
    sprite.fillRoundRect(172 + bri * 5, 122, 10, 8, 2, grays[2]);
    sprite.fillRoundRect(174 + bri * 5, 124,  6, 4, 2, grays[10]);

    sprite.drawRect(206, 119, 28, 12, GREEN);
    sprite.fillRect(234, 122,  3,  6, GREEN);

    for (int i = 0; i < 14; i++) {
        if (animate) g_eq[i] = random(1, 5);
        for (int j = 0; j < g_eq[i]; j++)
            sprite.fillRect(172 + i * 4, 50 - j * 3, 3, 2, grays[4]);
    }

    sprite.setTextFont(0);
    sprite.setTextDatum(0);

    int startIdx = (curTrack < 5) ? 0 : curTrack - 5;
    int row = 0;
    for (int i = startIdx; i < startIdx + 10 && i < fileCount; i++) {
        if (i == curTrack) sprite.setTextColor(WHITE, BLACK);
        else               sprite.setTextColor(GREEN, BLACK);
        sprite.drawString(displayNames[i], 8, 10 + row * 12);
        row++;
    }

    sprite.setTextColor(grays[1], gray);
    sprite.drawString("WINAMP", 150, 4);
    sprite.setTextColor(grays[2], gray);
    sprite.drawString("LIST", 58, 0);
    sprite.setTextColor(grays[4], gray);
    sprite.drawString("VOL", 150, 80);
    sprite.drawString("LIG", 150, 122);

    sprite.setTextColor(grays[8], BLACK);
    const char* st = g_stopped ? "STOP" : "PLAY";
    for (int k = 0; k < 4; k++) {
        char c[2] = { st[k], 0 };
        sprite.drawString(c, 152, 18 + k * 9);
    }

    sprite.setTextColor(GREEN, BLACK);
    sprite.setFont(&DSEG7_Classic_Mini_Regular_16);
    if (!g_stopped) {
        trackElapsedSecs = (millis() - trackStartTime) / 1000;
        sprite.drawString(getTimeString(), 172, 18);
    }
    sprite.setTextFont(0);

    int percent;
    float v = M5Cardputer.Power.getBatteryVoltage();
    if (v > 4.2f)       percent = 100;
    else if (v < 3.0f)  percent = 1;
    else                percent = map((int)(v * 100), 300, 420, 1, 100);

    sprite.setTextDatum(3);
    sprite.drawString(String(percent) + "%", 220, 121);

    sprite.setTextColor(BLACK, grays[4]);
    sprite.drawString("B", 220, 96);
    sprite.drawString("N", 198, 96);
    sprite.drawString("P", 176, 96);
    sprite.drawString("A", 154, 96);

    sprite.setTextColor(BLACK, grays[5]);
    sprite.drawString(">>", 202, 103);
    sprite.drawString("<<", 180, 103);

    spr.fillSprite(BLACK);
    spr.setTextColor(GREEN, BLACK);
    spr.setClipRect(0, 0, 86, 16);
    
    if (!g_stopped && curTrack < fileCount) {
        int slash = audioFiles[curTrack].lastIndexOf('/');
        String rawName = (slash >= 0) ? audioFiles[curTrack].substring(slash + 1) : audioFiles[curTrack];
        spr.drawString(rawName, g_textPos, 4);
        if (g_textPos < -(spr.textWidth(rawName) + 20)) g_textPos = 90;
    } else {
        g_textPos = 90; 
    }
    
    g_textPos -= 2;
    spr.pushSprite(&sprite, 148, 59);
    spr.clearClipRect();

    sprite.pushSprite(0, 0);
    s_dirty = false;
}

// ---------------------------------------------------------------------------
// TFT / Input Task
// ---------------------------------------------------------------------------
void Task_TFT(void *arg __attribute__((unused))) {
    int lastEqTick = 0;

    while (true) {
        M5Cardputer.update();

        if (M5Cardputer.Keyboard.isChange()) {
            auto& kb = M5Cardputer.Keyboard;

            // Play/Pause (Key 'a')
            if (kb.isKeyPressed('a')) {
                g_stopped = !g_stopped;
                g_playRequested = !g_stopped; 
                s_dirty = true;
            }

            // Volume (Key 'v')
            if (kb.isKeyPressed('v')) {
                g_volume = (g_volume + VOL_STEP) % (VOL_MAX + 1);
                g_volChanged = true;
                s_dirty = true;
            }

            // Brightness (Key 'l')
            if (kb.isKeyPressed('l')) {
                bri = (bri + 1) % 5;
                M5Cardputer.Display.setBrightness(brightness[bri]);
                s_dirty = true;
            }

            // Move UP in list (Key ';' or Up Arrow)
            if (kb.isKeyPressed(';') || kb.isKeyPressed(0x1A)) {
                int t = g_currentTrack - 1;
                if (t < 0) t = fileCount - 1;
                g_currentTrack = t;
                s_dirty = true;
            }

            // Move DOWN in list (Key '.' or Down Arrow)
            if (kb.isKeyPressed('.') || kb.isKeyPressed(0x1B)) {
                int t = g_currentTrack + 1;
                if (t >= fileCount) t = 0;
                g_currentTrack = t;
                s_dirty = true;
            }

            // Next Track (Key 'n')
            if (kb.isKeyPressed('n')) {
                resetClock();
                g_textPos = 90;
                int t = g_currentTrack + 1;
                if (t >= fileCount) t = 0;
                g_currentTrack = t;
                g_nextTrack = true;
                g_stopped = false;
                g_playRequested = true;
                s_dirty = true;
            }

            // Previous Track (Key 'p')
            if (kb.isKeyPressed('p')) {
                resetClock();
                g_textPos = 90;
                int t = g_currentTrack - 1;
                if (t < 0) t = fileCount - 1;
                g_currentTrack = t;
                g_nextTrack = true;
                g_stopped = false;
                g_playRequested = true;
                s_dirty = true;
            }

            // ENTER / OK: Play selected track
            if (kb.isKeyPressed('\n') || kb.isKeyPressed('\r') || kb.isKeyPressed(KEY_ENTER)) {
                resetClock();
                g_textPos = 90;
                g_stopped = false;
                g_nextTrack = true;
                g_playRequested = true;
                s_dirty = true;
            }

            // Random Track (Key 'b')
            if (kb.isKeyPressed('b')) {
                resetClock();
                g_textPos = 90;
                g_currentTrack = random(0, fileCount);
                g_nextTrack = true;
                g_stopped = false;
                g_playRequested = true;
                s_dirty = true;
            }
        }

        if ((millis() - lastEqTick) >= 100) {
            lastEqTick = millis();
            s_dirty = true;
        }

        draw();
        vTaskDelay(40 / portTICK_PERIOD_MS);
    }
}

// ---------------------------------------------------------------------------
// Audio Task
// ---------------------------------------------------------------------------
void Task_Audio(void *arg __attribute__((unused))) {
    while (true) {
        // Volume changes
        if (g_volChanged) {
            g_volChanged = false;
            int v = g_volume;
            if (audioOut) audioOut->SetGain((float)v / (float)VOL_MAX);
            M5Cardputer.Speaker.setVolume(map(v, 0, VOL_MAX, 0, 255));
        }

        // Track change request
        if (g_nextTrack) {
            g_nextTrack = false;
            
            if (audioMp3) {
                if (audioMp3->isRunning()) audioMp3->stop();
            }

            if (!loadTrack(g_currentTrack)) {
                Serial.printf("Failed to load track: %d\n", g_currentTrack);
            } else {
                resetClock();
                g_playRequested = true; // Guarantee it plays the new track
            }
        }

        // Playback Loop
        if (audioMp3) {
            if (!g_stopped) {
                if (!audioMp3->isRunning() && g_playRequested) {
                    audioMp3->loop();
                    g_playRequested = false;
                } 
                else if (audioMp3->isRunning()) {
                    if (!audioMp3->loop()) {
                        // End of file, auto advance
                        int t = g_currentTrack + 1;
                        if (t >= fileCount) t = 0;
                        g_currentTrack = t;
                        g_nextTrack = true;
                        g_playRequested = true;
                        resetClock();
                    }
                }
            }
        }

        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}