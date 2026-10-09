# KariPom (かりポム)

[日本語 README](README.md)

![Two KariPom units in pink and Tiffany blue (built by the creator)](docs/manual/images/karipom_cover.png)

**KariPom (かりポム)** is a small desktop animatronic robot built around an M5Stack CoreS3. It talks, moves, and listens. Designed and built as a personal project in the summer of 2026, this repository shares its code, 3D models, and manuals as a record of **one complete work**.

## About This Repository

This project is published to document and share a personally built creation. It is not intended for commercialization, sale, or distribution as a DIY kit. Building it requires basic knowledge of Arduino, electronics, and 3D printing. The creator does not provide individual build support. However, the two published manuals and source code are written so that you can load them into an AI assistant (such as ChatGPT or Claude) and work through questions as you go. When asking for setup assistance, it is recommended that you also ask the AI to help you make the application launch by double-clicking next time, rather than stopping once it runs (see the manuals for details).

## Main Features

Facial expressions and audio playback; servo-driven head movement (up/down and left/right); real-time lip-sync to PC audio (BBX); eight Visualizer modes and 24 Lighting effects; a browser-operated Web UI (KariPom Lab); joystick control; spontaneous speech; sleep and wake modes; and SD-card management of logs and audio files.

Three Face Modes are available: **KariPom**, **Miss KariPom**, and **Face Gallery**. Selecting **None** in Face Gallery hides the face and lets you enjoy the Lighting and Visualizer effects across the entire display.

With the optional **Karipom Ear** hardware extension (PCM1808 + Raspberry Pi Pico 2 H), KariPom can react to line-input audio using BBX without a PC.

## Repository Structure

| Directory | Contents |
|---|---|
| `cores3-main/` | Arduino firmware for the main KariPom unit (M5Stack CoreS3) |
| `pico2-pcm1808/` | Raspberry Pi Pico 2 firmware for the Karipom Ear extension. **The completed version is `pico2-pcm1808/karipom_pico2/karipom_pico2.ino`** (the “Step2” label in the sketch header is a historical development name). |
| `mac-companion/` | PC-side KariPom Companion application (Python; Windows, macOS, and Linux) |
| `3d_models/` | 3D-printable enclosure files (`karipom.3mf` and nine STL files under `stl/`) |
| `sdcard/` | Files to copy to the microSD card (`faces/` expression PNGs and the `sounds/` audio-placement guide) |
| `docs/manual/` | KariPom User Manual |
| `docs/pcm1808_pico2/` | Official Karipom Ear Design and Build Manual |
| `docs/karipom_ear/` | Development records, specifications, and verification procedures for Karipom Ear |
| `docs/stackchan/` | Research documents on porting to the official M5Stack StackChan |

## Documentation

Start with the **[KariPom User Manual](docs/manual/karipom_manual_EN.md)** ([DOCX version](docs/manual/karipom_manual.docx)). It explains what KariPom can do, how to enjoy it, and where to begin.

For the electronics-based Karipom Ear extension (PCM1808 + Pico 2), LINE OUT modification, and Bluetooth input (MH-M18), the official reference is the **[Karipom Ear Design and Build Manual](docs/pcm1808_pico2/PCM1808_Pico2_設計マニュアル.md)** ([DOCX version](docs/pcm1808_pico2/PCM1808_Pico2_設計マニュアル.docx)). Documents under `docs/karipom_ear/` are development records. If they overlap with or differ from the official manual, follow the official manual.

The LINE OUT modification has been built and tested on the creator's own unit. The Bluetooth input (MH-M18) option has complete design documents and instructions, but **has not yet been installed on the creator's unit**.

For those considering a port to the official M5Stack StackChan, the **[StackChan Porting Specification](docs/stackchan/StackChan_KariPom移植用仕様書_公開版.md)** documents the investigation of the official StackChan servo specifications and the proposed porting approach from KariPom.

## 3D Models

The repository includes `3d_models/karipom.3mf` (a project file containing all parts) and nine parts under `3d_models/stl/`: `body`, `chest`, `hip`, `pelvis`, `spinal_cord`, `left_shoulder`, `right_shoulder`, `ears`, and `feet`. You may choose any colors. See Chapter 5 of the User Manual for details.

The enclosure for Karipom Ear (PCM1808 + Pico 2) is `karipom_hip.stl` under `stl/`. An optional `karipom_hip_lineout.stl` with an opening for a 3.5 mm jack is also available for the LINE OUT modification. If you do not add LINE OUT, you can use `karipom_hip.stl` unchanged. See Chapter 11 of the Karipom Ear Design and Build Manual.

## SD Card Files

KariPom requires a microSD card (see Chapter 17, “SD Card Management,” of the User Manual). The `sdcard/` directory contains publicly distributable files for that card.

Copy the **contents** of `sdcard/`—the `faces/` and `sounds/` directories—to the **root of the microSD card**, preserving their directory structure. Do not copy the enclosing `sdcard` directory itself.

- `faces/` — 13 expression PNGs for Face Gallery (fixed displays selected in Face Mode), random faces during sleep or spontaneous speech, and related functions.
- `sounds/` — A `README.md` explaining where audio files belong (**WAV files are not included**).

To avoid imposing the licensing and redistribution terms of individual text-to-speech services or audio assets on this repository, WAV files for fixed speech and spontaneous speech are not bundled. If you want audio playback, create your own WAV files using a text-to-speech tool or another source of your choice. See [`sdcard/sounds/README.md`](sdcard/sounds/README.md) for the required fixed-audio filenames and placement of spontaneous-speech audio. KariPom continues running even if these files are missing; only the corresponding audio playback is skipped.

## Sister Project: KariPom Desktop

A standalone version lets you enjoy KariPom's lip-sync on a PC without the physical CoreS3 robot.

**[KariPom Desktop](https://github.com/kariagepompadour/KariPom-Desktop)**

## License

This repository is published for noncommercial use. See [LICENSE.md](LICENSE.md) for details.

| Material | License |
|---|---|
| Source code (Arduino firmware and Python application) | Custom noncommercial license (personal building, modification, and noncommercial sharing permitted; commercial use prohibited) |
| 3D models, manuals, images, and other materials | [CC BY-NC 4.0](https://creativecommons.org/licenses/by-nc/4.0/) (Attribution–NonCommercial) |
| Names and logos including “KariPom” and “かりポム” | Outside the scope of the above licenses; trademark-like or brand use is not permitted |

---

Copyright (c) 2026 Kariage POMPADOUR Entertainment Corporation
