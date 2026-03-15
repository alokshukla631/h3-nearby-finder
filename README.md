# h3-nearby-finder

**Find restaurants (or any POI) near you using Uber H3 + ip-api + OpenStreetMap.**

A practical demo that combines three free, no-API-key-required tools:

| Component | Purpose |
|-----------|---------|
| [**Uber H3**](https://h3geo.org/) | Hierarchical hexagonal geospatial index — assigns every point on Earth a compact 64-bit cell ID |
| [**ip-api.com**](http://ip-api.com) | Free IP geolocation — resolves your public IP to approximate lat/lng |
| [**OpenStreetMap Overpass API**](https://overpass-api.de) | Planet-scale POI database — query by type, name, brand, or any tag, within any radius |

## Why H3?

H3 lets you express spatial relationships as compact integer math. Instead of "within 3 miles", you can say "within k-ring 15 at resolution 9". Cells at the same resolution are roughly equal-area, so you get consistent search behaviour globally. You can also express multi-resolution queries: find everything in my res-9 cell, and everything in adjacent res-7 cells for a broader sweep.

This demo computes your H3 cell at resolutions 5 through 11, shows your k=1 ring of neighbors, and then queries OpenStreetMap for nearby amenities sorted by walking distance.

---

## Features

- **Auto-detects your location** from your public IP (no GPS / no permissions needed)
- **Prints H3 cell IDs** at four resolutions (5, 7, 9, 11) with approximate area
- **Shows k=1 neighbor ring** at resolution 9 (~174 m hexagons)
- **Queries any amenity** via Overpass QL (`restaurant`, `cafe`, `bar`, `fast_food`, …)
- **Sorts by haversine distance** in miles
- **Shows each result's H3 cell ID** so you can do further spatial reasoning
- **Caches Overpass results** to a temp file — rerun instantly

---

## Build (Windows + MinGW)

### Prerequisites

| Dependency | How to get |
|------------|-----------|
| MinGW-w64 (gcc ≥ 12) | [winlibs.com](https://winlibs.com) |
| Uber H3 (built from source) | `git clone https://github.com/uber/h3` — see [build guide](docs/build-h3.md) |
| cmake (for H3) | `pip install cmake` |

### Build H3

```bat
git clone https://github.com/uber/h3 C:\h3
python -c "
import subprocess, os
cmake = r'C:\...\cmake.exe'   # adjust to pip-installed cmake path
os.makedirs(r'C:\h3\build_manual\obj', exist_ok=True)
import glob
srcs = glob.glob(r'C:\h3\src\h3lib\lib\*.c')
for src in srcs:
    subprocess.run([r'C:\mingw64\bin\gcc.exe', '-c', src,
        '-I', r'C:\h3\src\h3lib\include', '-O2',
        '-o', src.replace('.c','.o').replace('lib\\','build_manual\\obj\\')])
objs = glob.glob(r'C:\h3\build_manual\obj\*.o')
subprocess.run([r'C:\mingw64\bin\ar.exe', 'rcs',
    r'C:\h3\build_manual\libh3.a'] + objs)
"
```

### Build the finder

```bat
g++ src/nearby_finder.cpp -o nearby_finder.exe ^
    -I"C:\h3\src\h3lib\include" ^
    -L"C:\h3\build_manual" -lh3 ^
    -lwinhttp -lws2_32 -O2 -std=c++17
```

### Run

```bat
nearby_finder.exe
```

```
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
  H3 Nearby Finder — Restaurants near you
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

[1/3] Detecting your location via IP...
  IP Address : 50.35.119.21
  Location   : Everett, United States
  Latitude   : 47.894800  Longitude : -122.203100

[2/3] Computing H3 cell IDs...
  Res  5  │ 08528d0b7fffffff │ ~252 km²
  Res  7  │ 08728d0b70ffffff │ ~5.16 km²
  Res  9  │ 08928d0b7053ffff │ ~0.105 km²  ← your cell
  Res 11  │ 08b28d0b70504fff │ ~0.00047 km²

  k=1 ring (res 9 neighbors):
    08928d0b7053ffff  ← YOU ARE HERE
    08928d0b7627ffff
    08928d0b705bffff
    ...

[3/3] Finding restaurants within 1 mile (~1609 m)...

  #  │ Distance │ H3 Cell (res 9)   │ Name
  ───┼──────────┼───────────────────┼──────────────────────────
   1 │  0.23 mi │ 08928d0b7053ffff │ Tandoori Hut
   2 │  0.41 mi │ 08928d0b7057ffff │ Pho Hoa
   3 │  0.55 mi │ 08928d0b7057ffff │ Everett Thai
  ...
```

---

## Change the search

Edit `src/nearby_finder.cpp` around line 295:

```cpp
// ── Amenity and radius ────────────────────────────────────────────
const std::string AMENITY   = "restaurant";   // cafe, bar, fast_food, ...
const double      RADIUS_MI = 1.0;            // search radius in miles
const int         H3_RES    = 9;              // resolution for output cell IDs
```

Recompile and run.

---

## How the H3 math works

```
Earth surface → H3 cells at resolution 9
Each cell ≈ 0.105 km² ≈ a city block

Your location:
  latLngToCell({lat, lng}, res=9)  →  cell 08928d0b7053ffff

k-ring of radius 1:
  gridDisk(yourCell, k=1)  →  7 cells  (you + 6 neighbors)

k-ring of radius 3:
  gridDisk(yourCell, k=3)  →  37 cells (~3 "hexagonal blocks" away)
```

For this demo, we use the Overpass `around:` operator for the raw geographic query, then use H3 to annotate each result with its cell ID — useful for aggregation, bucketing, and spatial joins.

---

## Architecture

```
┌───────────────┐     HTTP GET      ┌──────────────────────┐
│  ip-api.com   │ ◄──────────────── │                      │
│ (geolocation) │ ────────────────► │   nearby_finder.exe  │
└───────────────┘   {lat, lon, ...} │                      │
                                    │  ┌─────────────────┐ │
                                    │  │   libh3.a       │ │
                                    │  │  latLngToCell() │ │
                                    │  │  gridDisk()     │ │
                                    │  └─────────────────┘ │
┌───────────────┐     HTTP POST     │                      │
│  Overpass API │ ◄──────────────── │  PowerShell script   │
│ (OSM data)    │ ────────────────► │  (handles TLS)       │
└───────────────┘   {elements: [...]}└──────────────────────┘
```

---

## Why PowerShell for the Overpass call?

WinHTTP (the native Windows HTTPS stack used in the core binary) struggles with some Overpass API server configurations. The finder writes a one-shot PowerShell script to a temp file and executes it to handle the TLS handshake reliably. The result lands in a temp JSON file that the C++ binary reads and parses. This pattern keeps the binary dependency-free while still leveraging the OS's full TLS capabilities.

---

## License

MIT — use freely, attribution appreciated.

## Author

**Alok Shukla** · [LinkedIn](https://www.linkedin.com/in/alshukla/) · Engineering Leader, Amazon Web Services
