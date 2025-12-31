# FlightWatch Firmware

Firmware for FlightWatch running on ESP32 Trinity, driving a 64x64 HUB75 RGB LED matrix.

### What it does
- **Fetch nearby aircraft** from OpenSky Network using OAuth (states/all) filtered by location, radius, and bearing.
- **Enrich flights** with readable airline/aircraft info from AeroAPI and regional embedded lookups (no external CDN).
- **Render** a clean, minimal three-line flight card on a 64x64 HUB75 RGB LED matrix (ESP32 Trinity as controller).

### Key components
- **src/main.cpp**: Entry point. Initializes serial, WiFi/captive portal, fetchers, and display. Periodically fetches/enriches and renders.
- **core/FlightDataFetcher**: Orchestrates: fetch state vectors -> fetch flight metadata -> enrich names using AeroAPI + regional embedded fallback tables.
- **adapters/OpenSkyFetcher**: Queries OpenSky states/all with OAuth; parses and filters by geo.
- **adapters/AeroAPIFetcher**: Retrieves flight details by ident via AeroAPI.
- **adapters/NeoMatrixDisplay**: HUB75 renderer for the 64x64 panel driven by ESP32 Trinity; draws bordered, centered three-line flight card; cycles flights; shows loading.
- **config/**: User/API/timing/hardware/WiFi settings and portal defaults.
- **models/**: Lightweight structs for `StateVector`, `FlightInfo`, `AirportInfo`.
- **utils/GeoUtils.h**: Haversine distance and bounding boxes.
- **tools/generate_lookup_header.py**: Builds `core/LookupTables.generated.h` from local `airlines.json`/`aircraft.json` maps to avoid CDN lookups (run manually).

### Configuration quickstart
- WiFi: captive portal defaults (SSID/password/timeouts) in `config/WiFiConfiguration.h` (credentials collected via portal).
- Set location and display preferences in `config/UserConfiguration.h`.
- Set intervals in `config/TimingConfiguration.h`.
- Hardware: 64x64 HUB75 panel + ESP32 Trinity pinning in `config/HardwareConfiguration.h`.
- Provide API credentials/URLs in `config/APIConfiguration.h` (OpenSky OAuth, AeroAPI key).
- Airline/aircraft lookup source JSONs live in `tools/airlines.json` and `tools/aircraft.json`. Regenerate the embedded lookup header after editing with:
  ```
  python tools/generate_lookup_header.py --airlines tools/airlines.json --aircraft tools/aircraft.json --out core/LookupTables.generated.h
  ```

### Build
- PlatformIO project: see `platformio.ini`.

### Notes
- OpenSky OAuth is required for `states/all`. Token auto-refreshes with a safety skew.
- Display timing/pins are tuned for a single 64x64 HUB75 chain on ESP32 Trinity; adjust if you wire differently.
- Maximum supported search radius is **18 km**—do not exceed this when configuring location/radius filters.
- If more than **5 flights** are present in the region, the device may skip newly detected aircraft due to memory pressure.

## Project file architecture
- `src/main.cpp`: Firmware entry, WiFi/captive portal, scheduling, background fetch task, display loop.
- `core/`: `FlightDataFetcher` orchestrates state vector fetch + enrichment; glue between adapters.
- `adapters/`: API/display implementations (`OpenSkyFetcher`, `AeroAPIFetcher`, `NeoMatrixDisplay`).
- `models/`: Data structs for flights, airports, state vectors.
- `config/`: Defaults and runtime settings (user, WiFi, timing, hardware, API).
- `utils/`: Helpers (geo math, etc.).

## Data flow
- WiFi setup via captive portal (`flightwatch.local`) -> settings saved to NVS -> optional auto-restart.
- Background fetch task (FreeRTOS) every `FETCH_INTERVAL_SECONDS`: OpenSky `states/all` (OAuth) -> AeroAPI enrichment -> embedded airline/aircraft lookup fallback -> `g_lastFlights` (mutex-protected).
- Main loop ticks display ~40 FPS independent of fetches: copies latest flights -> renders flight cards on HUB75 matrix (progress bar, marquees, metrics).
- Settings server (MDNS + HTTP) serves `/` for config; changes persist via `RuntimeSettings`.

## Deployment architecture
- Single ESP32 Trinity driving a 64x64 HUB75 RGB LED matrix; double-buffered rendering.
- Network: outbound HTTPS to OpenSky OAuth + `states/all` and AeroAPI; local captive portal for first-time WiFi.
- Storage: NVS for WiFi credentials, runtime settings (location, units, brightness, colors, API keys).
- Runtime tasks: main UI loop + background fetch task to prevent display stalls during API calls.
