# PhantoCure – Firmware Vibrotactile Closed-Loop (ESP32)

Firmware purwarupa untuk perangkat pendamping *mirror therapy* pada *phantom limb pain* (PLP). Sistem membaca sinyal fisiologis (GSR dan PPG), menghitung skor nyeri komposit 0–100, lalu menyesuaikan intensitas 6 motor vibrasi secara otomatis (closed-loop).

> **Status:** purwarupa untuk pembuktian kelayakan teknis (*proof of concept*). Belum divalidasi secara klinis dan bukan perangkat medis.

File: `phantom_limb_vibrotactile.ino`

## Fitur

- Pembacaan **GSR** (analog) dan **PPG** (MAX30102/MAX30105) untuk menghitung BPM.
- Pembacaan **EMG** (analog) dengan fitur *Mean Absolute Value* (MAV) dan deteksi kontraksi berbasis ambang.
- **Skor nyeri komposit** 0–100 dari GSR dan kenaikan BPM terhadap baseline pribadi.
- Klasifikasi 4 tingkat: `TIDAK_NYERI`, `RINGAN`, `SEDANG`, `BERAT`.
- Kontrol intensitas **6 motor vibrasi** via PWM (LEDC ESP32).
- Tampilan status di **OLED SSD1306** dan log data ke **Serial** (CSV).

## Perangkat Keras

| Komponen | Keterangan | Pin ESP32 |
|---|---|---|
| Sensor GSR | Output analog | GPIO34 (ADC1) |
| Sensor EMG | Output analog tunggal (modul generik, AD8232, atau INA128/INA333) | GPIO35 (ADC1) |
| MAX30102 / MAX30105 | PPG, I2C | SDA = GPIO21, SCL = GPIO22 |
| OLED SSD1306 128×64 | I2C, alamat `0x3C` | SDA = GPIO21, SCL = GPIO22 |
| 6 motor vibrasi | PWM | GPIO 13, 33, 14, 27, 26, 25 |

Catatan:
- GPIO12 (strapping pin) sengaja tidak dipakai; diganti GPIO33 agar tidak mengganggu proses boot.
- Pin GPIO34 dan GPIO35 hanya input, sehingga cocok untuk sensor analog.
- Kode tidak mengatur driver motor. Pin GPIO ESP32 memiliki batas arus kecil, jadi motor sebaiknya dihubungkan lewat transistor/driver, bukan langsung ke pin.

## Dependensi

Pasang lewat Arduino Library Manager:

- SparkFun MAX3010x Pulse and Proximity Sensor Library (`MAX30105.h`, `heartRate.h`)
- Adafruit SSD1306
- Adafruit GFX Library

Board: **ESP32 Arduino Core 3.x**. Kode memakai `ledcAttach(pin, freq, resolusi)` yang tidak tersedia di Core 2.x.

## Cara Menggunakan

1. Rangkai perangkat sesuai tabel pin di atas.
2. Buka `phantom_limb_vibrotactile.ino` di Arduino IDE, pilih board ESP32 dan port yang sesuai.
3. Upload, lalu buka Serial Monitor pada **115200 baud**.
4. Saat startup, **pasien harus diam** selama proses kalibrasi:
   - Baseline GSR: 30 sampel selama ±3 detik.
   - Baseline BPM: ±15 detik. Jika tidak ada detak terdeteksi, dipakai nilai cadangan 75 BPM.
5. Setelah kalibrasi, sistem berjalan otomatis. Status tampil di OLED dan data dikirim ke Serial.

## Cara Kerja

### 1. Skor nyeri komposit

```
gsrNorm  = clamp((GSR_raw - baselineGSR) / 200 × 100, 0, 100)
bpmNorm  = clamp(((BPM - baselineBPM) / baselineBPM × 100) / 30 × 100, 0, 100)
skor     = 0.6 × gsrNorm + 0.4 × bpmNorm
```

- Bobot GSR 60% dan BPM 40%.
- BPM dinormalisasi terhadap baseline pribadi, bukan angka absolut.
- Pembagi 200 (GSR) dan 30 (BPM) adalah nilai awal yang perlu dikalibrasi ulang dengan data riil.

### 2. Klasifikasi tingkat nyeri

Skor dipetakan mengikuti cutpoint NRS Serlin dkk. (1995) yang diskalakan ×10.

| Skor | Tingkat | Setara NRS |
|---|---|---|
| < 10 | TIDAK_NYERI | 0 |
| 10 – <40 | RINGAN | 1–4 |
| 40 – <60 | SEDANG | 5–6 |
| ≥ 60 | BERAT | 7–10 |

> Skor komposit adalah indikator fisiologis, bukan pengukuran nyeri yang sudah tervalidasi terhadap NRS pasien. Kesetaraan di atas adalah penskalaan, bukan hasil validasi.

### 3. Kontrol intensitas (closed-loop)

Dijalankan setiap 50 ms. Intensitas awal 60 dari rentang PWM 0–255.

| Tingkat | Aksi | Batas |
|---|---|---|
| TIDAK_NYERI / RINGAN | Naik 15 | Maks 220 |
| SEDANG | Dipertahankan | – |
| BERAT | Turun 30 (mode proteksi) | Min 40 |

Nilai yang sama dikirim ke keenam motor.

### 4. EMG

Sinyal EMG dipusatkan pada titik tengah ADC (2048), dihaluskan dengan EMA (α = 0,3), lalu dihitung MAV per jendela 200 ms. Kontraksi dianggap terdeteksi jika MAV > 150.

**Pada versi ini, hasil EMG hanya ditampilkan di OLED dan dicatat ke Serial. EMG belum memengaruhi kontrol motor.**

## Format Log Serial

Satu baris per 50 ms, tanpa header, dipisah koma:

```
waktu_ms, gsrNorm, bpm, painScore, stimIntensity, kontraksiEMG
```

`kontraksiEMG` bernilai 1 (ya) atau 0 (tidak). Salin output ke file `.csv` dan tambahkan header secara manual untuk analisis.

## Parameter yang Perlu Dikalibrasi

| Parameter | Lokasi | Nilai awal |
|---|---|---|
| Pembagi normalisasi GSR | `readGSR()` | 200 |
| Pembagi normalisasi BPM | `computePainScore()` | 30 |
| Bobot GSR / BPM | `computePainScore()` | 0,6 / 0,4 |
| Ambang kontraksi EMG | `isMuscleContracting()` | 150 |
| Faktor smoothing EMG | `EMG_ALPHA` | 0,3 |
| Langkah dan batas intensitas | `updateStimulation()` | 15; 40–220 |
| Arus LED merah MAX30102 | `setup()` | `0x0A` |

## Keterbatasan yang Perlu Diketahui

- Nilai ambang dan pembagi masih placeholder, belum diturunkan dari data pasien atau relawan.
- Sebelum detak pertama terdeteksi, `bpm` bernilai 0 sehingga kontribusi BPM ke skor menjadi 0.
- Karena kontrol dijalankan tiap 50 ms dengan langkah +15, intensitas naik dari 60 ke batas 220 dalam sekitar setengah detik saat skor rendah. Jika ingin transisi lebih halus, perlambat interval kontrol atau perkecil `STEP`.
- Semua motor menerima intensitas yang sama, belum ada pola per titik pemetaan *phantom hand*.
- Deteksi kontraksi EMG memakai ambang sederhana, bukan pengklasifikasi seperti LDA.
- Pembacaan ADC ESP32 bersifat non-linear dan berderau; hasil GSR perlu diperiksa pada perangkat masing-masing.
- Kalibrasi baseline hanya dilakukan sekali saat startup. Jika sensor bergeser atau kulit berubah kondisi, restart untuk kalibrasi ulang.

## Rencana Pengembangan

- Menghubungkan deteksi kontraksi EMG ke logika stimulasi.
- Pola stimulasi berbeda per motor sesuai peta *phantom hand*.
- Kalibrasi parameter dengan data uji, lalu validasi klinis.
