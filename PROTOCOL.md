# Nesto Panel protocol

**Version 1 · Draft.** This document is the contract between the panel firmware
(`nesto-firmware`, the C++ program on the thermostat) and the Home Assistant
integration (`ha-nesto`). Both sides are built from it, so a change here comes
first and both repos follow.

Throughout this document, **panel** means the thermostat running Nesto and
**HA** means the `ha-nesto` integration running inside Home Assistant.

The key words MUST, SHOULD and MAY are used as in RFC 2119.

## Contents

1. [Design principles](#1-design-principles)
2. [Discovery](#2-discovery)
3. [Transport and framing](#3-transport-and-framing)
4. [Message envelope](#4-message-envelope)
5. [Session start](#5-session-start)
6. [Pairing](#6-pairing)
7. [Authentication and encryption](#7-authentication-and-encryption)
8. [Synchronisation](#8-synchronisation)
9. [Layout](#9-layout)
10. [State](#10-state)
11. [Intents](#11-intents)
12. [Panel configuration](#12-panel-configuration)
13. [Display power](#13-display-power)
14. [Notifications](#14-notifications)
15. [Images for the carousel](#15-images-for-the-carousel)
16. [Time](#16-time)
17. [Sensors and input events](#17-sensors-and-input-events)
18. [Maintenance messages](#18-maintenance-messages)
19. [Firmware update](#19-firmware-update)
20. [Errors](#20-errors)
21. [Limits](#21-limits)
22. [Versioning](#22-versioning)
23. [Panel behaviour requirements](#23-panel-behaviour-requirements)
24. [Appendix A: how HA maps entities to items](#appendix-a-how-ha-maps-entities-to-items)
25. [Appendix B: example session](#appendix-b-example-session)
26. [Appendix C: built-in icons](#appendix-c-built-in-icons)

## 1. Design principles

- **The panel is a thin client.** It draws what HA sends and reports what the
  user did. It never talks to Home Assistant's own APIs, holds no access token,
  needs no DNS and no TLS client. HA chooses the entities, keeps their states,
  and turns the user's actions into service calls.
- **HA connects to the panel.** The panel listens on TCP and announces itself
  over mDNS. HA discovers it, pairs once, and from then on keeps one connection
  open. This is the same direction as ESPHome's native API.
- **Items, not entities.** HA translates each entity into an *item* with a
  small, fixed set of kinds and features (section 9). The panel doesn't need to
  know HA's domains, attributes or feature bit masks, and it keeps working when
  those change.
- **Optimistic and coalesced.** The panel updates its own screen as the ring
  turns and sends at most a few intents per second. HA's state updates are the
  final word (section 23).
- **Works when disconnected.** Without HA the panel can still sleep and wake
  and show the clock. It can only *control* things while connected.

## 2. Discovery

The panel announces a DNS-SD service on the local network:

| Field | Value |
|---|---|
| Service type | `_nesto._tcp.local.` |
| Instance name | `Nesto Panel <last 6 hex digits of the Wi-Fi MAC>` |
| Port | Default `6170`. HA MUST use the port from the SRV record, not the default. |

TXT record keys:

| Key | Example | Meaning |
|---|---|---|
| `id` | `18b4300a1b2c` | Panel ID: the Wi-Fi MAC address, lower-case hex, no separators. Stable for the life of the device. HA uses it as the config entry's unique ID. |
| `proto` | `1` | Protocol major version (section 22). |
| `fw` | `0.1.0` | Firmware version. |
| `model` | `nest-gen2` | Hardware model. |
| `paired` | `0` or `1` | Whether the panel already has a pairing key. |

HA's manifest declares `"zeroconf": ["_nesto._tcp.local."]`. Users can also add
a panel by IP address and port.

When the panel's IP address changes, HA follows the new address announced over
mDNS, matched by `id`.

## 3. Transport and framing

- TCP, panel listening, HA connecting.
- The panel accepts **one session at a time**. When a new connection
  authenticates successfully (section 7), the panel closes the older one. A new
  connection that has not authenticated never displaces an authenticated one.
- Every frame is a 4-byte big-endian unsigned length followed by that many
  bytes of payload:

  ```
  +----------------+----------------------------+
  | length (u32 BE)| payload (length bytes)     |
  +----------------+----------------------------+
  ```

- Before encryption starts, the payload is one UTF-8 JSON object. After it
  starts, the payload is that JSON object encrypted (section 7.3).
- A frame whose length is 0 or larger than the limit in section 21 is a
  protocol error: the receiver MUST close the connection.

Plain TCP with length-prefixed JSON was chosen over WebSocket or HTTP because
it needs no handshake parser on the panel, maps directly onto Python's asyncio
streams, and binary data (images, firmware) is rare enough that base64 inside
JSON is acceptable.

## 4. Message envelope

Every message is a JSON object with a type field `t`:

```json
{"t": "intent", "id": 41, "key": "l1", "action": "toggle"}
```

| Field | Required | Meaning |
|---|---|---|
| `t` | Yes | Message type, lower-case snake_case. |
| `id` | Only on requests | Positive integer chosen by the sender, unique among its open requests. It asks for a `result`. Each side has its own ID space, so both may use `id: 1` at the same time. |

A **request** carries `id` and MUST be answered with exactly one `result`:

```json
{"t": "result", "re": 41, "ok": true}
{"t": "result", "re": 41, "ok": false, "error": {"code": "unknown_key", "message": "No item l9"}}
```

A result MAY carry extra fields with the answer, as listed for each request.
A message without `id` is a one-way **event** and gets no result.

Receivers MUST ignore fields they don't know, so either side can add optional
fields within a protocol version. A receiver that gets an unknown `t` in a
request MUST answer with error `unknown_type`; an unknown event is ignored.

The direction of each message is marked **H→P** (HA to panel) or **P→H**
(panel to HA).

## 5. Session start

1. HA opens the TCP connection.
2. The panel immediately sends `hello` (P→H, event, plaintext):

   ```json
   {
     "t": "hello",
     "proto": 1,
     "panel_id": "18b4300a1b2c",
     "fw": "0.1.0",
     "model": "nest-gen2",
     "paired": true,
     "pairing_open": false,
     "nonce": "q3V0b2N1c3RvbXJhbmRvbQ==",
     "caps": {
       "display": {"width": 320, "height": 320, "shape": "round"},
       "input": ["ring", "press"],
       "sensors": ["temperature", "humidity", "illuminance", "motion_near", "motion_far",
                   "supply_voltage", "usb_voltage", "battery_voltage"],
       "beeper": true,
       "image_formats": ["jpeg", "png"],
       "ota": false,
       "reboot": true,
       "kinds": ["light", "switch", "scene", "climate", "cover", "sensor", "fan", "lock"]
     }
   }
   ```

   | Field | Meaning |
   |---|---|
   | `proto` | Protocol major version. |
   | `paired` | The panel holds a pairing key. |
   | `pairing_open` | The panel will accept `pair_begin` (section 6.1). |
   | `nonce` | 16 random bytes, base64, fresh for every connection. |
   | `caps` | What this panel can do. `sensors` lists only sensors that are actually working, so a panel off its base reports none. The Nest Gen 2 lists the base's sensors whenever it talks to its base, even before the base has answered; until it does, `sensors` messages leave those readings out (section 17). `ota`: the panel takes firmware updates (section 19). `reboot`: it reboots when asked (section 18); firmware 0.2.0 and older leave it out. `kinds`: the item kinds the panel draws (section 9.1); HA sends only those, and falls back for the rest as appendix A says. Firmware 0.2.1 and older leave it out: they draw the first six kinds. |
   | `insecure` | Present and true only on development builds (section 7.4). |

3. HA checks `proto` (section 22). Then:
   - if HA has a key for this `panel_id` and `paired` is true, it authenticates
     (section 7);
   - otherwise it pairs (section 6), if the user is adding the panel.

The panel MUST close a connection that hasn't completed `auth` or a pairing
step within 10 seconds of `hello`, except while waiting for the user during
pairing (section 6.2).

## 6. Pairing

Pairing creates a 32-byte key shared by the panel and one HA instance. It uses
an X25519 key exchange, confirmed by a 6-digit code that the panel shows and the
user types into HA. The code is never sent over the network, so someone who can
watch the network can't learn the key, and someone who sits between the two
produces a different code and is caught.

### 6.1 When pairing is allowed

The panel accepts `pair_begin` only when `pairing_open` is true:

- always, while the panel has no key;
- when it has a key, only for 5 minutes after the user chooses
  **Settings → Pair** ("Pair with Home Assistant") on the panel itself.

Otherwise it answers `pair_begin` with error `pairing_closed`. This stops another
device on the network from silently re-pairing a panel that's already in use.

### 6.2 Steps

1. **H→P `pair_begin`** (request, plaintext):

   ```json
   {"t": "pair_begin", "id": 1, "pub": "<HA's ephemeral X25519 public key, base64>"}
   ```

2. The panel creates its own ephemeral X25519 key pair and computes:

   ```
   S    = X25519(panel_secret, ha_pub)
   code = u64_be(SHA-512("nesto-sas-v1" || ha_pub || panel_pub || S)[0..8]) mod 1 000 000
   ```

   It shows `code` on screen as six digits, zero-padded, and answers:

   ```json
   {"t": "result", "re": 1, "ok": true, "pub": "<panel's ephemeral public key, base64>"}
   ```

   If `S` is all zeros, the other side sent a low-order point: the panel
   answers with error `bad_request` and closes, and HA likewise aborts if it
   computes an all-zero `S` from the panel's key.

3. HA computes the same `S` and `code`. Its config flow asks the user to type
   the code shown on the panel. If it doesn't match, HA sends `pair_cancel`
   (event) and closes the connection. The panel keeps the code on screen for up
   to 120 seconds; after that it drops the pairing state and closes.

4. If the code matches, both sides derive the pairing key:

   ```
   K = HKDF-SHA-512(ikm = S, salt = ha_pub || panel_pub, info = "nesto-pair-key-v1", length = 32)
   ```

   **H→P `pair_confirm`** (request, plaintext):

   ```json
   {"t": "pair_confirm", "id": 2, "mac": "<HMAC-SHA-512(K, \"nesto-confirm-ha-v1\"), base64>"}
   ```

5. The panel checks the MAC. If it's correct, it stores `K` (replacing any old
   key), shows "Paired", and answers:

   ```json
   {"t": "result", "re": 2, "ok": true, "mac": "<HMAC-SHA-512(K, \"nesto-confirm-panel-v1\"), base64>"}
   ```

   If not, it answers with error `auth_failed` and keeps its old key.

6. HA checks the panel's MAC and stores `K` in the config entry. **The panel
   then closes the connection.** HA reconnects and authenticates normally, so
   there is only one path into an encrypted session.

### 6.3 Unpairing

- **H→P `unpair`** (request, encrypted session only): the panel deletes its
  key, answers, and closes. HA sends this when the user deletes the config
  entry, if the panel is reachable.
- On the panel, **Settings → Forget**, once confirmed, deletes the key and
  closes the connection. HA will
  then fail to authenticate and SHOULD start a re-pair flow (a repair issue).

## 7. Authentication and encryption

### 7.1 Authentication

After `hello`, a paired HA sends **H→P `auth`** (request, plaintext):

```json
{"t": "auth", "id": 1, "nonce": "<16 random bytes, base64>", "mac": "<base64>"}
```

where

```
mac = HMAC-SHA-512(K, "nesto-auth-ha-v1" || panel_nonce || ha_nonce || panel_id)
```

`panel_id` is the ASCII string from `hello`. If the MAC is right, the panel
answers:

```json
{"t": "result", "re": 1, "ok": true, "mac": "<HMAC-SHA-512(K, \"nesto-auth-panel-v1\" || ha_nonce || panel_nonce || panel_id), base64>"}
```

HA MUST check this MAC before trusting the session. A wrong MAC on either side
closes the connection. After three failed `auth` attempts the panel waits
30 seconds before accepting the next one.

### 7.2 Session keys

Both sides derive one key per direction:

```
k_hp = HKDF-SHA-512(ikm = K, salt = panel_nonce || ha_nonce, info = "nesto-session-h2p-v1", length = 32)
k_ph = HKDF-SHA-512(ikm = K, salt = panel_nonce || ha_nonce, info = "nesto-session-p2h-v1", length = 32)
```

### 7.3 Encrypted frames

Every frame after the successful `auth` result, in both directions, is
encrypted with ChaCha20-Poly1305 (RFC 8439, the IETF variant):

- **Key:** `k_hp` for frames from HA, `k_ph` for frames from the panel.
- **Nonce:** 12 bytes: 4 zero bytes, then a 64-bit little-endian counter. Each
  direction keeps its own counter, starting at 0 and increasing by 1 per frame.
  The counter is never sent; a missing or reordered frame fails decryption.
- **Associated data:** the frame's 4-byte length field.
- **Payload:** the ciphertext followed by the 16-byte tag. The length field
  counts both.

A frame that fails decryption closes the connection.

### 7.4 Development mode

Simulator builds MAY offer a `--insecure` flag that skips pairing, `auth` and
encryption, sending `"insecure": true` in `hello`. HA MUST refuse an insecure
panel unless its own developer option is turned on. Panel builds for real
hardware MUST NOT include this mode.

### 7.5 Primitives and libraries

| Primitive | Panel (C++) | HA (Python) |
|---|---|---|
| X25519, ChaCha20-Poly1305 IETF, SHA-512, HMAC-SHA-512, HKDF-SHA-512 | Monocypher 4 (BSD-2-Clause or CC0) | `cryptography`, already a Home Assistant dependency |
| Randomness | `/dev/urandom` | `secrets` |

SHA-512 is used throughout because Monocypher provides it but not SHA-256.

Monocypher's incremental AEAD interface re-keys itself after each message, so
only the first message after `crypto_aead_init_ietf` matches RFC 8439. The
panel therefore initialises a fresh context for every frame, with that frame's
nonce.

### 7.6 Reference implementation and test vectors

`tools/nesto_protocol.py` implements sections 3, 6 and 7 in Python, and
`tools/make_vectors.py` generates `tests/vectors/protocol-v1.json` from it:
fixed keys and nonces with the resulting pairing code, keys, MACs and encrypted
frames, plus RFC 8439's own example. The panel's tests check against that file,
and Home Assistant's side checks against the same file, so the two agree byte
for byte. CI regenerates the file and fails if it differs from the committed
one.

## 8. Synchronisation

Right after authentication, HA sends, in this order:

1. `time` (section 16)
2. `config` (section 12)
3. `layout` (section 9)
4. `states` with `"full": true` (section 10)

The panel shows its "connecting" screen until it has received all four, then
sends **P→H `ready`** (event):

```json
{"t": "ready"}
```

Only after `ready` does HA forward input events and notifications to
automations. HA resends any of the four whenever they change.

After `config`, HA also sends `ha_info` (section 18), for the panel's
Technical info screen. It isn't needed for `ready`, and a panel that doesn't
get it shows no versions.

## 9. Layout

**H→P `layout`** (event) replaces the whole layout. HA sends it after
synchronisation and whenever the set of items, their names, icons or features
change.

```json
{
  "t": "layout",
  "rev": 7,
  "title": "Living room",
  "items": [
    {"key": "a1", "kind": "light", "name": "All lights", "icon": "lightbulb-group",
     "features": {"brightness": true}},
    {"key": "l1", "kind": "light", "name": "Ceiling", "icon": "ceiling-light",
     "features": {"brightness": true, "color_temp": {"min_k": 2200, "max_k": 6500},
                  "color": true, "effects": ["Candle", "Fireplace", "Sunrise"]}},
    {"key": "c1", "kind": "climate", "name": "Radiator", "icon": "thermostat",
     "features": {"target": "single", "min": 5, "max": 30, "step": 0.5, "unit": "°C",
                  "hvac_modes": ["off", "heat", "auto"], "presets": ["eco", "comfort"],
                  "fan_modes": []}},
    {"key": "v1", "kind": "cover", "name": "Window blind", "icon": "blind",
     "features": {"open": true, "close": true, "stop": true, "position": true, "tilt": false}},
    {"key": "s1", "kind": "scene", "name": "Film night", "icon": "scene"},
    {"key": "w1", "kind": "switch", "name": "Fan", "icon": "fan"},
    {"key": "t1", "kind": "sensor", "name": "CO₂", "icon": "sensor", "features": {"unit": "ppm"}}
  ]
}
```

| Field | Meaning |
|---|---|
| `rev` | Increases with every layout HA sends for this panel. States always refer to the latest layout. |
| `title` | Shown on the home screen: the area name in room mode, the config entry's title in manual mode. |
| `items` | In display order. HA decides the order. |
| `key` | Opaque, 1–8 ASCII characters, unique in the layout. HA maps keys to entity IDs; the panel never sees entity IDs. |
| `kind` | One of the kinds below. The panel MUST skip items with a kind it doesn't know. |
| `name` | Display name, UTF-8. |
| `icon` | A built-in icon name (appendix C). Optional; the panel picks a default from `kind` if it's missing or unknown. |
| `features` | Per kind, below. Missing features are false or empty. |

### 9.1 Kinds and features

| Kind | Features | Notes |
|---|---|---|
| `light` | `brightness`: bool · `color_temp`: `{min_k, max_k}` or absent · `color`: bool · `effects`: list of names | Every light can turn on and off. HA may also add a synthetic "all lights" item that controls a group. |
| `switch` | none | On/off things: `switch`, `input_boolean`, simple fans. |
| `scene` | none | Activate only: `scene` and `script`. |
| `climate` | `target`: `"single"`, `"range"` or `"none"` · `min`, `max`, `step` · `unit` · `hvac_modes`, `presets`, `fan_modes`: lists | `hvac_modes` values: `off`, `heat`, `cool`, `heat_cool`, `auto`, `dry`, `fan_only`. Presets and fan modes are display strings HA passes through. |
| `cover` | `open`, `close`, `stop`, `position`, `tilt`: bools | |
| `sensor` | `unit`: string | Read-only. |
| `fan` | `speed`: bool · `speed_step`: percent between speeds (`33.33` for three; default 1) · `presets`: list · `oscillate`: bool · `direction`: bool | Every fan can turn on and off. Since firmware 0.3.0. |
| `lock` | `open`: bool (it can open the latch, not only unlock) | Every lock can lock and unlock. The panel asks before unlocking or opening. Since firmware 0.3.0. |

Kinds planned for later versions: `media_player`. A new kind needs no
protocol change: old panels skip it, and `caps.kinds` tells HA which kinds a
panel draws.

## 10. State

**H→P `states`** (event) carries the state of one or more items:

```json
{
  "t": "states",
  "full": false,
  "items": {
    "l1": {"on": true, "brightness": 64, "color_temp_k": 2700},
    "c1": {"current": 20.5, "target": 21}
  }
}
```

- With `"full": true`, the message holds every item in the layout and replaces
  everything the panel knew. HA sends it after synchronisation and after every
  `layout`.
- With `"full": false`, each item lists only the fields that changed.
- Every item MAY carry `"available": false`. The panel then greys it out and
  ignores input on it. Missing `available` means true.

Fields per kind:

| Kind | Fields |
|---|---|
| `light` | `on`: bool · `brightness`: 0–100 · `color_temp_k`: int or null · `hs`: `[hue 0–360, saturation 0–100]` or null · `effect`: string or null · `mode`: `"color_temp"`, `"color"` or `"brightness"`, whichever the light is currently in |
| `switch` | `on`: bool |
| `scene` | none (scenes are stateless; the panel shows a short confirmation after activating one) |
| `climate` | `hvac_mode` · `hvac_action`: Home Assistant's value passed through (`"heating"`, `"cooling"`, `"idle"`, `"off"`, `"preheating"`, …) or null · `current`: number or null · `target`: number or null · `target_low`, `target_high`: number or null · `preset`, `fan_mode`: string or null · `current_humidity`: number or null |
| `cover` | `state`: `"open"`, `"closed"`, `"opening"`, `"closing"` · `position`: 0–100 or null (100 = fully open) · `tilt`: 0–100 or null |
| `sensor` | `value`: string, already formatted by HA |
| `fan` | `on`: bool · `percentage`: 0–100 or null · `preset`: string or null · `oscillating`: bool · `direction`: `"forward"`, `"reverse"` or null |
| `lock` | `state`: `"locked"`, `"unlocked"`, `"locking"`, `"unlocking"`, `"jammed"`, `"open"`, `"opening"`, or null when unknown |

Brightness is a percentage on the wire because that's what the panel shows; HA
converts to and from Home Assistant's 0–255 scale.

## 11. Intents

**P→H `intent`** (request) asks HA to act on an item:

```json
{"t": "intent", "id": 12, "key": "l1", "action": "turn_on", "args": {"brightness": 40}}
```

HA translates the intent into a service call and answers once the call returns.
The result says only whether the call succeeded; the new state arrives in a
separate `states` message. On error, the panel shows a short message and
restores the item's last known state.

| Kind | Action | `args` |
|---|---|---|
| `light` | `toggle` | none |
| | `turn_on` | Optional, at most one of `brightness`, `color_temp_k`, `hs`, `effect` |
| | `turn_off` | none |
| `switch` | `toggle`, `turn_on`, `turn_off` | none |
| `scene` | `activate` | none |
| `climate` | `set_target` | `value` |
| | `set_range` | `low`, `high` |
| | `set_hvac_mode` | `mode` |
| | `set_preset` | `preset` |
| | `set_fan_mode` | `fan_mode` |
| `cover` | `open`, `close`, `stop` | none |
| | `set_position` | `position` 0–100 |
| | `set_tilt` | `tilt` 0–100 |
| `fan` | `toggle`, `turn_on`, `turn_off` | none |
| | `set_percentage` | `percentage` 1–100 |
| | `set_preset` | `preset` |
| | `oscillate` | `oscillating`: bool |
| | `set_direction` | `direction`: `"forward"` or `"reverse"` |
| `lock` | `lock`, `unlock`, `open` | none |

Errors specific to intents: `unknown_key` (stale layout), `unsupported` (the
item lacks the feature), `unavailable`, `service_failed` (HA's call raised).

## 12. Panel configuration

**H→P `config`** (event) carries the panel's settings, which the user edits in
HA. The whole object is sent every time; the panel stores the latest copy in
flash so it can sleep, wake and show the clock before HA connects.

```json
{
  "t": "config",
  "display": {
    "brightness": 80,
    "auto_brightness": true,
    "timeout_s": 30,
    "wake_on_motion": true,
    "wake_on_input_when_off": true
  },
  "standby": {
    "mode": "clock",
    "brightness": 20,
    "clock": {"style": "analog", "seconds": false, "date": true},
    "carousel": {"interval_s": 15}
  },
  "locale": {"language": "es", "hour12": false},
  "sound": {"ring_click": true, "press_click": false, "notifications": true}
}
```

| Field | Meaning |
|---|---|
| `display.brightness` | Active brightness, 0–100. With `auto_brightness`, this is the brightness in a bright room, scaled down using the light sensor. |
| `display.auto_brightness` | Scale `display.brightness` and `standby.brightness` by the room's light. The Nest Gen 2 goes down to a quarter of them in the dark, but not below 5%. Without a light sensor, nothing changes. `display_state` reports the scaled brightness when the state changes, but not each adjustment to the light. |
| `display.timeout_s` | Seconds without input before leaving the active state (section 13). 0 means never. |
| `display.wake_on_motion` | Motion wakes the panel from standby. Ignored without sensors. |
| `display.wake_on_input_when_off` | The ring or a press wakes the panel from **off**. If false, only HA can turn it back on. |
| `standby.mode` | `"clock"`, `"carousel"` or `"off"` (screen off instead of standby). Firmware 0.1.0 has no carousel and shows the clock instead (section 15). |
| `standby.brightness` | Standby brightness, 0–100. |
| `standby.clock.style` | `"analog"` or `"digital"`. |
| `standby.carousel.interval_s` | Seconds per image, minimum 5. |
| `locale.language` | Language for the panel's own texts (`"en"`, `"es"`, …), falling back to English. Firmware 0.1.0 has English texts only and ignores it. |
| `locale.hour12` | 12-hour clock. |
| `sound.ring_click` | Click when a turn of the ring changes what the screen shows, while the display is active: the next item in a list, the next value. Default true. Nothing changes at either end of a list or a value, so no click there either. The turn that wakes the display doesn't click, and a fast turn clicks at most every 15 ms. |
| `sound.press_click` | Click when the display is pressed down while it's active. Default false. |
| `sound.notifications` | Allow notifications to beep. Default true. |

## 13. Display power

The panel's display is always in one of three states:

| State | Screen | Enters when | Leaves when |
|---|---|---|---|
| `active` | Controls, at `display.brightness` | Ring, press, motion (if enabled), a waking notification, or HA's `display` command | `timeout_s` passes without input → `standby`, or `off` if `standby.mode` is `"off"` |
| `standby` | Clock or carousel, at `standby.brightness` | Timeout from `active`, or HA's command | Ring, press, motion (if `wake_on_motion`), a `normal` or higher notification, or HA's command → `active` |
| `off` | Backlight off | HA's `display` command, or timeout when `standby.mode` is `"off"` | Ring or press if `wake_on_input_when_off`; HA's command; high-priority notification |

In `off` and `standby`, the ring or press that wakes the panel only wakes it and
is not passed on to the controls. A double press doesn't wake it; it is only
reported to HA (section 17).

**H→P `display`** (request) sets the state:

```json
{"t": "display", "id": 30, "state": "off"}
```

`state` is `"active"`, `"standby"` or `"off"`. Setting `"active"` restarts the
timeout.

**P→H `display_state`** (event) reports every change, whatever caused it:

```json
{"t": "display_state", "state": "standby", "brightness": 20, "cause": "timeout"}
```

`cause` is `"input"`, `"motion"`, `"timeout"`, `"ha"` or `"notification"`, or
`"sync"` for the one the panel sends right after synchronisation, so HA knows
where the display stands. HA exposes this through a `light` entity (on =
`active` or `standby`, with brightness) and a `select` entity for the standby
mode.

Until a panel has a standby screen to show, its `standby` is dark: brightness
0, waking as `standby` does. A panel whose clock waits for the time (section
16) reports `standby` at `standby.brightness` with cause `"ha"` once the
first `time` message arrives. Motion is the base's proximity sensor, on the
thermostat; a press wakes the panel once it's recognised as a single press or
a hold, so a double press can stay a trigger for HA. Woken from `standby` or
`off`, the panel shows the top of its home screen, which is the Notifications
entry while there are any (section 14), whatever screen it rested on.

## 14. Notifications

**H→P `notify`** (request) shows a notification:

```json
{
  "t": "notify",
  "id": 50,
  "nid": "door-1",
  "title": "Front door",
  "message": "Someone is at the door",
  "icon": "door",
  "priority": "high",
  "duration_s": 30,
  "beep": "alert",
  "actions": [
    {"id": "open", "label": "Open"},
    {"id": "ignore", "label": "Ignore"}
  ]
}
```

| Field | Meaning |
|---|---|
| `nid` | Notification ID, 1–64 characters. A new `notify` with an existing `nid` replaces that notification. HA generates one if the caller didn't. |
| `title` | Optional. |
| `message` | Required, up to 256 characters. |
| `icon` | Optional built-in icon. |
| `priority` | `"low"`, `"normal"`, `"high"` or `"critical"` (table below). Default `"normal"`. |
| `duration_s` | Seconds on screen before it moves to the list. 0 means until dismissed. Default 15. |
| `beep` | `"none"`, `"short"` or `"alert"`. Default `"short"`. Silenced if `sound.notifications` is false, except for `critical`. |
| `actions` | Up to 3 buttons. The user turns the ring to choose and presses to send. |

| Priority | Wakes from `standby` | Wakes from `off` | On screen |
|---|---|---|---|
| `low` | No | No | Only a badge on the home screen; listed when opened |
| `normal` | Yes | No | Pop-up for `duration_s` |
| `high` | Yes | Yes | Pop-up for `duration_s` |
| `critical` | Yes | Yes | Stays until dismissed; repeats the alert beep every 30 seconds |

The panel keeps the 10 most recent notifications. When the list is full, the
oldest non-critical one is dropped. They are kept in memory only: a panel
that restarts has none, and HA may send them again.

On the panel:

- A notification pops up only while the display is lit, after waking it as
  the table says. One that arrives while the display stays dark, a `normal`
  one while it's off, goes to the list unread.
- The ring chooses among the actions and **Dismiss**, and a press sends the
  choice. Either one removes the notification: an action sends `action` only,
  not `dismissed` as well. With only Dismiss to choose, the ring scrolls a
  message too long for the screen.
- A long press closes the pop-up and keeps the notification in the list. A
  critical one can't be closed that way: it has to be answered or dismissed.
- A newer pop-up replaces the one on screen, except a critical one, which
  stays until it's answered. A pop-up on a timer keeps the display lit for
  its time. One without a time limit (critical, or `duration_s` 0) lets the
  display rest as usual and pops up again when it wakes.
- The home screen starts with a Notifications entry while any are kept,
  showing how many are unread. It opens the list, newest first, where a press
  opens one and a long press goes back. A notification opened there is
  interrupted only by a critical one, and is back on screen once that's
  answered. A pop-up that comes over the list goes back to the list once it's
  answered or its time is up.
- `beep` plays when the notification arrives, unless `sound.notifications` is
  false; a critical one beeps regardless. A critical notification whose
  `beep` isn't `"none"` plays its alert again every 30 seconds until it's
  answered or dismissed, also while the display rests.

**H→P `notify_clear`** (event) removes notifications:
`{"t": "notify_clear", "nid": "door-1"}`, or `{"t": "notify_clear", "all": true}`.

**P→H `notify_event`** (event) reports what happened:

```json
{"t": "notify_event", "nid": "door-1", "event": "action", "action": "open"}
```

`event` is `"shown"`, `"dismissed"`, `"expired"` or `"action"` (with `action`).
`shown` is sent once, when the notification first appears on screen, as a
pop-up or opened from the list. `expired` is sent when a pop-up on a timer
leaves the screen without an answer, because its time is up or the display
rested; the notification stays in the list. `notify_clear` sends no events.
HA turns these into events on an `event` entity, so automations can react to a
button pressed on the panel.

## 15. Images for the carousel

> Not in firmware 0.1.0, nor in ha-nesto 0.1.0: the panel never asks for
> images, and shows the clock instead.

The panel pulls images; HA never pushes them. This lets the panel stay within
its memory budget and stop fetching when it isn't showing the carousel.

**P→H `image_next`** (request):

```json
{"t": "image_next", "id": 60, "after": "img-41"}
```

`after` is the ID of the last image the panel received, or absent to start
from the beginning. HA answers with the next image:

```json
{
  "t": "result", "re": 60, "ok": true,
  "image": {"img_id": "img-42", "format": "jpeg", "width": 320, "height": 320,
            "data": "<base64>"}
}
```

- HA picks images from the folder the user chose in Home Assistant's media
  browser, crops and scales them to exactly 320×320, and encodes JPEG at a
  quality that keeps `data` under the frame limit.
- The panel keeps at most 3 decoded images (shown, next, previous) and requests
  the next one about 5 seconds before it's needed.
- If HA answers with error `no_images`, or isn't connected, the panel falls
  back to the clock until the next attempt, 60 seconds later.

The panel MAY cache a few images in flash in a later version, so the carousel
keeps going while HA is offline. That needs no protocol change.

## 16. Time

**H→P `time`** (event) gives the panel the time and time zone:

```json
{"t": "time", "utc_ms": 1789000000000, "utc_offset_min": 60, "tz": "Europe/Madrid"}
```

- HA sends it after synchronisation, every hour, and right after every UTC
  offset change in its time zone (daylight saving), so the panel never needs a
  time zone database.
- The panel keeps time from its monotonic clock between messages and also sets
  the system clock. If the hardware clock keeps time across reboots, as the
  Nest Gen 2's does, the panel can show the clock right after booting, using
  the last offset stored in flash, before HA connects. Otherwise it shows no
  clock until the first `time` message.
- `tz` is informational; the panel only uses `utc_offset_min`.

## 17. Sensors and input events

**P→H `sensors`** (event) reports the base's sensors:

```json
{
  "t": "sensors",
  "temperature_c": 21.4,
  "humidity_pct": 46.0,
  "illuminance_raw": 312,
  "motion_near": false,
  "motion_far": true,
  "supply_v": 0.0,
  "usb_v": 4.94,
  "battery_v": 3.91
}
```

- Sent once right after synchronisation, then every 30 seconds, and
  immediately whenever `motion_near` or `motion_far` changes.
- `motion_near` is the proximity sensor: on at once when something is within
  about a hand's width of the glass, off 5 s after the last close reading.
  `motion_far` is the room's motion sensor: on at once when someone moves, off
  30 s after the last movement, since that sensor sends nothing while the
  room is still. HA can add its own delay on top.
- The base measures its two supplies separately, and both are reported:
  `supply_v` is the voltage from the heating's wires, and `usb_v` is from a
  USB supply, as on a desk stand. Either can be 0 while the other powers the
  panel. `battery_v` is the head unit's battery, as the base measures it.
- Rounding: temperature to 0.01 °C, humidity to 0.1%, voltages to 0.01 V.
- A field is left out when its sensor isn't available, for instance with no
  base attached.
- `temperature_c` is the raw reading. The head unit warms itself and raises
  the reading; HA applies an offset the user can calibrate.
- `illuminance_raw` is the light sensor's raw value, higher brighter. On a Nest
  Gen 2 it read about 100 in a dark room, 2000 in a lit one and 19000 with a
  torch at the glass (docs/BACKPLATE.md); mapping it to lux needs a lux meter.

**P→H `input`** (event) reports a press gesture:

```json
{"t": "input", "gesture": "double", "context": "standby"}
```

- `gesture` is `"single"`, `"double"` or `"long"`.
- `context` is where it happened: `"ui"` (it also acted on the controls),
  `"standby"` or `"off"`.
- A double press is two presses within 350 ms. It is only recognised in
  `standby` and `off`, never in `ui`, so `"double"` always comes with one of
  those two contexts. It is purely an automation trigger: it doesn't wake the
  panel, so an automation can, for example, turn off the room while the panel
  stays asleep. A single or long press in `standby` or `off` wakes the panel as
  section 13 describes.
- In `ui`, presses are reported as `"single"` or `"long"` only, because
  recognising a double press there would delay every single press (section 23).

Ring turns are not reported; they would be too frequent and have no use as
automation triggers. HA exposes gestures on an `event` entity so users can bind
them to anything.

## 18. Maintenance messages

| Message | Direction | Kind | Purpose |
|---|---|---|---|
| `ping` / `pong` | Both | Event | `{"t": "ping", "seq": 5}` answered by `{"t": "pong", "seq": 5}`. HA pings every 20 seconds; the panel pings every 3 seconds while its Technical info screen is open, to show the round trip. In an authenticated session, either side closes the connection after 60 seconds without receiving any frame. Before authentication the timeouts in sections 5 and 6 apply instead. |
| `info` | H→P | Request | Result carries `uptime_s`, `free_ram_kb`, `wifi_rssi`, `wifi_ssid`, `wifi_freq_mhz`, `ip`, `fw`, `base_connected` and `base_fw`, for HA's diagnostic entities. `wifi_rssi` is the Wi-Fi signal in dBm; it and the Wi-Fi fields are null without Wi-Fi. |
| `ha_info` | H→P | Event | `{"t": "ha_info", "ha_version": "2026.11.1", "integration_version": "0.1.0"}`, sent after `config` in synchronisation (section 8), for the panel's Technical info screen. Each at most 32 characters. |
| `identify` | H→P | Request | Wakes the display, pulses the backlight and beeps three times, over about 3 seconds. |
| `reboot` | H→P | Request | The panel answers, shows that it's restarting, and within a few seconds reboots: the Nest Gen 2 hands its base back as the stock software leaves it, then reboots the thermostat, which runs Nesto again about a minute later. During a firmware update, or once asked, it answers `bad_request` instead. A panel reports it can in `hello.caps.reboot`; firmware 0.2.0 and older answer `unsupported`. |
| `unpair` | H→P | Request | See section 6.3. |

## 19. Firmware update

HA can replace the panel's program. The panel trusts a program only when a
release key compiled into it signed it, so HA, or anything else on the
network, only carries the bytes. A panel takes updates when `hello` reports
`"ota": true`: on the Nest Gen 2, when the boot script runs Nesto and the
build trusts a release key.

**What's signed.** An Ed25519 signature over this text, each line ending in
`\n`, the SHA-512 in lower-case hex:

```
nesto-ota-v1
model=nest-gen2
version=0.2.0
size=2410424
sha512=8f3a…
```

A release publishes the program and its signature, in base64, as
`nesto-VERSION-MODEL.bin` and `nesto-VERSION-MODEL.bin.sig`.

**The messages**, all H→P requests, in this order, one at a time:

| Request | Fields | The panel |
|---|---|---|
| `ota_begin` | `model`, `version`, `size`, `sha512` (hex), `signature` (base64) | Checks the model, that `version` isn't older than the one running, the size (at most 16 MiB), the signature and the free space, and starts the new program beside the running one. A new `ota_begin` drops an update in progress. |
| `ota_chunk` | `offset`, `data` (base64, 1 byte to 64 KiB decoded) | Writes the next piece. `offset` is where it starts, so pieces come in order. |
| `ota_end` | | Checks all of it arrived and its SHA-512 matches. |
| `ota_apply` | | Answers, puts the new program in place, keeping the running one, and restarts with it within a few seconds. |

Errors: `unsupported` (no updates, or another model), `downgrade` (older than
the one running), `bad_signature` (no trusted key signed it, or the program
doesn't match its SHA-512), `too_large`, `no_space`, `bad_request` (missing
fields, a piece out of order, a request out of turn). An update with nothing
arriving for 60 seconds is dropped.

**Rollback.** The new program has to confirm itself: once it's `ready` with
HA (section 8), it does. If it doesn't within 5 minutes, or fails to start
twice, the panel goes back to the program it replaced. So HA, after
`ota_apply`, sees the panel go, come back, and report the new version in
`hello.fw`; or the old one, if the new one didn't work.

The panel shows an Updating screen, with the progress, from `ota_begin` to
the restart. Firmware can also be installed over SSH (docs/INSTALL.md), with
no checks: whoever has root on the panel can replace its program anyway.

## 20. Errors

| Code | Meaning |
|---|---|
| `bad_request` | Missing or invalid fields. |
| `unknown_type` | The receiver doesn't know this request type. |
| `pairing_closed` | `pair_begin` while pairing isn't open. |
| `auth_failed` | Wrong MAC in `auth` or `pair_confirm`. |
| `not_ready` | Request before synchronisation finished. |
| `unknown_key` | No item with this key in the current layout. |
| `unsupported` | The item doesn't support this action. |
| `unavailable` | The entity is unavailable in Home Assistant. |
| `service_failed` | Home Assistant's service call raised an error; `message` has the reason. |
| `no_images` | No carousel images are configured or found. |
| `too_large` | A field or the frame exceeds the limits. |
| `bad_signature` | A firmware update no trusted key signed, or that doesn't match its SHA-512 (section 19). |
| `downgrade` | A firmware update older than the program running (section 19). |
| `no_space` | Not enough room for a firmware update (section 19). |
| `internal` | Anything else. |

## 21. Limits

| Limit | Value |
|---|---|
| Frame payload | 512 KiB |
| Items per layout | 48 |
| `name`, `title` | 64 characters |
| Effects, presets, fan modes per item | 32 each |
| Notification `message` | 256 characters |
| Notifications kept | 10 |
| Image `data` decoded | 256 KiB |
| Open requests per side | 16 |

Text uses UTF-8. The panel's fonts cover Latin-1 and Latin Extended-A, which
includes Spanish and Esperanto. Characters outside that set are shown as a
placeholder.

## 22. Versioning

- `proto` in `hello` is the major version. It changes only for incompatible
  changes. HA refuses a panel whose major version it doesn't support, and raises
  a repair issue asking the user to update the panel or the integration.
- Compatible additions (new optional fields, new events, new item kinds) don't
  change `proto`. Receivers ignore what they don't know (section 4).
- New capabilities are advertised in `hello.caps`, so HA only uses what the
  panel says it supports.

## 23. Panel behaviour requirements

These rules are part of the contract because HA's load and the user's
experience depend on them.

- **Optimistic display.** When the user turns the ring, the panel changes the
  value on screen at once and sends intents as follows:
  - lights, covers and fans' speed: at most one intent every 300 ms while
    turning, and a final one 300 ms after the ring stops, so lights follow the
    ring live;
  - climate: one intent 1.5 seconds after the ring stops, so a thermostat isn't
    sent every half-degree step;
  - locks: locking at once; unlocking and opening only after the user confirms
    on the panel, with a press.
- **States win.** A `states` message for an item overrides the optimistic value,
  except for the item being adjusted right now and for 2 seconds after the last
  turn, to stop the value jumping back while the user is still turning.
- **Ring and press.** Turning moves the selection or changes the value. A single
  press opens an item or steps to its next control. A long press (over 800 ms)
  goes back, or toggles on the home screen. A single press acts as soon as it is
  released: the controls don't use double presses, because telling one apart
  would make every single press wait 350 ms. In `standby` and `off`, the panel
  does wait those 350 ms, which costs nothing there, and reports a double
  press to HA without waking (section 17). The exact controls are the
  firmware's choice; only gestures in section 17 are visible to HA.
- **Disconnected.** If HA disconnects, the panel shows a small "offline" mark,
  greys out all items and keeps the display-power logic and clock running from
  its stored `config`. It doesn't queue intents for later.
- **Pairing and settings screens.** The panel's own menu (reached from the home
  screen) has at least: Pair with Home Assistant, Forget Home Assistant, Wi-Fi
  and IP information, firmware version, and Reboot.

## Appendix A: how HA maps entities to items

This appendix is informative; it describes `ha-nesto` so the panel side knows
what to expect.

**Which entities.** The integration's options hold one ordered list of
entities per kind (lights, climate, covers, switches, scenes, sensors). In
room mode the lists start as every entity whose own area, or whose device's
area, is the panel's area, in supported domains, except sensors, which the
user adds; entities added to the area later come after the listed ones, and
the ones the user took off stay off. In manual mode, the lists are what the
user picked.

**Domains to kinds.**

| Domain | Kind | Features from |
|---|---|---|
| `light` | `light` | `supported_color_modes`: any of `hs`, `xy`, `rgb`, `rgbw`, `rgbww` gives `color`; `color_temp` gives `color_temp` with `min_color_temp_kelvin` and `max_color_temp_kelvin`; anything other than `onoff` gives `brightness`. `effect_list`, minus "off"/"none" entries, gives `effects`. |
| `switch`, `input_boolean` | `switch` | none |
| `fan` | `fan` | `FanEntityFeature` `SET_SPEED` gives `speed`, with `percentage_step`; `PRESET_MODE` gives `presets` from `preset_modes`; `OSCILLATE` and `DIRECTION`. On a panel without `fan` in `caps.kinds`, a fan is a `switch`. |
| `scene`, `script` | `scene` | none |
| `climate` | `climate` | `ClimateEntityFeature`: `TARGET_TEMPERATURE` gives `single`, `TARGET_TEMPERATURE_RANGE` gives `range`; `hvac_modes`, `preset_modes`, `fan_modes`; `min_temp`, `max_temp`, `target_temperature_step` (default 0.5); unit from Home Assistant's unit system |
| `cover` | `cover` | `CoverEntityFeature` `OPEN`, `CLOSE`, `STOP`, `SET_POSITION`, `SET_TILT_POSITION` |
| `lock` | `lock` | `LockEntityFeature` `OPEN` gives `open`. A lock with a `code_format` is left out unless it has a default code in Home Assistant, which then fills it in; the panel never asks for a code. On a panel without `lock` in `caps.kinds`, left out. |
| `sensor` (picked by the user) | `sensor` | unit from the entity |

**Colour.** The panel always works in hue and saturation. HA converts to and
from whatever colour mode the light uses.

**Order.** Kind by kind: lights, climate, covers, locks, switches (fans
among them), scenes and sensors; within a kind, the user's order. With the user's "All lights"
option and two or more lights, a synthetic "all lights" item comes first.

## Appendix B: example session

```
HA → panel   (TCP connect)
P→H  hello        {"proto": 1, "panel_id": "18b4300a1b2c", "paired": true, "nonce": "…", …}
H→P  auth         {"id": 1, "nonce": "…", "mac": "…"}
P→H  result       {"re": 1, "ok": true, "mac": "…"}
                  ── everything below is encrypted ──
H→P  time         {"utc_ms": 1789000000000, "utc_offset_min": 60, "tz": "Europe/Madrid"}
H→P  config       {"display": {…}, "standby": {"mode": "clock", …}, …}
H→P  layout       {"rev": 7, "title": "Living room", "items": [ … ]}
H→P  states       {"full": true, "items": {"l1": {"on": false, …}, …}}
P→H  ready        {}
                  (user turns the ring on "Ceiling")
P→H  intent       {"id": 12, "key": "l1", "action": "turn_on", "args": {"brightness": 30}}
P→H  intent       {"id": 13, "key": "l1", "action": "turn_on", "args": {"brightness": 55}}
H→P  result       {"re": 12, "ok": true}
H→P  result       {"re": 13, "ok": true}
H→P  states       {"items": {"l1": {"on": true, "brightness": 55}}}
                  (30 s without input)
P→H  display_state {"state": "standby", "brightness": 20, "cause": "timeout"}
                  (automation sends a notification)
H→P  notify       {"id": 50, "nid": "door-1", "message": "Someone is at the door", "priority": "high", …}
P→H  display_state {"state": "active", "brightness": 80, "cause": "notification"}
P→H  result       {"re": 50, "ok": true}
P→H  notify_event {"nid": "door-1", "event": "shown"}
H→P  ping         {"seq": 5}
P→H  pong         {"seq": 5}
```

## Appendix C: built-in icons

The panel ships a fixed icon set. HA maps an entity's Material Design icon, or
its device class, to the closest name here and leaves `icon` out when nothing
fits.

`lightbulb`, `lightbulb-group`, `ceiling-light`, `lamp`, `led-strip`,
`switch`, `power-socket`, `fan`, `scene`, `script`, `thermostat`, `radiator`,
`air-conditioner`, `blind`, `shutter`, `curtains`, `garage`, `window`, `door`,
`sensor`, `thermometer`, `water`, `tv`, `speaker`, `bell`, `alert`, `info`,
`lock`, `lock-open`.
