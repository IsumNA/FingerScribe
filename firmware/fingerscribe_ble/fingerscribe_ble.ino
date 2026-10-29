/*
 * FingerScribe — XIAO nRF52840 Sense firmware v9: Bluetooth + hold-to-talk mic + double-tap + battery + SURFACE VIBRATION
 *
 * v8: the motion sensor's accelerometer now also runs fast (833 readings/s) into its own memory (FIFO). Every 10 ms the
 *     firmware measures how much fast vibration there was: the fingertip sliding on a surface buzzes, a finger in the air
 *     doesn't. Sent as byte 16 of every motion packet (0x01 packets are now 17 bytes) and as the last field of the USB D line.
 *     The always-on microphone from v7 is switched off by default (MIC_CONTACT 0): it read 0 and slowed sampling.
 *
 * v7: the microphone now runs all the time and measures the scratchy friction sound of the fingertip sliding on a
 *     surface (high-pitched energy, every 10 ms). Sliding on a desk, a leg or glass makes it; a finger moving in the air
 *     doesn't. Sent as one extra byte on every motion packet (0x01 packets are now 16 bytes) and as the 'press' field on
 *     the USB D line. No new hardware: it is the same mic used for voice notes.
 *
 * v6.1: v6 only managed ~36 samples/s (BLE.poll() every loop). Now polls Bluetooth every 20 ms and
 *       prints E,RATE_HZ,<n> every 10 s on USB so you can check it is ~100.
 *
 * New in v6
 *   - Push button between pin D1 and GND (no resistor needed). Hold it to talk:
 *     the board's own microphone streams to the phone while the button is held.
 *     Audio: 8 kHz, IMA-ADPCM (4 bits per sample), packet type 0x03 below.
 *
 * What it does
 *   - Reads the board's own motion sensor (LSM6DS3TR-C) 100 times a second.
 *   - Streams it to the phone over Bluetooth (Nordic UART service, name "FingerScribe").
 *   - Detects a deliberate double-tap of the fingertip in the sensor's own hardware
 *     and sends a TAP event (the phone uses it to start / stop a voice note).
 *   - Sends the battery voltage every 10 s.
 *   - Still prints the same USB lines as v4 (D,... / E,...) so capture_server.py keeps working.
 *
 * Works with either Seeed board package:
 *   - "Seeed nRF52 Boards"               -> uses the built-in Bluefruit library (nothing to install)
 *   - "Seeed nRF52 mbed-enabled Boards"  -> needs the ArduinoBLE library (Library Manager)
 *
 * Bluetooth packets (TX characteristic 6E400003-..., little-endian):
 *   0x01  t_ms(u16)  gx gy gz ax ay az (6 x i16, raw) contact(u8) vib(u8)   17 bytes, one per sample
 *         gyro: 0.0175 deg/s per unit, accel: 0.000122 g per unit
 *   0x02  event(u8)  [value(u16)]
 *         event 1 = double tap, 2 = stroke start, 3 = stroke end (value = ms), 4 = battery (value = mV),
 *               5 = talk start (button pressed), 6 = talk end (button released)
 *   0x03  seq(u8) predictor(i16) index(u8) + 15 bytes ADPCM = 30 samples at 8 kHz, 20 bytes.
 *         Each packet carries its own decoder state, so a lost packet costs only 3.75 ms of sound.
 *         Low nibble first. Motion packets pause while talking, to leave room for the audio.
 * Commands the phone can write (RX characteristic 6E400002-...):
 *   'T' n   set double-tap threshold to n (1..31, 1 unit = 0.125 g, default 16 = 2 g)
 *   'S' 0/1 stop / start streaming motion data (events always flow)
 */

#include <Arduino.h>
#include <PDM.h>
#if defined(ARDUINO_ARCH_MBED)
  #include <ArduinoBLE.h>
#else
  #include <bluefruit.h>
#endif

// ======================= internal I2C bus, driven directly =======================
const uint8_t SDA_PIN = 7;    // P0.07
const uint8_t SCL_PIN = 27;   // P0.27

inline void sdaLow()  { NRF_P0->OUTCLR = (1UL << SDA_PIN); }
inline void sdaHigh() { NRF_P0->OUTSET = (1UL << SDA_PIN); }
inline void sclLow()  { NRF_P0->OUTCLR = (1UL << SCL_PIN); }
inline void sclHigh() { NRF_P0->OUTSET = (1UL << SCL_PIN); }
inline bool sdaRead() { return (NRF_P0->IN >> SDA_PIN) & 1UL; }
// v8.2: delayMicroseconds(1) really waited ~18 us here, so one sensor read took 5.6 ms. Wait on the CPU's cycle counter instead.
inline void dly()     { uint32_t s = DWT->CYCCNT; while (DWT->CYCCNT - s < 96) {} }   // 1.5 us at 64 MHz: ~300 kHz I2C
void cycleCounterOn() { CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk; DWT->CYCCNT = 0; DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk; }

void openDrain(uint8_t pin) {
  NRF_P0->PIN_CNF[pin] = (GPIO_PIN_CNF_DIR_Output    << GPIO_PIN_CNF_DIR_Pos)
                       | (GPIO_PIN_CNF_INPUT_Connect << GPIO_PIN_CNF_INPUT_Pos)
                       | (GPIO_PIN_CNF_PULL_Pullup   << GPIO_PIN_CNF_PULL_Pos)
                       | (GPIO_PIN_CNF_DRIVE_S0D1    << GPIO_PIN_CNF_DRIVE_Pos);
  NRF_P0->OUTSET = (1UL << pin);
}

void i2cStart() { sdaHigh(); sclHigh(); dly(); sdaLow(); dly(); sclLow(); dly(); }
void i2cStop()  { sdaLow(); dly(); sclHigh(); dly(); sdaHigh(); dly(); }

bool i2cWrite(uint8_t b) {
  for (uint8_t i = 0; i < 8; i++) {
    if (b & 0x80) sdaHigh(); else sdaLow();
    b <<= 1; dly(); sclHigh(); dly(); sclLow();
  }
  sdaHigh(); dly(); sclHigh(); dly();
  bool ack = !sdaRead();
  sclLow(); dly();
  return ack;
}

uint8_t i2cRead(bool ack) {
  uint8_t b = 0;
  sdaHigh();
  for (uint8_t i = 0; i < 8; i++) {
    b <<= 1; dly(); sclHigh(); dly();
    if (sdaRead()) b |= 1;
    sclLow();
  }
  if (ack) sdaLow(); else sdaHigh();
  dly(); sclHigh(); dly(); sclLow(); sdaHigh(); dly();
  return b;
}

bool regWrite(uint8_t addr, uint8_t reg, uint8_t val) {
  i2cStart();
  bool ok = i2cWrite(addr << 1) && i2cWrite(reg) && i2cWrite(val);
  i2cStop();
  return ok;
}

bool regRead(uint8_t addr, uint8_t reg, uint8_t *buf, uint8_t n) {
  i2cStart();
  if (!i2cWrite(addr << 1) || !i2cWrite(reg)) { i2cStop(); return false; }
  i2cStart();
  if (!i2cWrite((addr << 1) | 1)) { i2cStop(); return false; }
  for (uint8_t i = 0; i < n; i++) buf[i] = i2cRead(i < n - 1);
  i2cStop();
  return true;
}

// ======================= motion sensor =======================
uint8_t imuAddr = 0;
uint8_t tapThreshold = 4;    // x 0.125 g = 0.5 g. v8.3: catches each fingertip landing (single taps), not just deliberate double taps

void setTapThreshold(uint8_t t) {
  if (t < 1) t = 1; if (t > 31) t = 31;
  tapThreshold = t;
  regWrite(imuAddr, 0x59, t & 0x1F);          // TAP_THS_6D
}

bool imuInit() {
  cycleCounterOn();
  NRF_P1->PIN_CNF[8] = (GPIO_PIN_CNF_DIR_Output        << GPIO_PIN_CNF_DIR_Pos)
                     | (GPIO_PIN_CNF_INPUT_Disconnect  << GPIO_PIN_CNF_INPUT_Pos)
                     | (GPIO_PIN_CNF_PULL_Disabled     << GPIO_PIN_CNF_PULL_Pos)
                     | (GPIO_PIN_CNF_DRIVE_H0H1        << GPIO_PIN_CNF_DRIVE_Pos);
  NRF_P1->OUTSET = (1UL << 8);                // power the sensor
  delay(100);
  openDrain(SDA_PIN); openDrain(SCL_PIN); delay(5);

  const uint8_t addrs[2] = {0x6A, 0x6B};
  for (uint8_t i = 0; i < 2; i++) {
    uint8_t who = 0;
    if (regRead(addrs[i], 0x0F, &who, 1) && (who == 0x6A || who == 0x69)) { imuAddr = addrs[i]; break; }
  }
  if (!imuAddr) return false;

  regWrite(imuAddr, 0x12, 0x44);   // CTRL3_C: block data update + auto-increment
  regWrite(imuAddr, 0x10, 0x78);   // CTRL1_XL: accel 833 Hz, +-4 g (fast, for surface vibration; main loop still reads it at 100 Hz)
  regWrite(imuAddr, 0x0A, 0x00);   // FIFO_CTRL5: FIFO off (v8.1 polls the fast accelerometer instead)
  regWrite(imuAddr, 0x11, 0x44);   // CTRL2_G:  gyro 104 Hz, +-500 deg/s
  // Double-tap detection, done inside the sensor
  regWrite(imuAddr, 0x58, 0x8F);   // TAP_CFG: enable, tap on X/Y/Z, latch until read
  setTapThreshold(tapThreshold);   // TAP_THS_6D
  regWrite(imuAddr, 0x5A, 0x05);   // INT_DUR2: shock ~10 ms, quiet ~5 ms at 833 Hz: short taps, close together
  regWrite(imuAddr, 0x5B, 0x00);   // WAKE_UP_THS: single tap only
  delay(50);
  return true;
}

bool readRaw(int16_t r[6]) {
  uint8_t b[12];
  if (!regRead(imuAddr, 0x22, b, 12)) return false;      // gyro x,y,z then accel x,y,z
  for (uint8_t i = 0; i < 6; i++) r[i] = (int16_t)((b[2 * i + 1] << 8) | b[2 * i]);
  return true;
}

bool tapped() {             // v8.3: any single tap (fingertip landing) since the last read
  uint8_t s = 0;
  if (!regRead(imuAddr, 0x1C, &s, 1)) return false;      // TAP_SRC (reading clears the latch)
  return s & 0x20;                                       // SINGLE_TAP
}

// ======================= surface vibration (fast accelerometer, polled) =======================
// v8.1: the FIFO version read 0, so this no longer uses the FIFO. The accelerometer runs at 833 readings/s; between the
// 100 Hz samples the loop checks it every ~0.6 ms and picks up each new reading. vib = energy of the reading-to-reading
// change (a high-pass at 833 Hz): sliding on a surface makes it jump, moving in the air barely does.
int16_t vibLast[3] = {0, 0, 0}; bool vibHave = false;
uint64_t vibSum = 0; uint16_t vibN = 0; int32_t vibPeak = 0; uint8_t vibCount = 0; uint32_t vibReadings = 0, vibNextPoll = 0; uint8_t vibLastOut = 0;   // vibReadings: diagnostics
void pollVibration() {
  uint32_t t = micros();
  if ((int32_t)(t - vibNextPoll) < 0) return;
  vibNextPoll = t + 600;
  uint8_t st = 0;
  if (!regRead(imuAddr, 0x1E, &st, 1) || !(st & 0x01)) return;      // STATUS_REG: XLDA = new accelerometer reading
  uint8_t b[6];
  if (!regRead(imuAddr, 0x28, b, 6)) return;                          // OUTX_L_XL .. OUTZ_H_XL
  int16_t v[3] = {(int16_t)(b[0] | (b[1] << 8)), (int16_t)(b[2] | (b[3] << 8)), (int16_t)(b[4] | (b[5] << 8))};
  if (vibHave) { for (uint8_t c = 0; c < 3; c++) { int32_t d = v[c] - vibLast[c]; vibSum += (uint64_t)((int64_t)d * d); if (abs(d) > vibPeak) vibPeak = abs(d); } vibN++; }
  if (vibCount < 255) vibCount++;
  for (uint8_t c = 0; c < 3; c++) vibLast[c] = v[c];
  vibHave = true; vibReadings++;
}
uint8_t readVibration() {               // energy since the last 100 Hz sample, 0..255 (log scale)
  if (!vibN) return 0;
  float rms = sqrtf((float)vibSum / vibN);                            // raw units, 0.122 mg each
  vibSum = 0; vibN = 0;
  float out = 16.0f * log2f(1.0f + rms);
  vibLastOut = out > 255 ? 255 : (uint8_t)out;
  return vibLastOut;
}

// ======================= battery =======================
uint16_t batteryMv() {
#if defined(PIN_VBAT)
  #if defined(VBAT_ENABLE)
    pinMode(VBAT_ENABLE, OUTPUT); digitalWrite(VBAT_ENABLE, LOW);   // keep LOW: protects the pin
  #endif
  analogReadResolution(12);
  uint32_t raw = analogRead(PIN_VBAT);
  #if defined(ARDUINO_ARCH_MBED)
    const float ref = 3.3f;
  #else
    const float ref = 3.6f;
  #endif
  return (uint16_t)(raw * ref / 4095.0f * (1510.0f / 510.0f) * 1000.0f);   // 1M + 510k divider (approx.)
#else
  return 0;
#endif
}

// ======================= Bluetooth =======================
#define NUS_SERVICE "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX      "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_TX      "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"
bool streamMotion = true;

void storeStartDump(); void storeWipe();
void handleCommand(const uint8_t *d, int n) {
  if (n >= 1 && d[0] == 'D') storeStartDump();      // v9: send what was stored while no phone was connected
  if (n >= 1 && d[0] == 'X') storeWipe();           // v9: the app saved it, wipe the flash
  if (n >= 2 && d[0] == 'T') setTapThreshold(d[1]);
  if (n >= 2 && d[0] == 'S') streamMotion = d[1] != 0;
}

#if defined(ARDUINO_ARCH_MBED)
BLEService nus(NUS_SERVICE);
BLECharacteristic txChar(NUS_TX, BLENotify, 20);
BLECharacteristic rxChar(NUS_RX, BLEWrite | BLEWriteWithoutResponse, 20);

void bleBegin() {
  if (!BLE.begin()) { Serial.println("E,BLE_FAIL"); return; }
  BLE.setLocalName("FingerScribe"); BLE.setDeviceName("FingerScribe");
  BLE.setConnectionInterval(6, 12);          // 7.5-15 ms
  nus.addCharacteristic(txChar); nus.addCharacteristic(rxChar);
  BLE.addService(nus); BLE.setAdvertisedService(nus);
  BLE.advertise();
}
// BLE.poll() is slow on this core: calling it every loop dragged sampling down to ~36 Hz (v6).
// Poll at most every 20 ms and reuse the answer in between.
bool bleReady() {
  static uint32_t lastPoll = 0; static bool ok = false;
  uint32_t ms = millis();
  if (ms - lastPoll >= 20) { lastPoll = ms; BLE.poll(); ok = BLE.connected() && txChar.subscribed(); }
  return ok;
}
void bleSend(const uint8_t *d, uint8_t n) { txChar.writeValue(d, n); }
void bleReceive() {
  if (rxChar.written()) handleCommand(rxChar.value(), rxChar.valueLength());
}
#else
BLEUart bleuart;
void onRx(uint16_t) {
  uint8_t buf[20]; int n = 0;
  while (bleuart.available() && n < 20) buf[n++] = bleuart.read();
  handleCommand(buf, n);
}
void bleBegin() {
  Bluefruit.configPrphBandwidth(BANDWIDTH_MAX);
  Bluefruit.begin();
  Bluefruit.setTxPower(0);
  Bluefruit.setName("FingerScribe");
  Bluefruit.Periph.setConnInterval(6, 12);   // 7.5-15 ms
  bleuart.begin();
  bleuart.setRxCallback(onRx);
  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.addTxPower();
  Bluefruit.Advertising.addService(bleuart);
  Bluefruit.ScanResponse.addName();
  Bluefruit.Advertising.restartOnDisconnect(true);
  Bluefruit.Advertising.setInterval(160, 480);   // 100-300 ms
  Bluefruit.Advertising.start(0);
}
bool bleReady() { return Bluefruit.connected() && bleuart.notifyEnabled(); }
void bleSend(const uint8_t *d, uint8_t n) { bleuart.write(d, n); }
void bleReceive() {}
#endif

void sendEvent(uint8_t ev, uint16_t val) {
  uint8_t p[4] = {0x02, ev, (uint8_t)(val & 0xFF), (uint8_t)(val >> 8)};
  if (bleReady()) bleSend(p, 4);
}

// ======================= hold-to-talk: button + microphone =======================
const uint8_t BUTTON_PIN = D1;          // button between D1 and GND
const uint16_t HOLD_MS = 150;           // ignore brushes shorter than this

// IMA-ADPCM encoder (standard tables)
const int16_t stepTable[89] = {7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,
  107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,1060,1166,1282,
  1411,1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,10442,
  11487,12635,13899,15289,16818,18500,20350,22385,24623,27086,29794,32767};
const int8_t indexTable[16] = {-1,-1,-1,-1,2,4,6,8,-1,-1,-1,-1,2,4,6,8};
int16_t adpcmPred = 0; int8_t adpcmIndex = 0;

uint8_t adpcmEncode(int16_t sample) {
  int32_t diff = (int32_t)sample - adpcmPred;
  uint8_t code = 0;
  if (diff < 0) { code = 8; diff = -diff; }
  int32_t step = stepTable[adpcmIndex], delta = step >> 3;
  if (diff >= step) { code |= 4; diff -= step; delta += step; } step >>= 1;
  if (diff >= step) { code |= 2; diff -= step; delta += step; } step >>= 1;
  if (diff >= step) { code |= 1; delta += step; }
  int32_t p = adpcmPred + ((code & 8) ? -delta : delta);
  if (p > 32767) p = 32767; if (p < -32768) p = -32768;
  adpcmPred = (int16_t)p;
  adpcmIndex += indexTable[code]; if (adpcmIndex < 0) adpcmIndex = 0; if (adpcmIndex > 88) adpcmIndex = 88;
  return code;
}

volatile int pdmBytes = 0;
int16_t pdmBuf[256];
void onPdm() { pdmBytes = PDM.available(); }

bool talking = false, micOn = false;
uint32_t pressedAt = 0;
uint8_t audioSeq = 0;
int16_t pending[30]; uint8_t nPending = 0;
int32_t halfSum = 0; bool halfHave = false;   // 16 kHz -> 8 kHz by averaging pairs
// contact sound: energy of the sample-to-sample change (a simple high-pass) at 16 kHz, summed between motion samples
int16_t lastPdm = 0; uint64_t hfSum = 0; uint32_t hfCount = 0;
uint32_t micSamples = 0; int16_t micPeak = 0;          // diagnostics, printed every 10 s
uint8_t contactByte() {
  float rms = hfCount ? sqrtf((float)hfSum / hfCount) : 0; hfSum = 0; hfCount = 0;
  float v = 16.0f * log2f(1.0f + rms);                 // log scale: ~0 silent, ~100-200 loud scratching
  return v > 255 ? 255 : (uint8_t)v;
}

bool micStart() {
#if defined(PIN_PDM_PWR)
  pinMode(PIN_PDM_PWR, OUTPUT); digitalWrite(PIN_PDM_PWR, HIGH);
#endif
  PDM.onReceive(onPdm);
  PDM.setBufferSize(512);
  if (!PDM.begin(1, 16000)) { Serial.println("E,MIC_FAIL"); return false; }
  PDM.setGain(30);
  adpcmPred = 0; adpcmIndex = 0; nPending = 0; halfHave = false;
  return true;
}

void micStop() {
  PDM.end();
#if defined(PIN_PDM_PWR)
  digitalWrite(PIN_PDM_PWR, LOW);
#endif
}

void sendAudioPacket() {
  uint8_t p[20];
  p[0] = 0x03; p[1] = audioSeq++;
  p[2] = adpcmPred & 0xFF; p[3] = (adpcmPred >> 8) & 0xFF; p[4] = adpcmIndex;   // state BEFORE these samples
  for (uint8_t i = 0; i < 15; i++) {
    uint8_t lo = adpcmEncode(pending[2 * i]), hi = adpcmEncode(pending[2 * i + 1]);
    p[5 + i] = lo | (hi << 4);
  }
  bleSend(p, 20);
}

void pumpAudio(bool ready) {
  if (!micOn || pdmBytes == 0) return;
  int avail = pdmBytes;
  if (avail > (int)sizeof(pdmBuf)) avail = sizeof(pdmBuf);
  int n = PDM.read(pdmBuf, avail);
  pdmBytes = 0;
  for (int i = 0; i < n / 2; i++) { int32_t d = (int32_t)pdmBuf[i] - lastPdm; lastPdm = pdmBuf[i]; hfSum += (uint64_t)(d * d); hfCount++; if (abs(pdmBuf[i]) > micPeak) micPeak = abs(pdmBuf[i]); }
  micSamples += n / 2;
  if (!talking) return;                          // only stream sound while the button is held
  for (int i = 0; i < n / 2; i++) {
    if (!halfHave) { halfSum = pdmBuf[i]; halfHave = true; continue; }
    pending[nPending++] = (int16_t)((halfSum + pdmBuf[i]) / 2); halfHave = false;
    if (nPending == 30) { if (ready) sendAudioPacket(); nPending = 0; }
  }
}

void checkButton(bool ready, uint32_t now) {
  bool down = digitalRead(BUTTON_PIN) == LOW;
  if (down && pressedAt == 0) pressedAt = now;
  if (!down) pressedAt = 0;
  if (down && !talking && now - pressedAt >= HOLD_MS) {
    talking = true;
    if (!micOn) micOn = micStart();
    adpcmPred = 0; adpcmIndex = 0; nPending = 0; halfHave = false;
    sendEvent(5, 0);
    Serial.print("E,TALK_START,"); Serial.println(now);
  }
  if (!down && talking) {
    talking = false;
#if !MIC_CONTACT
    if (micOn) { micStop(); micOn = false; }
#endif
    sendEvent(6, 0);
    Serial.print("E,TALK_END,"); Serial.println(now);
  }
}

// ======================= writing detector (same as v4) =======================
const uint32_t SAMPLE_US     = 10000;   // 100 Hz
const uint8_t  WIN           = 20;
const float    BUSY_ON       = 25.0f;
const float    BUSY_OFF      = 12.0f;
const uint16_t END_HOLD_MS   = 300;
const uint16_t MIN_STROKE_MS = 80;

float gwin[WIN];
uint8_t gi = 0;
bool writing = false;
uint32_t strokeStart = 0, lastBusy = 0, nextSample = 0, nextBattery = 0, samplesSinceRate = 0;
uint32_t usRead = 0, usVib = 0, usBle = 0, usRest = 0, usSerial = 0, usN = 0;   // timing diagnostics

float busyness(float gmag) {
  gwin[gi] = gmag; gi = (gi + 1) % WIN;
  float m = 0; for (uint8_t i = 0; i < WIN; i++) m += gwin[i]; m /= WIN;
  float v = 0; for (uint8_t i = 0; i < WIN; i++) { float d = gwin[i] - m; v += d * d; }
  return sqrtf(v / WIN);
}

// ======================= v9: pocket mode — store writing in the sleeve's own 2 MB flash =======================
// When no phone is connected, everything written (plus 1 s before and 1.5 s after each burst) is saved to the XIAO's
// QSPI flash chip (P25Q16H, 2 MB). When the app connects it is told how much is stored ('event 7'), asks for it ('D'),
// receives it as 0x04 packets, and tells the sleeve to wipe it ('X') once it has saved the notes.
// Page = 256 bytes: 16-byte header + 15 samples x 16 bytes. 2 MB = 8192 pages = about 20 minutes of actual writing.
const uint8_t Q_SCK = 21, Q_CSN = 25, Q_IO0 = 20, Q_IO1 = 24, Q_IO2 = 22, Q_IO3 = 23;   // XIAO nRF52840 QSPI pins (port 0)
const uint32_t PAGES = 8192, PAGE = 256, SPP = 15;                                       // samples per page
bool qOk = false;
static uint32_t qbuf[64] __attribute__((aligned(4)));                                   // one page, word aligned for EasyDMA
void qWait() { uint32_t t = millis(); while (!NRF_QSPI->EVENTS_READY && millis() - t < 200) {} NRF_QSPI->EVENTS_READY = 0; }
uint8_t qStatus() {                                       // read the flash status register (opcode 0x05)
  NRF_QSPI->CINSTRCONF = 0x05 | (2 << 8) | (1 << 12) | (1 << 13) | (0 << 15);   // opcode, length 2 bytes, keep IO2/IO3 high
  qWait(); return NRF_QSPI->CINSTRDAT0 & 0xFF;
}
void qBusyWait() { uint32_t t = millis(); while ((qStatus() & 1) && millis() - t < 400) {} }
bool qInit() {
  NRF_QSPI->PSEL.SCK = Q_SCK; NRF_QSPI->PSEL.CSN = Q_CSN; NRF_QSPI->PSEL.IO0 = Q_IO0; NRF_QSPI->PSEL.IO1 = Q_IO1; NRF_QSPI->PSEL.IO2 = Q_IO2; NRF_QSPI->PSEL.IO3 = Q_IO3;
  NRF_QSPI->IFCONFIG0 = 0;                                // FASTREAD + PP (single line, simplest and robust), 24-bit address
  NRF_QSPI->IFCONFIG1 = (1 << 0) | (3 << 28);             // SCK delay 1, SCK = 32 MHz / (3 + 1) = 8 MHz, mode 0
  NRF_QSPI->ENABLE = 1;
  NRF_QSPI->EVENTS_READY = 0; NRF_QSPI->TASKS_ACTIVATE = 1; qWait();
  NRF_QSPI->CINSTRCONF = 0x9F | (4 << 8) | (1 << 12) | (1 << 13);   // JEDEC ID: 3 bytes back
  qWait(); uint32_t id = NRF_QSPI->CINSTRDAT0 & 0xFFFFFF;
  Serial.print("E,FLASH_ID,"); Serial.println(id, HEX);
  return id != 0 && id != 0xFFFFFF;
}
void qRead(uint32_t addr, void *dst, uint32_t n) { NRF_QSPI->READ.SRC = addr; NRF_QSPI->READ.DST = (uint32_t)dst; NRF_QSPI->READ.CNT = n; NRF_QSPI->EVENTS_READY = 0; NRF_QSPI->TASKS_READSTART = 1; qWait(); }
void qWrite(uint32_t addr, const void *src, uint32_t n) { NRF_QSPI->WRITE.DST = addr; NRF_QSPI->WRITE.SRC = (uint32_t)src; NRF_QSPI->WRITE.CNT = n; NRF_QSPI->EVENTS_READY = 0; NRF_QSPI->TASKS_WRITESTART = 1; qWait(); qBusyWait(); }
void qErase4k(uint32_t addr) { NRF_QSPI->ERASE.PTR = addr; NRF_QSPI->ERASE.LEN = 0; NRF_QSPI->EVENTS_READY = 0; NRF_QSPI->TASKS_ERASESTART = 1; qWait(); qBusyWait(); }

struct __attribute__((packed)) PageHdr { uint16_t magic; uint8_t ver, n; uint32_t seq, t0, sess; };   // 16 bytes
struct __attribute__((packed)) Rec { uint16_t dt; int16_t r[6]; uint8_t vib, tap; };                  // 16 bytes
void storeAdd(uint32_t t, const Rec &rc);   // explicit prototype: stops the Arduino IDE generating one above Rec
uint32_t wPage = 0, nStored = 0, nextSeq = 1, eraseFrom = 0, eraseLeft = 0;
uint8_t pg[PAGE] __attribute__((aligned(4))); uint8_t pgN = 0; uint32_t sessId = 0, lastRecMs = 0;
Rec preRoll[100]; uint32_t preT[100]; uint8_t preHead = 0, preCount = 0;

void storeScan() {                                        // on boot: find where the stored writing ends
  uint32_t best = 0, bestPage = 0; nStored = 0; PageHdr h;
  for (uint32_t p = 0; p < PAGES; p++) {
    qRead(p * PAGE, qbuf, 16); memcpy(&h, qbuf, 16);
    if (h.magic == 0x5346) { nStored++; if (h.seq >= best) { best = h.seq; bestPage = p; } }
  }
  nextSeq = best + 1; wPage = nStored ? (bestPage + 1) % PAGES : 0;
  Serial.print("E,STORED_PAGES,"); Serial.println(nStored);
}
void storeFlushPage() {
  if (!pgN) return;
  if (nStored >= PAGES - 16) { pgN = 0; return; }         // full: stop storing (never overwrite unsynced writing)
  if ((wPage % 16) == 0) qErase4k(wPage * PAGE);          // first page of a 4 KB sector: erase the sector
  PageHdr *h = (PageHdr *)pg; h->magic = 0x5346; h->ver = 1; h->n = pgN; h->seq = nextSeq++; h->sess = sessId;
  qWrite(wPage * PAGE, pg, PAGE);
  wPage = (wPage + 1) % PAGES; nStored++; pgN = 0;
}
void storeAdd(uint32_t t, const Rec &rc) {
  PageHdr *h = (PageHdr *)pg;
  if (pgN && (t - h->t0 > 60000 || t < h->t0)) storeFlushPage();
  if (!pgN) { memset(pg, 0xFF, PAGE); h->t0 = t; }
  Rec x = rc; x.dt = (uint16_t)(t - h->t0); memcpy(pg + 16 + pgN * 16, &x, 16);
  if (++pgN == SPP) storeFlushPage();
}
// called every sample (100 Hz). Records while no phone is connected and the finger is writing (or just was).
void storeSample(uint32_t now, const int16_t r[6], uint8_t vib, uint8_t tap, bool phone, bool isWriting) {
  if (!qOk) return;
  Rec rc; rc.dt = 0; memcpy(rc.r, r, 12); rc.vib = vib; rc.tap = tap;
  bool rec = !phone && (isWriting || now - lastBusy < 1500);
  if (!rec) {                                              // not recording: keep the last second in RAM
    preRoll[preHead] = rc; preT[preHead] = now; preHead = (preHead + 1) % 100; if (preCount < 100) preCount++;
    if (pgN && now - lastRecMs > 3000) storeFlushPage();
    return;
  }
  if (now - lastRecMs > 5000) sessId = now;                // a new burst of writing
  for (uint8_t k = 0; k < preCount; k++) { uint8_t i = (preHead + 100 - preCount + k) % 100; storeAdd(preT[i], preRoll[i]); }
  preCount = 0; storeAdd(now, rc); lastRecMs = now;
}
// ---- sync with the app ----
bool dumping = false; uint32_t dumpPage = 0, dumpLeft = 0, dumpSent = 0; uint8_t dumpChunk = 0;
void storeStartDump() {
  storeFlushPage(); dumping = true; dumpLeft = nStored; dumpSent = 0; dumpChunk = 0;
  dumpPage = (wPage + PAGES - nStored) % PAGES;            // oldest stored page
  uint8_t p[11] = {0x05}; uint32_t now = millis(); memcpy(p + 1, &now, 4); uint32_t n = nStored; memcpy(p + 5, &n, 4); p[9] = 1; p[10] = 0;
  bleSend(p, 11);                                         // 0x05: device clock now (ms) + how many pages follow
}
void storePump(bool ready) {                               // sends a few packets per loop while dumping; erases in the background after 'X'
  if (dumping && ready) {
    for (uint8_t k = 0; k < 4 && dumpLeft; k++) {
      if (dumpChunk == 0) qRead(dumpPage * PAGE, qbuf, PAGE);
      uint8_t p[20]; p[0] = 0x04; p[1] = dumpSent & 0xFF; p[2] = (dumpSent >> 8) & 0xFF; p[3] = dumpChunk;
      memcpy(p + 4, ((uint8_t *)qbuf) + dumpChunk * 16, 16); bleSend(p, 20);
      if (++dumpChunk == 16) { dumpChunk = 0; dumpPage = (dumpPage + 1) % PAGES; dumpLeft--; dumpSent++; }
    }
    if (!dumpLeft) { dumping = false; sendEvent(8, (uint16_t)min(dumpSent, (uint32_t)65535)); }
  }
  if (eraseLeft && !dumping) {                             // one 4 KB sector per loop, so sampling barely notices
    qErase4k(eraseFrom * PAGE); eraseFrom = (eraseFrom + 16) % PAGES;
    eraseLeft = eraseLeft > 16 ? eraseLeft - 16 : 0;
    if (!eraseLeft) { nStored = 0; wPage = 0; sendEvent(7, 0); }
  }
}
void storeWipe() { storeFlushPage(); uint32_t first = (wPage + PAGES - nStored) % PAGES; eraseFrom = first - (first % 16); eraseLeft = nStored + (first % 16) + 16; }

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 1500) { }
  Serial.println("boot v9 (pocket mode: stores writing when no phone)");
  if (!imuInit()) {
    Serial.println("E,IMU_NOT_FOUND");
    pinMode(LED_RED, OUTPUT);
    while (1) { digitalWrite(LED_RED, !digitalRead(LED_RED)); delay(200); }
  }
  bleBegin();
  qOk = qInit(); if (qOk) storeScan(); else Serial.println("E,FLASH_FAIL");
#ifndef MIC_CONTACT
#define MIC_CONTACT 0
#endif
#if MIC_CONTACT
  micOn = micStart();                             // always on: contact sound for pen-down / pen-up
#endif
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  for (uint8_t i = 0; i < WIN; i++) gwin[i] = 0;
  Serial.println("H,t_ms,ax,ay,az,gx,gy,gz,busy,press,writing");
  nextSample = micros();
}

// ",<value>" with a fixed number of decimals (scale 1000 -> 3 decimals, 10 -> 1), without needing float printf
int fx(char *b, int n, int cap, float v, int scale) {
  long q = lroundf(v * scale); const char *sg = q < 0 ? "-" : ""; if (q < 0) q = -q;
  return n + snprintf(b + n, cap - n, scale == 1000 ? ",%s%ld.%03ld" : ",%s%ld.%01ld", sg, q / scale, q % scale);
}

void loop() {
  bool ready = bleReady();
  bleReceive();
  pumpAudio(ready);
  storePump(ready);
  { static bool was = false; if (ready && !was) sendEvent(7, (uint16_t)min(nStored, (uint32_t)65535)); was = ready; }   // tell the app what's stored
  checkButton(ready, millis());
  pollVibration();                                // fast accelerometer, between samples
  if ((int32_t)(micros() - nextSample) < 0) return;
  nextSample += SAMPLE_US;

  uint32_t tA = micros();
  int16_t r[6];
  if (!readRaw(r)) return;
  uint32_t now = millis();
  uint8_t contact = contactByte();                // friction sound since the last sample (0 unless MIC_CONTACT)
  uint32_t tB = micros();
  uint8_t vib = readVibration();                  // surface vibration since the last sample
  uint32_t tC = micros();
  float gx = r[0] * 0.0175f, gy = r[1] * 0.0175f, gz = r[2] * 0.0175f;
  float ax = r[3] * 0.000122f, ay = r[4] * 0.000122f, az = r[5] * 0.000122f;

  // ---- v8.3 extras: sharpest jolt and fast-reading count in this 10 ms, and whether the chip saw a tap ----
  uint8_t peakB = 0; { float pk = 16.0f * log2f(1.0f + (float)vibPeak); peakB = pk > 255 ? 255 : (uint8_t)pk; }
  uint8_t nFast = vibCount; vibPeak = 0; vibCount = 0;
  uint8_t tap = tapped() ? 1 : 0;
  // ---- Bluetooth: motion packet ----
  if (ready && streamMotion && !talking && !dumping) {
    uint8_t p[20];
    p[0] = 0x01; p[1] = now & 0xFF; p[2] = (now >> 8) & 0xFF; p[15] = contact; p[16] = vib; p[17] = peakB; p[18] = nFast; p[19] = tap;
    memcpy(&p[3], r, 12);              // nRF52 is little-endian
    bleSend(p, 20);
  }
  uint32_t tD = micros();

  // ---- double tap (checked every 5th sample: the sensor latches it, nothing is missed) ----
  // (v8.3: double-tap events removed: the tap detector now fires on every fingertip landing, sent in each packet)

  // ---- writing detector ----
  float busy = busyness(sqrtf(gx * gx + gy * gy + gz * gz));
  bool active = busy > (writing ? BUSY_OFF : BUSY_ON);
  if (active) lastBusy = now;
  if (!writing && active) {
    writing = true; strokeStart = now;
    sendEvent(2, 0);
    Serial.print("E,STROKE_START,"); Serial.println(now);
  } else if (writing && (now - lastBusy) > END_HOLD_MS) {
    writing = false;
    uint32_t dur = lastBusy - strokeStart;
    if (dur >= MIN_STROKE_MS) {
      sendEvent(3, (uint16_t)min(dur, (uint32_t)65535));
      Serial.print("E,STROKE_END,"); Serial.print(lastBusy); Serial.print(','); Serial.println(dur);
    }
  }

  storeSample(now, r, vib, tap, ready, writing);   // v9: pocket mode
  // ---- battery every 10 s ----
  if ((int32_t)(now - nextBattery) >= 0) {
    nextBattery = now + 10000;
    uint16_t mv = batteryMv();
    sendEvent(4, mv);
    Serial.print("E,BATTERY_MV,"); Serial.println(mv);
    sendEvent(7, (uint16_t)min(nStored, (uint32_t)65535)); Serial.print("E,STORED_PAGES,"); Serial.println(nStored);
    Serial.print("E,RATE_HZ,"); Serial.println(samplesSinceRate / 10); samplesSinceRate = 0;   // should say ~100
    Serial.print("E,MIC,"); Serial.print(micOn ? 1 : 0); Serial.print(','); Serial.print(micSamples / 10); Serial.print(','); Serial.println(micPeak);   // on?, samples/s (~16000), loudest sample
    micSamples = 0; micPeak = 0;
    Serial.print("E,VIB,"); Serial.print(vibReadings / 10); Serial.print(','); Serial.println(vibLastOut); vibReadings = 0;   // fast readings/s (should be ~800), latest vibration value
    { uint8_t c1 = 0, who = 0; regRead(imuAddr, 0x10, &c1, 1); regRead(imuAddr, 0x0F, &who, 1);
      Serial.print("E,IMU,"); Serial.print(who, HEX); Serial.print(','); Serial.println(c1, HEX); }   // chip id (6A), CTRL1_XL (78 = 833 Hz)
    if (usN) { Serial.print("E,US,"); Serial.print(usRead / usN); Serial.print(','); Serial.print(usVib / usN); Serial.print(','); Serial.print(usBle / usN);
      Serial.print(','); Serial.print(usRest / usN); Serial.print(','); Serial.print(usSerial / usN); Serial.print(','); Serial.println(ready ? 1 : 0); }   // microseconds per sample: imu, vib, bluetooth, rest, usb print, connected?
    usRead = usVib = usBle = usRest = usSerial = 0; usN = 0;
  }

  samplesSinceRate++;
  uint32_t tE = micros();
  // ---- USB line, same columns as v4, but built first and sent in ONE write (13 separate prints took 21 ms) ----
  if (Serial) {
    char line[112]; int n = 0;
    n += snprintf(line + n, sizeof(line) - n, "D,%lu", (unsigned long)now);
    n = fx(line, n, sizeof(line), ax, 1000); n = fx(line, n, sizeof(line), ay, 1000); n = fx(line, n, sizeof(line), az, 1000);
    n = fx(line, n, sizeof(line), gx, 10); n = fx(line, n, sizeof(line), gy, 10); n = fx(line, n, sizeof(line), gz, 10);
    n = fx(line, n, sizeof(line), busy, 10);
    n += snprintf(line + n, sizeof(line) - n, ",%u,%u,%u,%u,%u,%u\r\n", contact, writing ? 1 : 0, vib, peakB, nFast, tap);
    if (n > 0 && n < (int)sizeof(line)) Serial.write((const uint8_t *)line, n);
  }
  uint32_t tF = micros();
  usRead += tB - tA; usVib += tC - tB; usBle += tD - tC; usRest += tE - tD; usSerial += tF - tE; usN++;
}
