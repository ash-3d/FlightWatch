# TheFlightWall Firmware

This is a high-level overview of the firmware that powers TheFlightWall on ESP32.

### What it does
- **Fetch nearby aircraft** from OpenSky Network using OAuth (states/all) filtered by location, radius, and bearing.
- **Enrich flights** with readable airline/aircraft info from AeroAPI and TheFlightWall CDN.
- **Render** a clean, minimal three-line flight card on a WS2812B LED matrix.

### Key components
- **src/main.cpp**: Entry point. Initializes serial, Wi‑Fi, fetchers, and display. Periodically fetches/enriches and renders.
- **core/FlightDataFetcher**: Orchestrates: fetch state vectors → fetch flight metadata → enrich names.
- **adapters/OpenSkyFetcher**: Queries OpenSky states/all with OAuth; parses and filters by geo.
- **adapters/AeroAPIFetcher**: Retrieves flight details by ident via AeroAPI.
- **adapters/FlightWallFetcher**: Looks up human‑friendly airline/aircraft names from CDN.
- **adapters/NeoMatrixDisplay**: Draws bordered, centered three‑line flight card; cycles flights; shows loading.
- **config/**: User/API/timing/hardware/Wi‑Fi settings.
- **models/**: Lightweight structs for `StateVector`, `FlightInfo`, `AirportInfo`.
- **utils/GeoUtils.h**: Haversine distance and bounding boxes.

### Configuration quickstart
- Set Wi‑Fi in `config/WiFiConfiguration.h`.
- Set location and display preferences in `config/UserConfiguration.h`.
- Set intervals in `config/TimingConfiguration.h`.
- Set display dimensions/pin in `config/HardwareConfiguration.h`.
- Provide API credentials/URLs in `config/APIConfiguration.h` (OpenSky OAuth, AeroAPI key, CDN base).

### Build
- PlatformIO project: see `platformio.ini`.

### Notes
- OpenSky OAuth is required for `states/all`. Token auto‑refreshes with a safety skew.
- Display uses `FastLED_NeoMatrix` with WS2812B strips; adjust tiling/orientation in hardware config.