# FlightWatch (ESP32 Trinity + 64x64 HUB75)

FlightWatch is based on the opensource TheFlightWall_OSS project by AxisNimble. FlightWatch is tuned for an ESP32 Trinity driving a single 64x64 HUB75 RGB panel. The firmware prioritizes low RAM usage (single-buffer display, streaming parses, static TLS clients, short-lived portal) while fetching nearby flights.

## Hardware (current build)
- ESP32 Trinity (HUB75 output)
- 64x64 HUB75 RGB matrix (single panel)
- 5V power matched to your panel; follow Trinity pinout in `config/HardwareConfiguration.h`
- Optional: 3D-printed bracket/enclosure for the single panel

## Data sources
- OpenSky (OAuth) for ADS-B state vectors (`states/all`)
- FlightAware AeroAPI for route/airline/aircraft enrichment (filtered, cached)
- Embedded airline/aircraft lookup tables (no CDN)

## Firmware behavior
- Fetch cadence: `TimingConfiguration::FETCH_INTERVAL_SECONDS` (default 30s)
- OpenSky: static TLS client, streaming JSON parse, heap guard (skips if heap is low), 2-minute TLS backoff after failure
- AeroAPI: static TLS client, stream parse with filter, per-pass limit (2 calls), 20s TLS backoff, 60s enrichment cache
- Weather: idle-only, plain HTTP (no TLS), short backoff; clears stale symbol if weather code is missing
- Portal: `flightwatch.local` available for 10 seconds after boot; shuts down if unused to free MDNS/HTTP resources
- Display: single-buffered HUB75 (double buffer disabled to save RAM); slight tearing possible; scroll/tick rate adjustable in `NeoMatrixDisplay`

## Setup
1) WiFi: Captive portal via WiFiManager; defaults in `config/WiFiConfiguration.h`
2) Location/display: Set in `config/UserConfiguration.h` (center lat/lon, radius, units, colors, brightness)
3) Hardware: HUB75 pin/size in `config/HardwareConfiguration.h`
4) API keys: `config/APIConfiguration.h` (OpenSky OAuth client_id/secret, AeroAPI key)
5) Lookup tables: edit `tools/airlines.json` / `tools/aircraft.json`, then regenerate:
python tools/generate_lookup_header.py --airlines tools/airlines.json --aircraft tools/aircraft.json --out core/LookupTables.generated.h


## Build & flash
- PlatformIO project (`platformio.ini`); ensure `utils/*.cpp` is included (NetLock)
- Open the `firmware` folder in VS Code with the PlatformIO extension
- Click Upload to flash the ESP32 Trinity

## Notes on memory/TLS
- Single-buffer display frees heap for TLS; streaming parses avoid large payload buffers
- If TLS failures persist, options: lengthen fetch interval, lower per-pass AeroAPI limit, or re-enable double-buffer only if RAM allows (at the cost of more heap)

## Thanks
- ADS-B contributors for making flight data available
- Brian Lough for developing the ESP32 Trinity board
