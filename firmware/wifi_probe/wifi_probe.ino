#include <WiFi.h>
#include <WiFiUdp.h>

// wifi_probe v2 - direction test.
//
// v1 result: the PC receives every packet we send (brain log shows
// "<- 192.168.7.102 JARVIS 00xx 0"), but no ACK ever comes back. So
// XIAO -> PC works and PC -> XIAO does not.
//
// v2 is non-blocking and prints EVERY datagram it receives, with its source,
// so we can test the PC -> XIAO direction without any demo code involved.
//
// Also expose a tiny echo: any packet we receive is answered with "PONG <text>"
// back to its sender.

static const char *SSID = "";
static const char *PASS = "<your-password>";
static const char *PC_IP = "192.168.7.217";
static const uint16_t PORT = 9001;
static const uint16_t LOCAL_PORT = 9002;

WiFiUDP udp;
uint32_t seq = 0;
uint32_t rxCount = 0;
uint32_t lastSend = 0;
uint32_t lastBeat = 0;

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0 < 3000)) delay(10);

  Serial.println();
  Serial.println(F("=== wifi_probe v2: bidirectional UDP test ==="));

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(SSID, PASS);
  Serial.printf("connecting to %s", SSID);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(400);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("FAILED to join (status %d)\n", (int)WiFi.status());
    return;
  }

  Serial.printf("connected: ip=%s gw=%s rssi=%d ch=%d\n",
                WiFi.localIP().toString().c_str(),
                WiFi.gatewayIP().toString().c_str(),
                WiFi.RSSI(), WiFi.channel());

  udp.begin(LOCAL_PORT);
  Serial.printf("listening on UDP %u, sending to %s:%u every 3 s\n",
                LOCAL_PORT, PC_IP, PORT);
  Serial.println(F("(a packet from the PC should print as   << RX ...)"));
}

void loop() {
  // 1. drain everything we receive, always
  for (int guard = 0; guard < 4; guard++) {
    int n = udp.parsePacket();
    if (n <= 0) break;
    uint8_t buf[96];
    int len = udp.read(buf, sizeof(buf) - 1);
    if (len < 0) len = 0;
    buf[len] = 0;
    rxCount++;
    Serial.printf("  << RX from %s:%u  len=%d  %s\n",
                  udp.remoteIP().toString().c_str(), udp.remotePort(), len, (char *)buf);

    // echo back so the PC side can prove the path too
    udp.beginPacket(udp.remoteIP(), udp.remotePort());
    udp.printf("PONG %s", (char *)buf);
    udp.endPacket();
  }

  if (WiFi.status() != WL_CONNECTED) {
    static uint32_t lastRetry = 0;
    if (millis() - lastRetry > 5000) {
      lastRetry = millis();
      Serial.println(F("wifi dropped - reconnecting"));
      WiFi.reconnect();
    }
    return;
  }

  // 2. send one probe every 3 s (no blocking wait - loop prints the reply)
  if (millis() - lastSend > 3000) {
    lastSend = millis();
    seq++;
    char msg[40];
    snprintf(msg, sizeof(msg), "JARVIS %04lu 0\n", (unsigned long)seq);
    udp.beginPacket(IPAddress(192, 168, 7, 217), PORT);
    udp.write((const uint8_t *)msg, strlen(msg));
    udp.endPacket();
    Serial.printf("  >> TX %s", msg);
  }

  // 3. heartbeat
  if (millis() - lastBeat > 5000) {
    lastBeat = millis();
    Serial.printf("[%5lus] up, rx so far: %lu\n",
                  (unsigned long)(millis() / 1000), (unsigned long)rxCount);
  }
  delay(5);
}
