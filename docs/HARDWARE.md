# Hardware

## Parts

| Part | Notes | Approx. cost |
|---|---|---|
| [Seeed XIAO nRF52840 **Sense**](https://wiki.seeedstudio.com/XIAO_BLE/) | Has the 6-axis IMU (LSM6DS3TR-C), PDM mic, 2 MB QSPI flash and BLE 5 on board. The non-Sense version **will not work**. | ~£15 |
| 3.7 V LiPo, 100–250 mAh | Soldered to the BAT+ / BAT− pads on the back. Charges over USB-C. | ~£5 |
| Momentary push button | Between **D1** and **GND**, hold to talk. No resistor needed (internal pull-up). | <£1 |
| Finger sleeve | A compression sleeve with the board on the back of the middle phalanx | |

## Wiring

```
             XIAO nRF52840 Sense (back)
   LiPo + ───── BAT+
   LiPo − ───── BAT−
   Button ───── D1
   Button ───── GND
```

## Flashing

1. Arduino IDE → Boards Manager → install **Seeed nRF52 mbed-enabled Boards** (tested with 2.9.3), and **ArduinoBLE** from the Library Manager (tested with 2.1.0). CI compiles this exact setup on every push. The code also has a Bluefruit path for the non-mbed *Seeed nRF52 Boards* package.
2. Open [`firmware/fingerscribe_ble/fingerscribe_ble.ino`](../firmware/fingerscribe_ble/fingerscribe_ble.ino), choose *Seeed XIAO nRF52840 Sense*, and upload.
3. Serial monitor at 115200: every 10 s it prints `E,RATE_HZ,<n>`. Check that it's about 100.

## What the firmware does

- **Motion:** gyro and accelerometer at **100 Hz**, streamed over the Nordic UART Service. The device name is `FingerScribe`.
- **Taps:** the IMU runs its own tap detector at **833 Hz**, because a fingertip landing lasts 2–5 ms, too short to catch at 100 Hz. A double tap starts and stops a voice note.
- **Hold to talk:** the PDM mic is encoded on the board as **8 kHz IMA-ADPCM**, 4 bits per sample. Each packet carries its own decoder state, so a lost packet costs only 3.75 ms of audio.
- **Pocket mode:** with no phone connected, writing is stored in the 2 MB QSPI flash, about **20 minutes of actual writing**. It syncs to the app on the next connection.
- **Battery:** the voltage is reported every 10 s.

## Bluetooth protocol

Nordic UART Service `6E400001-B5A3-F393-E0A9-E50E24DCCA9E`. Notifications arrive on TX `…0003`, commands are written to RX `…0002`. All values are little-endian.

| Packet | Layout | Size |
|---|---|---|
| `0x01` motion | `t_ms u16`, `gx gy gz ax ay az` (6 × i16 raw), `contact u8`, `vib u8`. Gyro 0.0175 °/s per unit, accel 0.000122 g per unit. | 17 B |
| `0x02` event | `event u8` [`value u16`]: 1 double tap, 2 stroke start, 3 stroke end (ms), 4 battery (mV), 5 talk start, 6 talk end, 7 stored data available | 2–4 B |
| `0x03` audio | `seq u8`, `predictor i16`, `index u8`, 15 B ADPCM (30 samples) | 20 B |
| `0x04` stored | One page of pocket-mode samples from flash | |

| Command | Meaning |
|---|---|
| `T n` | Double-tap threshold, 1–31 (units of 0.125 g, default 16) |
| `S 0/1` | Stop / start the motion stream. Events always flow. |
| `D` / `X` | Send stored pocket-mode data / wipe it after the app has saved it |
