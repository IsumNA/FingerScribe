# Judge testing guide

**Live app:** https://fingerscribe.vercel.app/?demo=1 (Chrome desktop or Android works best. On iPhone, use Safari for everything except Bluetooth.)
**Track:** Physical AI · **Video:** see the [README](../README.md#demo-video)

The app stays live with its Nebius key active until judging ends.

## 1. Without the sleeve (about 3 minutes)

`?demo=1` loads a sample journal of a few days of notes, so every AI feature has something to work with. To remove the sample later, open *Settings → Remove sample notes*.

| Step | What to do | What you should see | Model |
|---|---|---|---|
| 1 | Open a note, tap ✨ **Tidy** | Spelling and punctuation are fixed. Names from the journal are kept. | Nemotron 3.5 Lightning |
| 2 | 🎧 **Talk**: hold the mic and ask *"what book did Sam recommend?"* (allow microphone access) | A short spoken answer from the journal: *The Design of Everyday Things* | Lightning (1M context) |
| 3 | 🎧 **Talk**: *"is the Science Museum open on Sunday?"* | An answer with a source. The model decided to call Tavily itself. | Lightning + Tavily tool |
| 4 | 🌙 **Your day** | One entry in the writer's words, "you've written this before" echoes, an answer to today's saved question (*"how long does it actually take to form a habit?"*), to-dos and contacts, and one question | Nemotron 3 Super 120B + Tavily |
| 5 | Mind map (network icon) | Themes and people linked across notes | |

**Checking the model calls:** open DevTools → Network and filter on `nebius`. Each request body shows the `model`, for example `nvidia/Nemotron-3_5-Lightning`. You can also open `/nebius/health`, which returns `{"key": true}`.

## 2. With the sleeve (shown in the video)

1. *Connect pen* → choose **FingerScribe** in the Bluetooth picker.
2. Write a word on any surface. The raw reader's guess appears grey, then Nemotron's verified fix appears underlined in green, as in the screenshot in the README.
3. Write something with a number or email (*"call mum 07700 900123"*). The ink is sent as an image to Super VL.
4. Hold the sleeve's button and speak. The audio goes to Parakeet on the Nebius AI Cloud endpoint.
5. Draw a box (☐ → to-do), a star (★) or a `?` (question for tonight).

## 3. Where each judging criterion is addressed

| Criterion | Evidence |
|---|---|
| **Technological implementation** | Custom firmware with on-chip 833 Hz tap detection and 8 kHz ADPCM audio over BLE ([firmware](../firmware/fingerscribe_ble/fingerscribe_ble.ino)). On-phone path model and CTC reader ported to plain JS, matching TensorFlow to 0.00000. Three Nemotron models, each sized to its job. The **propose → verify** loop, where the sensor vetoes the LLM ([architecture](ARCHITECTURE.md)). Parakeet on an AI Cloud endpoint and training as an AI Cloud job ([cloud/](../cloud)). |
| **Design** | A wearable for one specific moment: catching a thought without looking at a screen. The app is calm and monochrome, with light and dark modes, and your real ink sits under every entry. The production sleeve design is a matte knit band with the electronics hidden inside. |
| **Potential impact** | Journalling helps mental health, yet most people stop because the phone gets in the way. Eyes-up capture also helps people with low vision or a hard time typing, and anyone who can't take out a phone in a meeting, lecture or on a commute. |
| **Quality of the idea** | Using the LLM as the *proposer* and the physics of the finger as the *verifier* is a general pattern for trustworthy physical AI. It lets a wearable learn each user's own names and words without training on their data. |

## 4. Honest scope

- The results are from **one writer** ([README → Results](../README.md#results)). Testing with more writers is next.
- The trained reader weights aren't in the repo. The live app serves them, and the [README](../README.md#weights) explains why.
- No numbers in the README or the video are estimated. Anything we haven't measured is left out.
