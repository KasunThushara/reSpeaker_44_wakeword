// Who else is talking on the display bus?
//
// The XIAO is the only master on GPIO5/6 in our code, yet scan results and
// write results disagree across identical probes. If the XVF3800/XMOS also
// drives this bus, we will see edges on SDA/SCL while the ESP32 drives nothing.
//
// Method: release both lines (input + pull-up), sample them in a tight loop for
// 300 ms and count transitions. A quiet bus shows 0 edges. Any other master
// shows traffic bursts, typically every few ms.

#define SDA 5
#define SCL 6

volatile uint32_t edgesSDA = 0, edgesSCL = 0, samples = 0;

void listenBus(const char *label, int ms) {
  pinMode(SDA, INPUT_PULLUP);
  pinMode(SCL, INPUT_PULLUP);
  delay(2);
  edgesSDA = edgesSCL = samples = 0;
  int lastSDA = digitalRead(SDA), lastSCL = digitalRead(SCL);
  uint32_t end = millis() + ms;
  while (millis() < end) {
    for (int i = 0; i < 400; i++) {          // small bursts so millis() stays honest
      int s = digitalRead(SDA), c = digitalRead(SCL);
      if (s != lastSDA) { edgesSDA++; lastSDA = s; }
      if (c != lastSCL) { edgesSCL++; lastSCL = c; }
      samples++;
    }
  }
  Serial.printf("%-18s %lums: %lu samples, SDA edges %lu, SCL edges %lu  -> %s\n",
                label, (unsigned long)ms, (unsigned long)samples,
                (unsigned long)edgesSDA, (unsigned long)edgesSCL,
                (edgesSDA + edgesSCL) > 20 ? "TRAFFIC (another master!)" : "quiet");
}

// same test on the free J5 pads, which have no other device on them
void listenFree(int ms) {
  pinMode(41, INPUT_PULLUP);
  pinMode(40, INPUT_PULLUP);
  delay(2);
  uint32_t e = 0, s = 0;
  int last = digitalRead(41);
  uint32_t end = millis() + ms;
  while (millis() < end) {
    for (int i = 0; i < 400; i++) {
      int v = digitalRead(41);
      if (v != last) { e++; last = v; }
      s++;
    }
  }
  Serial.printf("%-18s %lums: %lu samples, D14 edges %lu -> %s\n",
                "J5 free pads", (unsigned long)ms, (unsigned long)s, (unsigned long)e,
                e > 20 ? "TRAFFIC" : "quiet (expected)");
}

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0 < 3000)) delay(10);
  Serial.println();
  Serial.println(F("=== bus listener: GPIO5/6 (D4/D5) should be quiet if we are the only master ==="));
  Serial.println(F("(display and codec are slaves; any traffic here means the XMOS is driving)"));
  delay(300);
}

void loop() {
  static int round = 0;
  listenBus("control bus 5/6", 300);
  listenFree(300);
  listenBus("control bus 5/6", 300);
  Serial.printf("--- round %d done ---\n", ++round);
  delay(2000);
}
