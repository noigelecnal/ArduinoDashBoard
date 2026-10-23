# BioPod Container Showcase — System Documentation

*Luis Y. Ferrer Jr. SHS · STEM Capstone*

This document explains how the whole project works, from the sensors inside the chamber to the pixels in the browser. It is written from the actual source code in `ArduinoDashboard/`, and file names are given so every claim can be checked.

---

## 1. What this project is

**BioPod** is an IoT incubator for bacterial culture. An ESP32 reads four temperature/humidity sensors inside a 35 × 35 × 35 cm chamber, switches a heater, a Peltier cooler, a blower fan and a humidifier to hold a setpoint (default **37 °C, 50 % RH**), and reports to a PC over Wi-Fi.

The **3D model website showcase** (`3dmodelwebsiteshowcase/`) is the web application that goes with it. It combines two things in one page:

1. An **interactive 3D model** of the incubator (open/close the lid, exploded view, hide parts, click a part to read what it does).
2. A **live monitoring dashboard** (sensor readings, actuator states, setpoints, charts, alerts, the paper's performance metrics, a manual log, and the 30-minute trial log).

A small **Python server** sits between the ESP32 and the browser. It also keeps the official 30-minute log so logging never depends on a browser tab being open.

> BioPod is a student prototype for educational incubation monitoring. It is not a certified laboratory incubator (the dashboard footer says so).

---

## 2. Architecture at a glance

```
 ┌───────────────────── BioPod chamber (35×35×35 cm) ─────────────────────┐
 │  4× AM2302B (DHT22) sensors ──► ESP32 ──► MOSFETs ──► PTC heater        │
 │                                  (control loop)       Peltier TEC1-12706│
 │                                                       5010 blower fan   │
 │                                                       Ultrasonic atomizer│
 └──────────────────────────────────┬──────────────────────────────────────┘
                                    │ Wi-Fi · HTTP POST /api/reading every 5 s
                                    │ (reply carries the current setpoints)
                                    ▼
 ┌──────────────────────────── server.py  (port 8080) ─────────────────────┐
 │  latest reading · setpoints · 30-minute logger thread · CSV export      │
 │  state saved to ./data/*.json · serves the built React app from ./dist  │
 └──────────────────────────────────┬──────────────────────────────────────┘
                                    │ GET /api/reading  (every 2 s)
                                    │ GET /api/log      (every 15 s)
                                    │ POST /api/setpoints (when user applies)
                                    ▼
 ┌──────────────────────────── Browser (React app) ────────────────────────┐
 │  useBiopod hook ─► KPIs · chamber map · actuators · charts · alerts     │
 │  Viewer3D (Three.js) ─► interactive model     Touchscreen view (iframe) │
 └─────────────────────────────────────────────────────────────────────────┘
```

Key design decision: **the server is the source of truth** for setpoints and for the official log. The ESP32 and every browser/touchscreen view follow it.

---

## 3. Repository layout

```
ArduinoDashboard/
├── Project/                         ← the ORIGINAL standalone dashboard (legacy)
│   ├── index.html, script.js, style.css   plain HTML + Chart.js (CDN)
│   ├── server.py                    older server (same API, keeps files next to itself)
│   ├── touchscreen/                 original touchscreen page
│   ├── esp32/biopod/biopod.ino      ESP32 firmware  ← still the firmware in use
│   ├── telemetry.json               leftover test reading
│   └── _post_test.py                posts one fake reading to the server
└── 3dmodelwebsiteshowcase/          ← the CURRENT app (this document)
    ├── server.py                    BioPod server (API + serves ./dist)
    ├── vite.config.js               dev server, proxies /api → port 8080
    ├── package.json                 scripts and dependencies
    ├── index.html                   Vite entry page
    ├── public/touchscreen/          touchscreen UI (standalone page)
    ├── scripts/verify.mjs           headless browser smoke test
    └── src/
        ├── main.jsx                 mounts <App/>
        ├── App.jsx                  page layout, tabs, KPI row, wiring
        ├── config/modelConfig.js    3D model + component configuration
        ├── lib/biopod.js            constants and pure helper functions
        ├── hooks/useBiopod.js       all BioPod state and logic
        └── components/
            ├── Viewer3D.jsx         Three.js canvas, lights, camera
            ├── BlenderModel.jsx     loads the .glb file
            ├── DemoContainer.jsx    procedural placeholder model
            ├── ContainerModel.jsx   the "rig": animation, explode, highlight
            ├── ModelLabels.jsx      clickable 3D markers
            ├── ViewerControls.jsx   open/close, slider, explode, reset camera
            ├── ComponentPanel.jsx   component list + hide/show
            ├── ComponentInfo.jsx    description of the selected part
            ├── BiopodDashboard.jsx  chamber map, actuators, charts, alerts, logs
            ├── TrendChart.jsx       dependency-free SVG chart
            └── TouchscreenView.jsx  embeds the touchscreen page
```

Tech stack: React 19, Vite, Three.js, React Three Fiber, Drei, Lucide icons (see `package.json`). The backend uses only the Python standard library.

---

## 4. The hardware layer (ESP32 firmware)

File: `Project/esp32/biopod/biopod.ino`

### 4.1 Parts and pins

| Part | Role | ESP32 pin |
| --- | --- | --- |
| 4 × AM2302B (read as DHT22) | Temperature and humidity at four points | 14, 27, 26, 25 |
| PTC heater | Warms the chamber | 32 |
| Peltier TEC1-12706 | Cools the chamber | 33 |
| 5010 blower fan | Air circulation (always on) | 18 |
| Ultrasonic atomizer | Adds humidity | 19 |

The loads are MOSFET-switched 12 V / 5 V (shown on the dashboard's Actuators panel). A comment in the firmware warns that these pin numbers must be checked against the 4.0-inch touchscreen board actually used, because the display and touch controller occupy several GPIOs.

### 4.2 Control parameters

| Parameter | Value | Meaning |
| --- | --- | --- |
| Setpoint | 37 °C, 50 % RH | Defaults; overwritten by the server |
| Acceptance range | ±0.5 °C, ±5 % RH | What counts as "in range" |
| Temperature hysteresis | ±0.2 °C | Heater on/off band |
| Humidity hysteresis | ±2 % RH | Humidifier on/off band |
| Setpoint safety clamp | 25–45 °C, 20–90 % RH | Remote setpoints outside this are ignored |
| Sample interval | 5 000 ms | DHT22 needs at least 2 s |

### 4.3 The main loop (every 5 s)

1. If Wi-Fi dropped, retry the connection every 10 s (control keeps running either way).
2. Read temperature and humidity from all four sensors. A failed read is `NaN` and is skipped in the average.
3. Compute the average temperature and humidity.
4. Run `controlTemperature()` and `controlHumidity()` (below).
5. Drive the output pins.
6. If Wi-Fi is up and at least one sensor read succeeded, POST the reading to the server.

### 4.4 Control logic (latching hysteresis)

The controller **remembers its state between samples**, so it does not flutter on and off around the threshold.

| Output | Turns ON when | Turns OFF when |
| --- | --- | --- |
| Heater | avg T < setpoint − 0.2 | avg T ≥ setpoint + 0.2 |
| Peltier | avg T > setpoint + 0.5 (upper acceptance limit) | avg T ≤ setpoint + 0.2 |
| Humidifier | avg RH < setpoint − 2 | avg RH ≥ setpoint + 2 |
| Blower fan | always on | — |

Safety rules in the code:
- **Heater and Peltier are never on together** (if both latch, the heater is forced off).
- **Fail-safe:** if no sensor gives a valid reading, heater, Peltier and humidifier are switched off.
- Remote setpoints are range-checked on the ESP32 as well as on the server.

### 4.5 What the ESP32 sends

`POST http://<DASHBOARD_HOST>:8080/api/reading` with JSON:

```json
{
  "time": 1791730555000,
  "temps": [36.8, 37.0, 36.9, 37.1],
  "hums":  [49.0, 50.0, 51.0, 50.0],
  "setpoints": { "temp": 37.0, "hum": 50.0 },
  "actuators": { "heater": false, "peltier": false, "fan": true, "humidifier": false }
}
```

- `time` is epoch milliseconds from NTP (`pool.ntp.org`, `time.google.com`). It is only included once the clock is synced; otherwise the server stamps its own receive time.
- A failed sensor is sent as `null`.
- The server's reply contains the authoritative setpoints, which the ESP32 applies (`applyServerSetpoints`). This is how a setpoint changed in the browser reaches the hardware: **within one sample, up to about 5 s**.

---

## 5. The server (`3dmodelwebsiteshowcase/server.py`)

A threaded `http.server` on **port 8080**, using only the standard library. In production it also serves the built React app from `./dist`; if `dist` does not exist it shows a "web app not built yet" page.

### 5.1 API

| Method and path | Purpose |
| --- | --- |
| `POST /api/reading` | ESP32 telemetry. Reply: `{"ok":true,"setpoints":{…}}` |
| `GET /api/reading` | Latest reading (with receive time and `id`) |
| `GET /api/setpoints` | Current setpoints |
| `POST /api/setpoints` | `{"temp":37,"hum":50}` from the dashboard; rejected with 400 outside 25–45 °C / 20–90 % RH |
| `GET /api/log` | 30-minute log with `expected` and `valid` counters |
| `GET /api/log.csv` | Same log as CSV |
| `POST /api/log/reset` | Start a new trial (clears the log) |

### 5.2 State and persistence

State lives in memory behind a lock and is saved to `./data/`: `telemetry.json` (latest reading), `setpoints.json`, `log.json` (30-minute log). It is reloaded on start, so a restart does not lose the trial.

### 5.3 The 30-minute logger

A background thread wakes every second and implements the paper's logging rules:

- The trial clock starts (`startedAt`) when the **first fresh reading** arrives.
- Slot *n* is due at `startedAt + n × 30 min`.
- A slot is **valid** only if the latest reading is **fresh** (received within 120 s) **and all four sensors recorded both temperature and humidity**. Otherwise a `missed` entry is written.
- A valid entry stores the 8 raw values, average temperature/humidity, uniformity (max − min across sensors) and a status.
- Status is **"Out of range" if ANY single sensor** is outside ±0.5 °C or ±5 % RH of the setpoint, not just the average.
- **Logging completeness = valid slots ÷ expected slots × 100.**

Because this runs on the server, the official log keeps growing even if no browser is open.

---

## 6. The web app's brain: `useBiopod` and `lib/biopod.js`

`src/hooks/useBiopod.js` holds all BioPod state in one place and `src/lib/biopod.js` holds the constants and pure functions. `App.jsx` calls `useBiopod()` once and passes pieces down.

### 6.1 Data sources (three, all end in `pushReading`)

| Source | How it works | Tag on the reading |
| --- | --- | --- |
| **ESP32 (live)** | Browser polls `GET /api/reading` every 2 s and `GET /api/log` every 15 s | `esp32` |
| **Demo telemetry** | A simulated chamber that starts below setpoint, recovers, and drops at tick 18 to trigger an alert; one sample every 2 s | `demo` |
| **Manual entry** | Form for bench tests when the ESP32 is offline (validates 0–60 °C, 0–100 % RH) | `manual` |

Each reading is expanded with: average temperature/humidity, uniformity (range), a status ("In range" / "Out of range" — any sensor outside the acceptance band), and the actuator states.

### 6.2 Protections when ingesting ESP32 data

- Duplicate readings (same `id`, or same ESP32 timestamp) are ignored.
- Readings older than the newest by more than 2 s are ignored.
- **Setpoint hold:** after the user applies new setpoints, setpoints echoed back from the ESP32 are ignored for 15 s (`SETPOINT_HOLD_MS`) so a stale echo cannot overwrite the user's change.
- If the ESP32 sends no actuator states, they are inferred with `nextActuators`, which uses the same hysteresis as the firmware.

### 6.3 The paper's metrics

| Metric | Formula | How the code does it |
| --- | --- | --- |
| **ADT** (alert detection time) | ADT = Ta − To | To = sample time (NTP stamp) at which the chamber first went out of range; Ta = the moment the dashboard receives it. Only live samples (< 15 s old) count. |
| **RT** (recovery time) | RT = Tr − Td | Td = when the user presses **Mark door disturbance**; Tr = first in-range sample after the chamber actually left the range because of that disturbance. |
| **Logging completeness** | valid ÷ expected × 100 | Uses the server's 30-minute log when one exists; falls back to a local estimate for demo/manual sessions. |
| **Uniformity** | max − min across the four sensors | Shown as °C / % RH. |

### 6.4 Setpoints

`applySetpoints` validates (25–45 °C, 20–90 % RH, must differ from current), records a *setpoint change event* (drawn as a vertical marker on the charts and as a row in the log), then `POST`s to `/api/setpoints`. The ESP32 picks the new values up on its next reading.

### 6.5 Persistence in the browser

Readings (max 1 500), setpoints, events, alerts and metrics are saved in `localStorage`. This is a convenience only — **the official record is the server's 30-minute log.** "Clear all data" wipes the browser store, resets the server log and resets setpoints to 37 / 50.

---

## 7. The dashboard UI

All built in `App.jsx` and `components/BiopodDashboard.jsx`, top to bottom:

1. **Top bar** — project name, tagline from the config, a data-source pill ("ESP32 linked", "Demo live", "Standby"…), live clock.
2. **Tabs** — "3D model & dashboard" and "Touchscreen". The tab is stored in the URL hash (`#touchscreen`) so it can be linked.
3. **Status banner** — green/red/neutral "In range / Out of range" with the average values, plus the buttons *Run demo telemetry* and *Mark door disturbance*.
4. **KPI row** — average temperature, average humidity, uniformity, logging completeness, ADT, RT.
5. **Model summary row** — lid state, view mode, hidden parts, selected part.
6. **3D viewer + component panel** (section 8).
7. **Chamber map** — four tiles (S1 upper-left … S4 lower-right), each coloured in range / out of band per sensor.
8. **Actuators panel + setpoint form** — heater, Peltier, blower, atomizer states; temperature/humidity setpoint inputs.
9. **Charts** — temperature and humidity over time (`TrendChart.jsx`): four sensor lines, a bold average line, a shaded ±acceptance band, a dashed setpoint line, vertical markers for setpoint changes, and hover tooltips. Drawn as plain SVG (no chart library).
10. **Alert feed** and **Manual log entry**.
11. **Timestamped log** — live samples and setpoint events (latest 250), **Export CSV** (downloads the server's 30-minute log when a trial is running, otherwise the local readings), **Clear all data**.

---

## 8. The 3D model

### 8.1 Configuration (`src/config/modelConfig.js`)

This one file controls the 3D side:

- `glbUrl` — the model file (`/models/container.glb`).
- `animation` — names of the open/close clips to look for, plus a **fallback** (rotate the lid 110° around its z axis).
- `camera` — start position `[1, 0, 1.5]`, field of view 50, max zoom-out distance 7.
- `COMPONENTS` — **18 parts**. Each has an `id`, a UI `label`, the Blender object names to look for (`meshNames`, first match wins), a plain-language `description`, an `explodeOffset` (where it moves in exploded view) and a `labelAnchor` (where its marker sits).

Parts: container body, lid, inner tray, sensor module, Petri dish, humidifier, water-reservoir lid and bottom, ESP32, 4.0" touchscreen, PTC heater, blower fan, Peltier module, blower heat sink, heat sink, and three wiring runs.

The part descriptions double as design documentation. For example, the heater description states the same ±0.2 °C hysteresis the firmware uses.

### 8.2 Loading (`Viewer3D.jsx`, `BlenderModel.jsx`, `DemoContainer.jsx`)

- `Viewer3D` creates the Three.js canvas with shadows, ambient + directional light, an environment of three soft light panels, contact shadows and orbit controls (drag to rotate, scroll/pinch to zoom, panning disabled, camera cannot go below the floor).
- `BlenderModel` loads the exported `.glb` with `useGLTF` and passes the scene and its animation clips to the rig.
- If `glbUrl` is empty **or loading fails**, an error boundary swaps in `DemoContainer`, a procedural box model whose object names match the config. A banner and a "Placeholder model" tag tell the user.

### 8.3 The rig (`ContainerModel.jsx`) — the heart of the 3D side

When a model loads, `buildRegistry` runs once and, for every entry in `COMPONENTS`:

1. Finds the matching object in the model by name.
2. **Clones its materials**, so highlighting one part never tints another.
3. Adds an invisible **anchor** object inside the part, so its marker follows the part when it moves.
4. Stores the part's original position/rotation and its explode offset.

Then every frame (`useFrame`):

- **Lid open/close.** If the model has an `Open`/`Close` animation clip, the clip is paused and its time is *scrubbed* to match a 0–1 "open amount". One clip works for both directions. If there is no clip but there is a lid object, the lid is rotated about its origin instead. If there is neither, open/close is disabled and a banner explains why.
- **Smooth motion.** The open amount and the explode amount are eased toward their targets with exponential damping, so buttons and the slider glide rather than jump.
- **Exploded view.** Each part moves from its base position by `explodeOffset × explodeAmount`. For parts whose position is driven by an animation clip, last frame's offset is removed before the animation updates, so offsets can never pile up.
- **State reporting.** The rig tells the UI "closed / opening / open / closing / partly open" only when it changes.

Other effects: hiding a part sets `visible = false` (and removes its marker); selecting a part gives its materials a green emissive glow.

### 8.4 Markers and panels

- `ModelLabels.jsx` renders one clickable HTML marker per visible part, pinned to the anchor's world position every frame.
- `ComponentPanel.jsx` lists all parts with a select button and an eye toggle; parts not found in the loaded model are greyed out as "not in model".
- `ComponentInfo.jsx` shows the selected part's description, the model object name it matched, and whether it is hidden.
- `ViewerControls.jsx` has *Open container*, *Close container*, *Exploded view*, *Reset camera* and a lid-position slider.

### 8.5 How to swap in the Blender model

See the existing `README.md` for the full checklist. In short: export a `.glb` with each part as its own top-level object, lid origin on the hinge line, an `Open` animation, scale/rotation applied, +Y up; put it in `public/models/`; name objects as listed in `meshNames`.

---

## 9. The touchscreen view

- `TouchscreenView.jsx` embeds `public/touchscreen/index.html` in an iframe, drawn as a 480 × 320 landscape panel inside a bezel and scaled to fit the available space (0.5× to 1.5×). *Open full screen* opens the page directly.
- The touchscreen page is **standalone** (`touchscreen.js`, written in plain older-style JavaScript). It does its own polling of `/api/reading` and `/api/log`, so it shows the same data and setpoints as the dashboard.
- Three screens: **Home** (average values, four sensor zones, actuator chips), **Set** (large − / + steppers; hold to repeat; *Apply setpoints*; *Reset 37 / 50*), **Status** (actuators, logging %, ADT, RT, latest alert, *Mark door disturbance*).
- Edits are held as a *draft* until Apply is pressed; the same 15 s setpoint-hold protection applies.
- With no server data for 8 s it switches itself to demo readings.

---

## 10. End-to-end walkthroughs

**A. A sensor reading reaches the screen**
1. ESP32 samples the four sensors (every 5 s), runs the control loop, and POSTs JSON.
2. `server.py` stores it as `latest`, saves `telemetry.json`, and replies with the setpoints.
3. Within 2 s the browser's poll picks it up and `ingestPayload` filters duplicates and stale data.
4. `pushReading` computes averages, uniformity and status, runs the alert/recovery logic, saves to `localStorage`, and re-renders KPIs, chamber map, actuators and charts.

**B. The user changes a setpoint**
1. User edits values and presses *Apply* (dashboard or touchscreen).
2. The app validates, logs a setpoint event, and POSTs `/api/setpoints`.
3. The server validates again and saves it.
4. On the ESP32's next POST (≤ 5 s) the reply carries the new setpoints, the firmware range-checks and adopts them, and the control loop now aims at the new target.

**C. Something goes out of range**
1. A sample arrives with any sensor outside ±0.5 °C / ±5 % RH.
2. The first such live sample sets `outOfRangeAt`; ADT = now − sample time is computed, an alert is added to the feed, a toast appears, the status banner and header pill turn red.
3. Meanwhile the firmware's heater or Peltier is already working to bring the average back.
4. When samples return in range, the banner turns green. If a disturbance was marked, RT is computed.

**D. The official 30-minute log**
1. The first fresh reading starts the trial clock.
2. Every 30 minutes the logger writes one entry — valid (fresh, all 4 sensors) or missed.
3. The dashboard polls `/api/log` and shows completeness as `valid ÷ expected`.
4. *Export CSV* downloads `/api/log.csv`.

---

## 11. Running it

```bash
cd 3dmodelwebsiteshowcase
npm install

# Production (what you demonstrate)
npm run build
python server.py          # http://127.0.0.1:8080  — serves the app and takes ESP32 data
# or:  npm run start      # build, then run the server

# Development (hot reload)
python server.py          # API + ESP32 on 8080
npm run dev               # http://localhost:5173, proxies /api to 8080

# Smoke test (needs Playwright + Chromium; run build first)
npm run verify
```

**Connecting the ESP32:** in `biopod.ino` set `WIFI_SSID`, `WIFI_PASS` and `DASHBOARD_HOST` (the LAN IP of the PC running `server.py`; port 8080). Install the Arduino libraries *DHT sensor library* and *Adafruit Unified Sensor*. When the first reading arrives the header pill changes to "ESP32 linked".

**Testing without hardware:** run the demo from the dashboard, use the manual log form, or run `python Project/_post_test.py` to post one fake reading.

---

## 12. Known gaps and things to be ready to explain

These came up while reading the code. None stop the project from working, but a panel could ask about them.

1. **README is partly out of date.** It says a placeholder model is shown until a GLB is configured, but `modelConfig.js` already sets `glbUrl: '/models/container.glb'`. I could not confirm from here that `public/models/container.glb` exists (my file tool only lists text files). If it is missing, the app shows the placeholder and a banner, and most parts appear as "not in model". The README also lists a *Hinge* part and a firmware path (`Project/esp32/biopod.ino`) that no longer match the config / folder layout (`Project/esp32/biopod/biopod.ino`).
2. **`scripts/verify.mjs` is stale.** It expects the placeholder model, 5 markers, and *Hinge* / *Inner tray* parts; the current config has 18 components, so those checks will not match a real-model build.
3. **Unused files.** `sensorService.js`, `useSensorData.js`, `SensorDashboard.jsx`, `ModelMap.jsx` are not imported by `App.jsx`. The README says they can be deleted.
4. **The touchscreen is a web UI.** The firmware does not contain display or touch code, so the 480 × 320 interface runs in the browser (and as a standalone page). Be clear about whether the physical 4.0-inch display is running it.
5. **Touchscreen and dashboard use slightly different "in range" rules.** The dashboard and server use *any sensor* outside the band; the touchscreen checks only the *average*. They can disagree in edge cases.
6. **ADT, RT and alerts are computed in the browser**, so they exist only while a dashboard is open (stored in `localStorage`). Only the 30-minute log is computed by the server. ADT also includes sampling and polling delay, has about 1-second resolution (NTP seconds), and is only meaningful when the ESP32 clock is synced; without NTP the server stamps its own receive time and ADT will read near zero.
7. **Setpoints after an ESP32 reboot.** The firmware starts at 37 / 50 until its first successful POST returns the server's setpoints.
8. **Server downtime and the logger.** After a long server outage, the logger appends one catch-up slot per second until it reaches the present, and those slots are judged against the *current* latest reading.
9. **No security.** The server listens on all interfaces (`0.0.0.0`), has no login and allows any origin (CORS `*`). It is intended for a trusted LAN only.
10. **Two servers use port 8080.** `Project/server.py` (legacy, serves the old dashboard) and `3dmodelwebsiteshowcase/server.py` cannot run at the same time. Use the showcase one.
11. **Firmware pins** must be verified against the actual touchscreen board (noted in the firmware header).

---

## 13. Glossary

| Term | Meaning |
| --- | --- |
| Setpoint | The target temperature/humidity (default 37 °C, 50 % RH) |
| Acceptance range | ±0.5 °C and ±5 % RH around the setpoint; outside it the chamber is "out of range" |
| Hysteresis | A gap between turning an actuator on and off so it does not rapidly toggle |
| Uniformity | Spread (max − min) between the four sensors |
| ADT | Alert detection time, Ta − To |
| RT | Recovery time, Tr − Td |
| Logging completeness | Valid 30-minute records ÷ expected records × 100 |
| GLB | Binary glTF 3D model file exported from Blender |
| Exploded view | Parts slide apart along configured offsets so internals can be seen |
