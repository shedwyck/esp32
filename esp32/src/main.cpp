/*
 * ESP32 + GM861S — QR/Barcode Scanner with WebSocket Server
 * ==========================================================
 * Scans a QR code and immediately broadcasts the decoded string
 * to every connected WebSocket client on ws://<ESP32-IP>/ws
 *
 * GET  /ping → queues blink of all 5 LEDs (backend health check)
 * POST /led?led=0-4&blinks=N → queues blink of one agency LED
 *
 * LED → Agency mapping (matches AGENCY_LED_INDEX in admin.py):
 *   0 → SSS        GPIO 25
 *   1 → GSIS       GPIO 26
 *   2 → PhilHealth GPIO 27
 *   3 → Pag-IBIG   GPIO 32
 *   4 → COMELEC    GPIO 33
 *
 * Dependencies:
 *   - ESP Async WebServer  → https://github.com/ESP32Async/ESPAsyncWebServer
 *   - AsyncTCP             → https://github.com/ESP32Async/AsyncTCP
 *   - wyltek-embedded-builder (already in your project)
 *
 * Wiring (GM861S → ESP32):
 *   GM861S TX  → GPIO 16   (ESP32 Serial2 RX)
 *   GM861S RX  → GPIO 17   (ESP32 Serial2 TX)
 *   GM861S GND → GND
 *   GM861S VCC → 5V or 3.3V
 */

#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncWebSocket.h>
#include <sensors/WySensors.h>
#include <sensors/drivers/WyGM861S.h>

/* ── WiFi credentials ─────────────────────────────────────────────── */
static constexpr char WIFI_SSID[] = "rocky";
static constexpr char WIFI_PASS[] = "ayokonga";

/* ── Pin map ──────────────────────────────────────────────────────── */
static constexpr int8_t GM861S_TX_PIN = 17;
static constexpr int8_t GM861S_RX_PIN = 16;

/* ── LED pins — one per agency, index matches backend AGENCY_LED_INDEX */
static constexpr uint8_t LED_PINS[5] = { 25, 26, 27, 32, 33 };
static constexpr uint8_t LED_COUNT   = 5;

/* ── Agency names — index matches LED_PINS / AGENCY_LED_INDEX ─────── */
static const char* AGENCY_NAMES[LED_COUNT] = {
    "SSS", "GSIS", "PhilHealth", "Pag-IBIG", "COMELEC"
};

/* ── Blink job queue (set from HTTP handlers, consumed in loop) ────── */
struct BlinkJob {
    int8_t  ledIndex;   // -1 = empty slot, LED_COUNT = all LEDs
    uint8_t blinks;
};
static volatile BlinkJob blinkQueue[LED_COUNT];

/* ── Ping flag — blink all LEDs, set from /ping, consumed in loop ─── */
static volatile bool pingRequested = false;

/* ── Error flash flag ─────────────────────────────────────────────── */
static volatile bool flashError = false;

/* ── Server & WebSocket ───────────────────────────────────────────── */
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

/* ── Scanner ──────────────────────────────────────────────────────── */
WySensors  sensors;
WyGM861S*  scanner = nullptr;

/* ── WebSocket event handler ──────────────────────────────────────── */
void onWsEvent(AsyncWebSocket* server,
               AsyncWebSocketClient* client,
               AwsEventType type,
               void* arg,
               uint8_t* data,
               size_t len)
{
    switch (type) {
        case WS_EVT_CONNECT:
            Serial.printf("[ws] client #%u connected from %s\n",
                          client->id(),
                          client->remoteIP().toString().c_str());
            break;
        case WS_EVT_DISCONNECT:
            Serial.printf("[ws] client #%u disconnected\n", client->id());
            break;
        case WS_EVT_ERROR:
            Serial.printf("[ws] client #%u error(%u): %s\n",
                          client->id(), *((uint16_t*)arg), (char*)data);
            break;
        case WS_EVT_DATA:
            break;
        default:
            break;
    }
}

/* ── WiFi setup ───────────────────────────────────────────────────── */
void connectWifi() {
    Serial.printf("[wifi] connecting to %s", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print('.');
    }

    Serial.println();
    Serial.printf("[wifi] connected — IP: %s\n",
                  WiFi.localIP().toString().c_str());
    Serial.printf("[ws]   endpoint: ws://%s/ws\n",
                  WiFi.localIP().toString().c_str());
    Serial.printf("[led]  endpoint: http://%s/led\n",
                  WiFi.localIP().toString().c_str());
    Serial.printf("[ping] endpoint: http://%s/ping\n",
                  WiFi.localIP().toString().c_str());
}

/* ── setup ────────────────────────────────────────────────────────── */
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println();
    Serial.println(F("[boot] LAPAT QR Scanner — ESP32 + GM861S"));

    /* 1. LED pins */
    for (uint8_t i = 0; i < LED_COUNT; i++) {
        pinMode(LED_PINS[i], OUTPUT);
        digitalWrite(LED_PINS[i], LOW);
        blinkQueue[i].ledIndex = -1;
        blinkQueue[i].blinks   = 0;
    }

    /* 2. WiFi */
    connectWifi();

    /* Startup flash — all LEDs blink once to confirm wiring */
    for (uint8_t i = 0; i < LED_COUNT; i++) digitalWrite(LED_PINS[i], HIGH);
    delay(300);
    for (uint8_t i = 0; i < LED_COUNT; i++) digitalWrite(LED_PINS[i], LOW);
    Serial.println(F("[led] startup flash OK"));

    /* 3. WebSocket */
    ws.onEvent(onWsEvent);
    server.addHandler(&ws);

    /* Status page */
    server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(200, "text/plain", "LAPAT QR Scanner — WebSocket at /ws");
    });

    /* Ping — queues all-LED blink, responds immediately */
    server.on("/ping", HTTP_GET, [](AsyncWebServerRequest* req) {
        Serial.println("[ping] received");
        pingRequested = true;
        req->send(200, "text/plain", "pong");
    });

    /* LED — per-agency blink via query params ?led=0&blinks=3 */
    server.on("/led", HTTP_POST, [](AsyncWebServerRequest* req) {
        if (!req->hasParam("led") || !req->hasParam("blinks")) {
            Serial.println("[propagation] ERROR — missing params");
            flashError = true;
            req->send(400, "application/json", "{\"error\":\"missing led or blinks param\"}");
            return;
        }

        int ledIndex = req->getParam("led")->value().toInt();
        int blinks   = req->getParam("blinks")->value().toInt();

        Serial.printf("[led] params — led=%d blinks=%d\n", ledIndex, blinks);

        if (ledIndex < 0 || ledIndex >= (int)LED_COUNT || blinks <= 0) {
            Serial.printf("[propagation] ERROR — invalid values: led=%d blinks=%d\n",
                          ledIndex, blinks);
            flashError = true;
            req->send(400, "application/json", "{\"error\":\"invalid led or blinks\"}");
            return;
        }

        blinkQueue[ledIndex].ledIndex = (int8_t)ledIndex;
        blinkQueue[ledIndex].blinks   = (uint8_t)blinks;

        Serial.printf("[propagation] queued → %s (%d blinks)\n",
                      AGENCY_NAMES[ledIndex], blinks);
        req->send(200, "application/json", "{\"ok\":true}");
    });

    /* CORS preflight */
    server.on("/led", HTTP_OPTIONS, [](AsyncWebServerRequest* req) {
        AsyncWebServerResponse* res = req->beginResponse(204);
        res->addHeader("Access-Control-Allow-Origin",  "*");
        res->addHeader("Access-Control-Allow-Methods", "POST, OPTIONS");
        res->addHeader("Access-Control-Allow-Headers", "Content-Type");
        req->send(res);
    });

    server.begin();
    Serial.println(F("[http] server started"));

    /* 4. Scanner */
    Serial2.setRxBufferSize(1024);
    scanner = sensors.addUART<WyGM861S>("barcode",
                                        GM861S_TX_PIN,
                                        GM861S_RX_PIN,
                                        /*baud=*/9600,
                                        /*port=*/2);
    sensors.begin();
    sensors.list();

    Serial.println(F("[ready] point the scanner at a QR code."));
}

/* ── loop ─────────────────────────────────────────────────────────── */
void loop() {
    ws.cleanupClients();

    /* Ping — blink all LEDs 3x */
    if (pingRequested) {
        pingRequested = false;
        Serial.println("[ping] blinking all LEDs");
        for (uint8_t b = 0; b < 3; b++) {
            for (uint8_t i = 0; i < LED_COUNT; i++) digitalWrite(LED_PINS[i], HIGH);
            delay(200);
            for (uint8_t i = 0; i < LED_COUNT; i++) digitalWrite(LED_PINS[i], LOW);
            delay(200);
        }
        Serial.println("[ping] done");
    }

    /* Error flash — all LEDs rapid blink 3x */
    if (flashError) {
        flashError = false;
        Serial.println("[propagation] ERROR flash");
        for (uint8_t f = 0; f < 3; f++) {
            for (uint8_t i = 0; i < LED_COUNT; i++) digitalWrite(LED_PINS[i], HIGH);
            delay(100);
            for (uint8_t i = 0; i < LED_COUNT; i++) digitalWrite(LED_PINS[i], LOW);
            delay(100);
        }
    }

    /* Process pending per-agency blink jobs */
    for (uint8_t i = 0; i < LED_COUNT; i++) {
        if (blinkQueue[i].ledIndex >= 0) {
            uint8_t pin    = LED_PINS[blinkQueue[i].ledIndex];
            uint8_t blinks = blinkQueue[i].blinks;
            blinkQueue[i].ledIndex = -1;

            Serial.printf("[propagation] confirmed → %s\n", AGENCY_NAMES[i]);
            for (uint8_t b = 0; b < blinks; b++) {
                digitalWrite(pin, HIGH);
                delay(200);
                digitalWrite(pin, LOW);
                delay(200);
            }
            Serial.printf("[propagation] LED done → %s\n", AGENCY_NAMES[i]);
        }
    }

    /* Non-blocking scanner poll */
    if (scanner && scanner->available()) {
        const char* code = scanner->lastBarcode();

        Serial.printf("[scan] %s   (count=%lu, clients=%u)\n",
                      code,
                      (unsigned long)scanner->scanCount(),
                      ws.count());

        ws.textAll(code);
        scanner->clear();
    }

    delay(10);
}