# Flipper Access Audit

[![Build](https://github.com/matthewkayne/flipper-access-audit/actions/workflows/build.yml/badge.svg)](https://github.com/matthewkayne/flipper-access-audit/actions/workflows/build.yml)
[![Lint](https://github.com/matthewkayne/flipper-access-audit/actions/workflows/lint.yml/badge.svg)](https://github.com/matthewkayne/flipper-access-audit/actions/workflows/lint.yml)
[![Release](https://img.shields.io/github/v/release/matthewkayne/flipper-access-audit)](https://github.com/matthewkayne/flipper-access-audit/releases/latest)
[![License](https://img.shields.io/github/license/matthewkayne/flipper-access-audit)](LICENSE)

A Flipper Zero app for counting scans of particular tags over time.  Reporting to be done off device with separate tools.  This just logs data when it scans the NFC: Date & ID in a non-specifiable CSV that can be wiped from within the app. 

Forked from the [original](https://github.com/matthewkayne/flipper-access-audit) for my own purposes.  Not meant to be superior or a replacement.

### Build from source

Requires [uFBT](https://github.com/flipperdevices/flipperzero-ufbt).

```sh
ufbt
# FAP is written to dist/access_audit.fap
# Then follow any option above to copy it to your Flipper
```

---

## Usage

| Screen | Controls |
|---|---|
| Scan | Tap/hold card · **Left/Right** cycle NFC → RFID → iCLASS · **Up** view reports · **Back** exit |
| Result | **OK** rescan · **Back** save session and proceed to naming |
| Name session | QWERTY keyboard · **OK key** save with name · **Back** skip naming / backspace |
| Reports list | **Up/Down** scroll · **OK** open · **Back** return to scan |
| Report viewer | **Up/Down** scroll lines · **Back** return to list · **Hold Back** delete report |

### Score interpretation

| Label | Score | Meaning |
|---|---|---|
| HIGH RISK | 35-100 | Legacy family (MIFARE Classic, EM4100, HID iCLASS, Plus SL1) or static-replay pattern |
| MODERATE | 20-34 | Risk indicators present; review recommended |
| LOW RISK | 10-19 | Minor concerns, e.g. incomplete metadata |
| SECURE | 0-9 | Modern crypto family with no major findings |

### Card classification depth

| Family | Sub-types detected |
|---|---|
| MIFARE Classic | 1K · 4K · Mini (via SAK byte) |
| MIFARE DESFire | EV1 · EV2 · EV3 · Light (via GetVersion) |
| MIFARE Plus | SL1 · SL2 · SL3 (via security level response) |
| MIFARE Ultralight / NTAG | Ultralight C · NTAG203/213/215/216 · NTAG I2C |
| HID iCLASS | Legacy 2k · Legacy 16k · Legacy 32k (via ACTALL/IDENTIFY/READ block 1) |
| 125 kHz RFID | EM4100 · HID H10301 · HID Generic · Indala · generic 125 kHz |

---

## How it works

1. The NFC scanner detects which protocols the card supports.
2. The richest available poller is started (DESFire → Plus → Ultralight → ISO14443-3a).
3. The poller reads the UID and card-specific metadata without authentication; no sectors are unlocked, no data is modified.
4. For HID iCLASS, a proprietary ACTALL → IDENTIFY → SELECT → READ block 1 exchange runs over the ISO15693 RF channel to obtain the CSN and memory variant.
5. The observation is scored against eight named rules (see [docs/rules.md](docs/rules.md)).
6. Results are displayed on screen and appended to the session buffer.
7. On save, the session is written as a `.txt` report with per-card advice and a session-level advisory.

---

## Development

- Platform: Flipper Zero (official firmware, Momentum)
- Language: C (uFBT / Flipper SDK)
- CI: GitHub Actions, builds against official release, official dev, Momentum release, and Momentum dev SDKs on every push; clang-format and cppcheck lint on every push

```
core/
  observation.h           - data model (TechType, CardType, AccessObservation)
  observation_provider.c  - NFC scan pipeline (scanner + poller state machine)
  rfid_provider.c         - RFID 125 kHz scan pipeline (LFRFIDWorker)
  iclass_provider.c       - HID iCLASS scan pipeline (ISO15693 poller + proprietary exchange)
  rules.c                 - named audit rules
  scoring.c               - score calculator + card-type strings
  session.c               - multi-scan session buffer
  report.c                - SD card save + report listing/loading
access_audit.c            - app loop, screens, input handling
```

---

## Author

[@matthewkayne](https://github.com/matthewkayne) - AI-assisted development with [Claude Code](https://claude.ai/code)
---
