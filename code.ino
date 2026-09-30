/*
  PhantoCure - Sistem Vibrotactile Closed-Loop untuk Terapi Phantom Limb Pain
  ---------------------------------------------------------------------------
  Prototipe KTI - ESP32
  Fungsi:
    1. Membaca sinyal GSR (nyeri/arousal) dan PPG (BPM, indikator stres)
    2. Membaca sinyal EMG (deteksi kontraksi otot residual limb sederhana)
    3. Menghitung skor nyeri komposit (0-100)
    4. Mengontrol 6 motor vibrasi (PWM) di titik pemetaan phantom hand
       -> intensitas naik/turun otomatis mengikuti skor nyeri
    5. Menampilkan status di OLED SSD1306

  Library yang perlu di-install (Arduino Library Manager):
    - SparkFun MAX3010x Pulse and Proximity Sensor Library
    - Adafruit SSD1306
    - Adafruit GFX Library
*/

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "MAX30105.h"
#include "heartRate.h"

// ---------- KONFIGURASI PIN ----------
// Motor vibrasi (PWM) - 6 channel, tempatkan di titik pemetaan phantom hand
// Catatan: GPIO12 adalah strapping pin ESP32 (boot mode) - diganti ke GPIO33
// agar tidak berisiko mengganggu proses boot saat rangkaian motor terpasang.
const int MOTOR_PINS[6] = {13, 33, 14, 27, 26, 25};
const int NUM_MOTORS = 6;

// PWM channel ESP32 (LEDC)
const int PWM_FREQ = 5000;
const int PWM_RESOLUTION = 8; // 0-255

// Sensor analog
// EMG_PIN kompatibel dengan sensor EMG generik (breakout module),
// AD8232, atau rakitan INA128/INA333 - semuanya sama-sama output
// sinyal analog tunggal yang dibaca lewat ADC, jadi tidak perlu
// ubah kode apapun di updateEMG() saat ganti jenis sensor.
const int GSR_PIN = 34;   // ADC1
const int EMG_PIN = 35;   // ADC1

// OLED
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_ADDR 0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// PPG sensor
MAX30105 particleSensor;

// ---------- VARIABEL GLOBAL ----------
float gsrBaseline = 0;
long lastBeat = 0;
float bpm = 0;
const byte RATE_SIZE = 4;
byte rates[RATE_SIZE];
byte rateSpot = 0;

// EMG
const int EMG_WINDOW_MS = 200;
unsigned long lastEmgWindow = 0;
float emgBuffer[64];
int emgIndex = 0;
float emgMAV = 0;

// Skor nyeri
float painScore = 0; // 0-100
// 4 tingkat, dikalibrasi mengikuti cutpoint klinis NRS Serlin dkk. (1995):
// NRS 0=tidak nyeri, 1-4=ringan, 5-6=sedang, 7-10=berat -> diskalakan x10 ke 0-100
enum PainLevel { TIDAK_NYERI, RINGAN, SEDANG, BERAT };
PainLevel currentLevel = TIDAK_NYERI;

// Baseline BPM personal, diisi saat kalibrasi di setup()
float bpmBaseline = 0;

// Intensitas stimulasi saat ini (0-255 per motor)
int stimIntensity = 60; // mulai dari intensitas rendah, naik bertahap

// Timing
unsigned long lastSampleTime = 0;
const unsigned long SAMPLE_INTERVAL = 50; // ms, sesuai sliding window paper (50ms)

unsigned long lastDisplayUpdate = 0;
const unsigned long DISPLAY_INTERVAL = 500;

// ---------- SETUP ----------
void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22); // SDA, SCL

  // Setup motor PWM (API baru ESP32 Arduino Core 3.x: ledcAttach langsung
  // ke pin, tanpa perlu channel terpisah seperti ledcSetup/ledcAttachPin lama)
  for (int i = 0; i < NUM_MOTORS; i++) {
    ledcAttach(MOTOR_PINS[i], PWM_FREQ, PWM_RESOLUTION);
    ledcWrite(MOTOR_PINS[i], 0); // mati dulu di awal
  }

  // Setup OLED
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("OLED gagal diinisialisasi");
  }
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("PhantoCure v1.0");
  display.println("Inisialisasi...");
  display.display();

  // Setup MAX30102
  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    Serial.println("MAX30102 tidak terdeteksi. Cek wiring.");
  } else {
    particleSensor.setup(); // konfigurasi default (LED brightness, sample rate, dst)
    particleSensor.setPulseAmplitudeRed(0x0A);
    particleSensor.setPulseAmplitudeGreen(0);
  }

  // Kalibrasi baseline GSR (diam selama 3 detik saat start)
  Serial.println("Kalibrasi GSR baseline, jangan bergerak...");
  long sum = 0;
  for (int i = 0; i < 30; i++) {
    sum += analogRead(GSR_PIN);
    delay(100);
  }
  gsrBaseline = sum / 30.0;
  Serial.print("Baseline GSR: ");
  Serial.println(gsrBaseline);

  // Kalibrasi baseline BPM (diam selama ~15 detik agar PPG sempat
  // mendeteksi beberapa detak yang stabil)
  Serial.println("Kalibrasi BPM baseline, jangan bergerak...");
  long bpmSum = 0;
  int bpmSamples = 0;
  unsigned long calibStart = millis();
  while (millis() - calibStart < 15000) {
    updatePPG(); // fungsi ini mengisi variabel global 'bpm' saat detak terdeteksi
    if (bpm > 0) {
      bpmSum += bpm;
      bpmSamples++;
    }
    delay(50);
  }
  bpmBaseline = (bpmSamples > 0) ? (bpmSum / (float)bpmSamples) : 75.0; // fallback 75 jika gagal deteksi
  Serial.print("Baseline BPM: ");
  Serial.println(bpmBaseline);

  delay(1000);
}

// ---------- FUNGSI: BACA GSR ----------
float readGSR() {
  int raw = analogRead(GSR_PIN); // 0-4095 (ADC 12-bit ESP32)
  // Normalisasi terhadap baseline -> makin tinggi dari baseline = makin "aroused"
  float delta = raw - gsrBaseline;
  float normalized = constrain(delta / 200.0 * 100.0, 0, 100); // skala 0-100, sesuaikan pembagi setelah kalibrasi riil
  return normalized;
}

// ---------- FUNGSI: BACA PPG / BPM ----------
void updatePPG() {
  long irValue = particleSensor.getIR();

  if (checkForBeat(irValue)) {
    long delta = millis() - lastBeat;
    lastBeat = millis();

    float beatsPerMinute = 60000.0 / delta;

    if (beatsPerMinute < 255 && beatsPerMinute > 20) {
      rates[rateSpot++] = (byte)beatsPerMinute;
      rateSpot %= RATE_SIZE;

      int total = 0;
      for (byte i = 0; i < RATE_SIZE; i++) total += rates[i];
      bpm = total / RATE_SIZE;
    }
  }
}

// ---------- FUNGSI: BACA EMG (fitur MAV sederhana) ----------
float emgSmoothed = 0;
const float EMG_ALPHA = 0.3; // faktor smoothing EMA, 0-1 (lebih kecil = lebih halus)

void updateEMG() {
  int raw = analogRead(EMG_PIN);
  float centered = abs(raw - 2048); // pusatkan di titik tengah ADC 12-bit

  // Exponential moving average - meredam noise dari sensor EMG generik
  // sebelum masuk ke perhitungan MAV. Jika sinyal terlalu lemah/kuat,
  // atur dulu trimmer gain fisik di modul sensor, baru sesuaikan EMG_ALPHA.
  emgSmoothed = (EMG_ALPHA * centered) + ((1 - EMG_ALPHA) * emgSmoothed);

  if (emgIndex < 64) {
    emgBuffer[emgIndex++] = emgSmoothed;
  }

  if (millis() - lastEmgWindow >= EMG_WINDOW_MS) {
    // Hitung Mean Absolute Value (MAV) dari window
    float sum = 0;
    for (int i = 0; i < emgIndex; i++) sum += emgBuffer[i];
    emgMAV = (emgIndex > 0) ? (sum / emgIndex) : 0;

    emgIndex = 0;
    lastEmgWindow = millis();
  }
}

// deteksi kontraksi sederhana berbasis threshold (bukan LDA penuh, untuk prototipe cepat)
bool isMuscleContracting() {
  const float THRESHOLD = 150.0; // sesuaikan setelah kalibrasi dengan sinyal riil
  return emgMAV > THRESHOLD;
}

// ---------- FUNGSI: HITUNG SKOR NYERI KOMPOSIT ----------
float computePainScore(float gsrNorm, float bpmValue) {
  // Normalisasi BPM berbasis %kenaikan dari baseline PERSONAL (bukan angka
  // absolut), karena resting heart rate berbeda-beda antar individu (normal
  // klinis 60-100 BPM). %kenaikan dihitung relatif terhadap bpmBaseline
  // hasil kalibrasi di setup().
  float percentIncrease = 0;
  if (bpmBaseline > 0) {
    percentIncrease = ((bpmValue - bpmBaseline) / bpmBaseline) * 100.0;
  }

  // Pembagi 30 dipilih karena mendekati batas atas rata-rata kenaikan BPM
  // yang dilaporkan pada nyeri akut (~29%) dalam literatur nyeri-kardiovaskular.
  float bpmNorm = constrain(percentIncrease / 30.0 * 100.0, 0, 100);

  // Pembobotan: GSR 60%, BPM 40% (GSR literatur lebih kuat sbg indikator nyeri tunggal)
  float score = (0.6 * gsrNorm) + (0.4 * bpmNorm);
  return constrain(score, 0, 100);
}

// Klasifikasi 4 tingkat, dikalibrasi mengikuti cutpoint klinis NRS
// Serlin dkk. (1995): tidak nyeri=0, ringan=1-4, sedang=5-6, berat=7-10
// pada skala NRS 0-10, diskalakan x10 menjadi skor komposit 0-100.
PainLevel classifyPain(float score) {
  if (score < 10) return TIDAK_NYERI; // setara NRS 0
  if (score < 40) return RINGAN;      // setara NRS 1-4
  if (score < 60) return SEDANG;      // setara NRS 5-6
  return BERAT;                        // setara NRS 7-10
}

// ---------- FUNGSI: KONTROL INTENSITAS STIMULASI (CLOSED LOOP) ----------
void updateStimulation(PainLevel level) {
  const int STEP = 15;
  const int MIN_INTENSITY = 40;   // jangan pernah 0 total kalau sesi aktif, agar transisi halus
  const int MAX_INTENSITY = 220;  // batas aman, jangan sampai 255 (motor bisa terlalu kuat/panas)

  switch (level) {
    case TIDAK_NYERI:
    case RINGAN:
      // toleransi baik -> boleh naik bertahap (memperkuat persepsi phantom hand)
      stimIntensity = min(stimIntensity + STEP, MAX_INTENSITY);
      break;
    case SEDANG:
      // pertahankan level saat ini, jangan naikkan dulu
      break;
    case BERAT:
      // indikasi tidak nyaman -> turunkan segera (mode proteksi)
      stimIntensity = max(stimIntensity - STEP * 2, MIN_INTENSITY);
      break;
  }

  // Terapkan ke semua motor (bisa dikembangkan jadi pola berbeda per motor sesuai peta phantom hand)
  for (int i = 0; i < NUM_MOTORS; i++) {
    ledcWrite(MOTOR_PINS[i], stimIntensity);
  }
}

// ---------- FUNGSI: TAMPILKAN DI OLED ----------
void updateDisplay() {
  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("PhantoCure - Monitoring");
  display.print("BPM: "); display.println(bpm, 0);
  display.print("GSR: "); display.println(readGSR(), 0);
  display.print("Skor nyeri: "); display.println(painScore, 0);

  display.print("Level: ");
  switch (currentLevel) {
    case TIDAK_NYERI: display.println("TIDAK NYERI"); break;
    case RINGAN: display.println("RINGAN"); break;
    case SEDANG: display.println("SEDANG"); break;
    case BERAT: display.println("BERAT"); break;
  }

  display.print("Intensitas: "); display.println(stimIntensity);
  display.print("Kontraksi EMG: ");
  display.println(isMuscleContracting() ? "YA" : "TIDAK");

  display.display();
}

// ---------- LOOP UTAMA ----------
void loop() {
  updatePPG();
  updateEMG();

  if (millis() - lastSampleTime >= SAMPLE_INTERVAL) {
    lastSampleTime = millis();

    float gsrNorm = readGSR();
    painScore = computePainScore(gsrNorm, bpm);
    currentLevel = classifyPain(painScore);

    updateStimulation(currentLevel);

    // Logging ke Serial untuk pencatatan data (bisa disalin ke CSV untuk laporan KTI)
    Serial.print(millis()); Serial.print(",");
    Serial.print(gsrNorm); Serial.print(",");
    Serial.print(bpm); Serial.print(",");
    Serial.print(painScore); Serial.print(",");
    Serial.print(stimIntensity); Serial.print(",");
    Serial.println(isMuscleContracting() ? 1 : 0);
  }

  if (millis() - lastDisplayUpdate >= DISPLAY_INTERVAL) {
    lastDisplayUpdate = millis();
    updateDisplay();
  }
}
