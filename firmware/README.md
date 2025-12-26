# TheFlightWall Firmware

This is a high-level overview of the firmware that powers TheFlightWall on ESP32.

### What it does
- **Fetch nearby aircraft** from OpenSky Network using OAuth (states/all) filtered by location, radius, and bearing.
- **Enrich flights** with readable airline/aircraft info from AeroAPI and TheFlightWall CDN.
- **Render** a clean, minimal three-line flight card on a WS2812B LED matrix.

### Key components
- **src/main.cpp**: Entry point. Initializes serial, WiFi/captive portal, fetchers, and display. Periodically fetches/enriches and renders.
- **core/FlightDataFetcher**: Orchestrates: fetch state vectors -> fetch flight metadata -> enrich names.
- **adapters/OpenSkyFetcher**: Queries OpenSky states/all with OAuth; parses and filters by geo.
- **adapters/AeroAPIFetcher**: Retrieves flight details by ident via AeroAPI.
- **adapters/FlightWallFetcher**: Looks up human-friendly airline/aircraft names from CDN.
- **adapters/NeoMatrixDisplay**: Draws bordered, centered three-line flight card; cycles flights; shows loading.
- **config/**: User/API/timing/hardware/WiFi settings and portal defaults.
- **models/**: Lightweight structs for `StateVector`, `FlightInfo`, `AirportInfo`.
- **utils/GeoUtils.h**: Haversine distance and bounding boxes.

### Configuration quickstart
- WiFi: captive portal defaults (SSID/password/timeouts) in `config/WiFiConfiguration.h` (credentials collected via portal).
- Set location and display preferences in `config/UserConfiguration.h`.
- Set intervals in `config/TimingConfiguration.h`.
- Set display dimensions/pin in `config/HardwareConfiguration.h`.
- Provide API credentials/URLs in `config/APIConfiguration.h` (OpenSky OAuth, AeroAPI key, CDN base).

### Build
- PlatformIO project: see `platformio.ini`.

### Notes
- OpenSky OAuth is required for `states/all`. Token auto-refreshes with a safety skew.
- Display uses `FastLED_NeoMatrix` with WS2812B strips; adjust tiling/orientation in hardware config.

## Project file architecture
- `src/main.cpp`: Firmware entry, WiFi/captive portal, scheduling, background fetch task, display loop.
- `core/`: `FlightDataFetcher` orchestrates state vector fetch + enrichment; glue between adapters.
- `adapters/`: API/display implementations (`OpenSkyFetcher`, `AeroAPIFetcher`, `FlightWallFetcher`, `NeoMatrixDisplay`).
- `models/`: Data structs for flights, airports, state vectors.
- `config/`: Defaults and runtime settings (user, WiFi, timing, hardware, API).
- `utils/`: Helpers (geo math, etc.).

## Data flow
- WiFi setup via captive portal (`flightwatch.local`) → settings saved to NVS → optional auto-restart.
- Background fetch task (FreeRTOS) every `FETCH_INTERVAL_SECONDS`: OpenSky `states/all` (OAuth) → AeroAPI enrichment → FlightWall name lookups → `g_lastFlights` (mutex-protected).
- Main loop ticks display ~40 FPS independent of fetches: copies latest flights → renders flight cards on HUB75 matrix (progress bar, marquees, metrics).
- Settings server (MDNS + HTTP) serves `/` for config; changes persist via `RuntimeSettings`.

## Deployment architecture
- Single ESP32 (HUB75/Trinity) driving LED matrix; double-buffered rendering.
- Network: outbound HTTPS to OpenSky OAuth + `states/all`, AeroAPI, and FlightWall CDN; local captive portal for first-time WiFi.
- Storage: NVS for WiFi credentials, runtime settings (location, units, brightness, colors, API keys).
- Runtime tasks: main UI loop + background fetch task to prevent display stalls during API calls.
