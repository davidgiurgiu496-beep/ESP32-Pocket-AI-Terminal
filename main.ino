#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>
#include <time.h>
#include <vector>
#include <TJpg_Decoder.h>
#include "secrets.h"

// ============================================================
const char* cloudEndpoint = "https://api.groq.com/openai/v1/chat/completions";


#define TFT_CS  15
#define TFT_RST 4
#define TFT_DC  2

Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);

#define ST77XX_GRAY 0x7BEF

// ============================================================
// JOYSTICK
// ============================================================
#define JOY_X   34
#define JOY_Y   35
#define JOY_BTN 32

#define CHAT_H     58     // chat zone      y: 0..57
#define SLIDER_Y   60     // slider         y: 60..68
#define INPUT_Y    70     // text bar       y: 70..81
#define KB_Y       84     // keyboard       y: 84..127
#define KEY_H      10
#define KEY_STEP   11
#define MAX_CHARS_PER_LINE 15
#define LINE_H     9


#define C_BG        0x0000
#define C_KEY       0x2945
#define C_KEY_SPEC  0x3186
#define C_KEY_SEL   0xFFE0
#define C_USER      0x051D
#define C_AI        0x39E7
#define C_TEXTMODE  0x051D
#define C_FOTOMODE  0xFD20


#define MAX_MSG 10

struct Msg {
    bool mine;
    String text;
};

Msg msgs[MAX_MSG];
int msgCount = 0;

String textIntrodus = "";
int modImagine = 0;          // 0 = text, 1 = foto

int kr = 0;                  // cursor row (-1 = scroll chat)
int kc = 0;                  // cursor column
int scrollPx = 0;

GFXcanvas16* canvas = nullptr;

const char* rowsStr[3] = {
    "QWERTYUIOP",
    "ASDFGHJKL<",
    "ZXCVBNM.?!"
};

unsigned long lastMove = 0;
unsigned long lastBtn = 0;
bool btnPrev = false;


void drawAll();
void drawChat();
void drawSlider();
void drawInput(const char* status = nullptr);
void drawKeyboard();
void drawKey(int r, int c, bool sel);
void sendMessage();


bool tft_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
    if (y >= tft.height()) {
        return false;
    }
    tft.drawRGBBitmap(x, y, bitmap, w, h);
    return true;
}

String stripDiacritics(String s) {
    s.replace("ă", "a"); s.replace("â", "a"); s.replace("î", "i");
    s.replace("ș", "s"); s.replace("ț", "t"); s.replace("ş", "s"); s.replace("ţ", "t");
    s.replace("Ă", "A"); s.replace("Â", "A"); s.replace("Î", "I");
    s.replace("Ș", "S"); s.replace("Ț", "T"); s.replace("Ş", "S"); s.replace("Ţ", "T");
    s.replace("–", "-"); s.replace("—", "-");
    s.replace("‘", "'"); s.replace("’", "'");
    s.replace("“", "\""); s.replace("”", "\"");

    String out = "";
    for (unsigned int i = 0; i < s.length(); i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\n' || c == '\r' || c == '\t') {
            out += ' ';
        } else if (c >= 32 && c < 127) {
            out += (char)c;
        }
    }
    out.trim();
    return out;
}

String urlEncode(const String& s) {
    String out = "";
    const char* hex = "0123456789ABCDEF";
    for (unsigned int i = 0; i < s.length(); i++) {
        char c = s[i];
        if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += c;
        } else {
            out += '%';
            out += hex[((unsigned char)c) >> 4];
            out += hex[((unsigned char)c) & 15];
        }
    }
    return out;
}

void wrapText(const String& s, std::vector<String>& out) {
    out.clear();
    String line = "";
    String word = "";

    auto place = [&](String w) {
        while ((int)w.length() > MAX_CHARS_PER_LINE) {
            if (line.length()) {
                out.push_back(line);
                line = "";
            }
            out.push_back(w.substring(0, MAX_CHARS_PER_LINE));
            w = w.substring(MAX_CHARS_PER_LINE);
        }
        if (w.length() == 0) return;
        if (line.length() == 0) {
            line = w;
        } else if ((int)(line.length() + 1 + w.length()) <= MAX_CHARS_PER_LINE) {
            line += " ";
            line += w;
        } else {
            out.push_back(line);
            line = w;
        }
    };

    for (unsigned int i = 0; i < s.length(); i++) {
        char c = s[i];
        if (c == ' ') {
            place(word);
            word = "";
        } else {
            word += c;
        }
    }
    place(word);
    if (line.length()) out.push_back(line);
    if (out.empty()) out.push_back("");
}


void addMsg(bool mine, String text) {
    if (text.length() > 300) {
        text = text.substring(0, 300);
    }
    if (msgCount == MAX_MSG) {
        for (int i = 1; i < MAX_MSG; i++) {
            msgs[i - 1] = msgs[i];
        }
        msgCount = MAX_MSG - 1;
    }
    msgs[msgCount].mine = mine;
    msgs[msgCount].text = text;
    msgCount++;
    scrollPx = 0;
}


void drawChat() {
    canvas->fillScreen(C_BG);
    canvas->setTextSize(1);
    canvas->setTextWrap(false);

    std::vector<String> lines;

    int total = 0;
    for (int i = 0; i < msgCount; i++) {
        wrapText(msgs[i].text, lines);
        total += (int)lines.size() * LINE_H + 5 + 3;
    }

    int maxScroll = total - (CHAT_H - 2);
    if (maxScroll < 0) maxScroll = 0;
    if (scrollPx > maxScroll) scrollPx = maxScroll;
    if (scrollPx < 0) scrollPx = 0;

    int yBottom = CHAT_H - 2 + scrollPx;

    for (int i = msgCount - 1; i >= 0 && yBottom > 0; i--) {
        wrapText(msgs[i].text, lines);
        int n = lines.size();
        int h = n * LINE_H + 5;
        int top = yBottom - h;

        if (top < CHAT_H) {
            int maxlen = 0;
            for (int k = 0; k < n; k++) {
                if ((int)lines[k].length() > maxlen) maxlen = lines[k].length();
            }
            int w = maxlen * 6 + 8;
            int x = msgs[i].mine ? (128 - w - 3) : 2;

            canvas->fillRoundRect(x, top, w, h, 4, msgs[i].mine ? C_USER : C_AI);
            canvas->setTextColor(ST77XX_WHITE);
            for (int k = 0; k < n; k++) {
                canvas->setCursor(x + 4, top + 3 + k * LINE_H);
                canvas->print(lines[k]);
            }
        }
        yBottom = top - 3;
    }

    if (msgCount == 0) {
        canvas->setTextColor(ST77XX_GRAY);
        canvas->setCursor(20, 22);
        canvas->print("write a message");
        canvas->setCursor(14, 34);
        canvas->print("with joystick+click");
    }

    if (scrollPx < maxScroll) {
        canvas->fillTriangle(122, 6, 118, 2, 126, 2, ST77XX_CYAN);
    }
    if (scrollPx > 0) {
        canvas->fillTriangle(122, CHAT_H - 6, 118, CHAT_H - 2, 126, CHAT_H - 2, ST77XX_CYAN);
    }

    if (kr == -1) {
        canvas->drawRect(0, 0, 128, CHAT_H, ST77XX_CYAN);
    }

    tft.drawRGBBitmap(0, 0, canvas->getBuffer(), 128, CHAT_H);
}


void drawSlider() {
    tft.fillRect(0, SLIDER_Y - 1, 128, 11, C_BG);

    tft.fillRoundRect(24, SLIDER_Y, 80, 9, 4, C_KEY);

    int knobX = (modImagine == 0) ? 24 : 64;
    tft.fillRoundRect(knobX, SLIDER_Y, 40, 9, 4, modImagine == 0 ? C_TEXTMODE : C_FOTOMODE);

    tft.setTextSize(1);

    tft.setTextColor(modImagine == 0 ? ST77XX_WHITE : ST77XX_GRAY);
    tft.setCursor(32, SLIDER_Y + 1);
    tft.print("TEXT");

    tft.setTextColor(modImagine == 1 ? ST77XX_WHITE : ST77XX_GRAY);
    tft.setCursor(72, SLIDER_Y + 1);
    tft.print("FOTO");
}

void drawInput(const char* status) {
    tft.fillRect(0, INPUT_Y, 128, 13, C_BG);
    tft.drawRoundRect(0, INPUT_Y, 128, 13, 3, modImagine == 0 ? C_TEXTMODE : C_FOTOMODE);

    tft.setTextSize(1);
    tft.setCursor(4, INPUT_Y + 3);

    if (status != nullptr) {
        tft.setTextColor(ST77XX_YELLOW);
        tft.print(status);
        return;
    }

    if (textIntrodus.length() == 0) {
        tft.setTextColor(ST77XX_GRAY);
        tft.print("write message...");
        return;
    }

    String vis = textIntrodus;
    if (vis.length() > 19) {
        vis = vis.substring(vis.length() - 19);
    }
    tft.setTextColor(ST77XX_WHITE);
    tft.print(vis);
    tft.print("_");
}


void keyRect(int r, int c, int& x, int& y, int& w) {
    y = KB_Y + r * KEY_STEP;
    if (r < 3) {
        x = c * 128 / 10;
        int nx = (c + 1) * 128 / 10;
        w = nx - x - 1;
    } else {
        if (c == 0)      { x = 0;  w = 31; }
        else if (c == 1) { x = 32; w = 63; }
        else             { x = 96; w = 32; }
    }
}

void drawKey(int r, int c, bool sel) {
    int x, y, w;
    keyRect(r, c, x, y, w);

    bool spec = false;
    String label = "";

    if (r < 3) {
        char ch = rowsStr[r][c];
        if (ch == '<') {
            spec = true;
            label = "<";
        } else {
            label = String(ch);
        }
    } else {
        spec = true;
        if (c == 0)      label = "MODE";
        else if (c == 1) label = "space";
        else             label = "SEND";
    }

    uint16_t bg = sel ? C_KEY_SEL : (spec ? C_KEY_SPEC : C_KEY);
    uint16_t fg = sel ? ST77XX_BLACK : ST77XX_WHITE;

    tft.fillRoundRect(x, y, w, KEY_H, 2, bg);
    tft.setTextSize(1);
    tft.setTextColor(fg);

    int tw = label.length() * 6;
    tft.setCursor(x + (w - tw) / 2 + 1, y + 1);
    tft.print(label);
}

void drawKeyboard() {
    tft.fillRect(0, KB_Y - 1, 128, 128 - KB_Y + 1, C_BG);
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 10; c++) {
            drawKey(r, c, (r == kr && c == kc));
        }
    }
    for (int c = 0; c < 3; c++) {
        drawKey(3, c, (kr == 3 && c == kc));
    }
}

void drawAll() {
    tft.fillScreen(C_BG);
    drawChat();
    drawSlider();
    drawInput();
    drawKeyboard();
}


String intreabaGroq(String promptUser, bool& ok) {
    ok = false;

    if (WiFi.status() != WL_CONNECTED) {
        return "WiFi disconnected";
    }

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    http.setTimeout(30000);

    if (!http.begin(client, cloudEndpoint)) {
        return "http.begin failed";
    }

    DynamicJsonDocument doc(4096);
    doc["model"] = "openai/gpt-oss-20b";

    JsonArray messages = doc.createNestedArray("messages");
    JsonObject msg = messages.createNestedObject();
    msg["role"] = "user";

    String prompt =
        "in english shortly explained "
        "sentence must be made of maximum 50 words: "
        + promptUser;

    msg["content"] = prompt;

    doc["temperature"] = 0.6;
    doc["max_completion_tokens"] = 1024;
    doc["reasoning_effort"] = "low";
    doc["include_reasoning"] = false;

    String jsonPayload;
    serializeJson(doc, jsonPayload);

    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", groqToken);

    Serial.println("sending POST to Groq...");
    int httpResponseCode = http.POST(jsonPayload);
    Serial.print("HTTP CODE: ");
    Serial.println(httpResponseCode);

    String responseStr = http.getString();
    http.end();

    Serial.println(responseStr);

    DynamicJsonDocument jsonResponse(16384);
    DeserializationError error = deserializeJson(jsonResponse, responseStr);

    if (httpResponseCode != 200) {
        String errorMessage = "HTTP " + String(httpResponseCode);
        if (!error && jsonResponse.containsKey("error") &&
            jsonResponse["error"].containsKey("message")) {
            errorMessage = jsonResponse["error"]["message"].as<String>();
        }
        return errorMessage;
    }

    if (error) {
        return String("JSON: ") + error.c_str();
    }

    if (jsonResponse.containsKey("error")) {
        return jsonResponse["error"]["message"].as<String>();
    }

    if (!jsonResponse.containsKey("choices") || jsonResponse["choices"].size() == 0) {
        return "Raspuns fara choices";
    }

    JsonObject firstChoice = jsonResponse["choices"][0];
    String textFinal = "";

    if (firstChoice.containsKey("message")) {
        JsonObject message = firstChoice["message"];
        if (message.containsKey("content") && !message["content"].isNull()) {
            textFinal = message["content"].as<String>();
        }
    }

    textFinal = stripDiacritics(textFinal);

    if (textFinal.length() == 0) {
        String diag = "Empty answer";
        if (firstChoice.containsKey("finish_reason")) {
            diag += " (";
            diag += firstChoice["finish_reason"].as<String>();
            diag += ")";
        }
        return diag;
    }

    ok = true;
    return textFinal;
}


String genereazaImagine(String promptUser, bool& ok) {
    ok = false;

    if (WiFi.status() != WL_CONNECTED) {
        return "WiFi disconnected";
    }

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;

    long seedAleatoriu = micros() + random(0, 100000);

    String urlComplet =
        "https://image.pollinations.ai/p/" +
        urlEncode(promptUser) +
        "?width=128"
        "&height=128"
        "&nologo=true"
        "&model=turbo"
        "&seed=" +
        String(seedAleatoriu);

    Serial.println("IMAGE URL:");
    Serial.println(urlComplet);

    if (!http.begin(client, urlComplet)) {
        return "Can't open URL";
    }

    http.setTimeout(40000);
    http.useHTTP10(true);   // without chunked, so that we can read clean JPEG
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

    int httpCode = http.GET();

    Serial.print("Image HTTP code: ");
    Serial.println(httpCode);

    if (httpCode != 200) {
        http.end();
        return "HTTP Image " + String(httpCode);
    }

    const size_t maxBufSize = 32768;
    uint8_t* jpgBuffer = (uint8_t*)malloc(maxBufSize);

    if (jpgBuffer == NULL) {
        http.end();
        return "Insufficient memory";
    }

    int totalLen = http.getSize();   // -1 if it isnt known
    WiFiClient* stream = http.getStreamPtr();

    size_t bytesRead = 0;
    unsigned long startTimeout = millis();

    while (
        (http.connected() || stream->available() > 0) &&
        bytesRead < maxBufSize &&
        (totalLen < 0 || bytesRead < (size_t)totalLen) &&
        millis() - startTimeout < 15000
    ) {
        int av = stream->available();
        if (av > 0) {
            size_t toRead = maxBufSize - bytesRead;
            if ((size_t)av < toRead) toRead = av;
            int r = stream->readBytes(jpgBuffer + bytesRead, toRead);
            bytesRead += r;
            startTimeout = millis();
        } else {
            delay(5);
        }
    }

    http.end();

    Serial.print("Bytes JPEG: ");
    Serial.println(bytesRead);

    if (bytesRead < 100) {
        free(jpgBuffer);
        return "Data wasn't recieved";
    }

    tft.fillScreen(C_BG);

    JRESULT result = TJpgDec.drawJpg(0, 0, jpgBuffer, bytesRead);

    Serial.print("JPEG result: ");
    Serial.println(result);

    if (result != 0) {
        free(jpgBuffer);
        return "Decoding of JPEG failed";
    }

    // image stays in RAM + on screen for 10 seconds
    delay(10000);

    // clearing from RAM
    free(jpgBuffer);
    jpgBuffer = NULL;

    ok = true;
    return "[image generated]";
}

// ============================================================
// MESSAGE SENT
// ============================================================
void sendMessage() {
    textIntrodus.trim();
    if (textIntrodus.length() == 0) {
        return;
    }

    String prompt = textIntrodus;
    prompt.toLowerCase();

    if (modImagine == 1) {
        addMsg(true, String("[foto] ") + textIntrodus);
    } else {
        addMsg(true, textIntrodus);
    }

    textIntrodus = "";

    drawChat();

    bool ok = false;
    String rezultat;

    if (modImagine == 1) {
        drawInput("Generating image...");
        rezultat = genereazaImagine(prompt, ok);
    } else {
        drawInput("Waiting for AI...");
        rezultat = intreabaGroq(prompt, ok);
    }

    if (ok) {
        addMsg(false, rezultat);
    } else {
        addMsg(false, String("ERROR: ") + stripDiacritics(rezultat));
    }

    kr = 3;
    kc = 2;

    drawAll();
}


void moveCursor(int dx, int dy) {

    // ---------------- chat scroll mode ----------------
    if (kr == -1) {
        if (dy < 0) {
            scrollPx += 12;
            drawChat();
        } else if (dy > 0) {
            if (scrollPx > 0) {
                scrollPx -= 12;
                if (scrollPx < 0) scrollPx = 0;
                drawChat();
            } else {
                kr = 0;
                if (kc > 9) kc = 9;
                drawChat();
                drawKey(kr, kc, true);
            }
        }
        return;
    }

    int oldR = kr;
    int oldC = kc;

    int cols = (kr < 3) ? 10 : 3;

    if (dx != 0) {
        kc += dx;
        if (kc < 0) kc = cols - 1;
        if (kc >= cols) kc = 0;
    } else if (dy < 0) {
        if (kr == 0) {
            drawKey(oldR, oldC, false);
            kr = -1;
            drawChat();
            return;
        }
        kr--;
        if (oldR == 3) {
            kc = (oldC == 0) ? 1 : (oldC == 1 ? 5 : 9);
        }
    } else if (dy > 0) {
        if (kr < 3) {
            kr++;
            if (kr == 3) {
                kc = (oldC < 3) ? 0 : (oldC < 8 ? 1 : 2);
            }
        }
    }

    if (oldR != kr || oldC != kc) {
        drawKey(oldR, oldC, false);
        drawKey(kr, kc, true);
    }
}

void pressKey() {
    if (kr == -1) return;

    if (kr < 3) {
        char ch = rowsStr[kr][kc];

        if (ch == '<') {
            if (textIntrodus.length() > 0) {
                textIntrodus.remove(textIntrodus.length() - 1);
            }
        } else {
            if (textIntrodus.length() < 80) {
                textIntrodus += ch;
            }
        }
        drawInput();
        return;
    }

    if (kc == 0) {
        modImagine = 1 - modImagine;
        drawSlider();
        drawInput();
    } else if (kc == 1) {
        if (textIntrodus.length() < 80) {
            textIntrodus += ' ';
        }
        drawInput();
    } else {
        sendMessage();
    }
}


void setup() {

    Serial.begin(115200);
    delay(500);

    pinMode(JOY_BTN, INPUT_PULLUP);

    tft.initR(INITR_144GREENTAB);
    tft.setRotation(1);
    tft.fillScreen(ST77XX_BLACK);

    tft.setCursor(2, 20);
    tft.setTextColor(ST77XX_CYAN);
    tft.setTextSize(1);
    tft.println("Connecting to WiFi...");

    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);

    Serial.println();
    Serial.println("Connecting to WiFi...");

    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }

    Serial.println();
    Serial.println("WiFi connected!");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());

    tft.fillScreen(ST77XX_BLACK);
    tft.setCursor(2, 20);
    tft.setTextColor(ST77XX_GREEN);
    tft.println("WiFi connected!");
    delay(1000);

    tft.fillScreen(ST77XX_BLACK);
    tft.setCursor(2, 20);
    tft.setTextColor(ST77XX_GREEN);
    tft.println("Syncing time...");

    configTime(2 * 3600, 0, "pool.ntp.org", "time.nist.gov");

    time_t now = time(nullptr);
    unsigned long startTime = millis();

    while (now < 8 * 3600 * 2 && millis() - startTime < 15000) {
        delay(500);
        now = time(nullptr);
    }

    Serial.println("Time syncing done.");

    TJpgDec.setJpgScale(1);
    TJpgDec.setCallback(tft_output);

    canvas = new GFXcanvas16(128, CHAT_H);

    kr = 0;
    kc = 0;

    drawAll();
}


void loop() {

    int xVal = analogRead(JOY_X);
    int yVal = analogRead(JOY_Y);
    bool btn = (digitalRead(JOY_BTN) == LOW);

    unsigned long now = millis();

    if (btn && !btnPrev && now - lastBtn > 200) {
        lastBtn = now;
        pressKey();
    }
    btnPrev = btn;

    if (now - lastMove > 170) {
        int dx = 0;
        int dy = 0;

        if (xVal < 1000) dx = -1;
        else if (xVal > 3000) dx = 1;

        if (yVal < 1000) dy = -1;
        else if (yVal > 3000) dy = 1;

        if (dx != 0) dy = 0;

        if (dx != 0 || dy != 0) {
            lastMove = now;
            moveCursor(dx, dy);
        }
    }

    delay(5);
}
