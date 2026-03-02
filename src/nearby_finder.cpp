/*
 * nearby_finder.cpp
 * -----------------
 * Generalized "nearby places" demo using Uber H3 + ip-api + OpenStreetMap.
 *
 * 1. Detects your location via IP geolocation (ip-api.com, HTTP).
 * 2. Converts your coordinates to an H3 cell (resolution 9, ~174 m hexagons).
 * 3. Queries OpenStreetMap Overpass API for any amenity type within a radius.
 *
 * Usage:
 *   nearby_finder.exe [amenity] [radius_miles]
 *
 *   nearby_finder.exe                      # restaurants within 1 mile
 *   nearby_finder.exe cafe                 # cafes within 1 mile
 *   nearby_finder.exe hospital 5           # hospitals within 5 miles
 *   nearby_finder.exe fuel 2               # gas stations within 2 miles
 *
 * Common amenity values (OpenStreetMap tags):
 *   restaurant, cafe, bar, pub, fast_food, pharmacy, hospital,
 *   school, bank, atm, fuel, parking, library, cinema, theatre
 *
 * Build (MinGW on Windows):
 *   g++ src/nearby_finder.cpp -o nearby_finder.exe           ^
 *       -I"C:/Users/alok1/h3/src/h3lib/include"              ^
 *       -L"C:/Users/alok1/h3/build_manual" -lh3              ^
 *       -lwinhttp -lws2_32 -O2
 *
 * H3 library:  https://github.com/uber/h3
 * ip-api docs: https://ip-api.com/docs
 * Overpass QL: https://wiki.openstreetmap.org/wiki/Overpass_API/Overpass_QL
 */

#define WIN32_LEAN_AND_MEAN
#define _USE_MATH_DEFINES
#include <cmath>
#include <windows.h>
#include <winhttp.h>

#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>

extern "C" {
#include "h3api.h"
}

// ── Runtime config (overridden by argv) ──────────────────────────────────────

static std::string g_amenity  = "restaurant";
static double      g_radiusMi = 1.0;
static int         g_h3Res    = 9;

// ── HTTP helper ───────────────────────────────────────────────────────────────

struct HttpResponse {
    bool        ok;
    DWORD       winErr;
    std::string body;
};

// Drain all data from an open WinHTTP request handle into a string.
static std::string winHttpReadAll(HINTERNET hReq)
{
    std::string result;
    DWORD avail = 0;
    do {
        avail = 0;
        WinHttpQueryDataAvailable(hReq, &avail);
        if (!avail) break;
        std::vector<char> buf(avail + 1);
        DWORD read = 0;
        WinHttpReadData(hReq, buf.data(), avail, &read);
        result.append(buf.data(), read);
    } while (avail > 0);
    return result;
}

// Plain HTTP GET — used for ip-api.com (HTTP-only on free tier).
static HttpResponse httpGet(const std::wstring& host,
                            const std::wstring& path,
                            bool useHttps = false)
{
    HttpResponse res{false, 0, ""};

    HINTERNET hSess = WinHttpOpen(L"NearbyFinder/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSess) { res.winErr = GetLastError(); return res; }

    INTERNET_PORT port = useHttps ? INTERNET_DEFAULT_HTTPS_PORT
                                  : INTERNET_DEFAULT_HTTP_PORT;
    HINTERNET hConn = WinHttpConnect(hSess, host.c_str(), port, 0);
    if (!hConn) {
        res.winErr = GetLastError();
        WinHttpCloseHandle(hSess); return res;
    }

    DWORD flags = useHttps ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hReq = WinHttpOpenRequest(hConn, L"GET", path.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hReq) {
        res.winErr = GetLastError();
        WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSess); return res;
    }

    if (!WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(hReq, nullptr))
    {
        res.winErr = GetLastError();
        WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConn);
        WinHttpCloseHandle(hSess); return res;
    }

    res.body = winHttpReadAll(hReq);
    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hConn);
    WinHttpCloseHandle(hSess);
    res.ok = true;
    return res;
}

// ── Minimal JSON field extractor ──────────────────────────────────────────────

// Returns the raw value of the first "key": VALUE pair found (no nesting).
static std::string jsonField(const std::string& json, const std::string& key)
{
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return "";
    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
    if (pos >= json.size()) return "";

    if (json[pos] == '"') {
        ++pos;
        size_t end = json.find('"', pos);
        return (end == std::string::npos) ? "" : json.substr(pos, end - pos);
    } else {
        size_t end = pos;
        while (end < json.size() &&
               json[end] != ',' && json[end] != '}' &&
               json[end] != ']' && json[end] != '\n')
            ++end;
        std::string val = json.substr(pos, end - pos);
        while (!val.empty() && (val.back() == ' ' || val.back() == '\r'))
            val.pop_back();
        return val;
    }
}

// ── Point of Interest ─────────────────────────────────────────────────────────

struct PointOfInterest {
    std::string name;
    double      lat, lon;
    H3Index     cell;
};

// Parse Overpass JSON "elements" array into a list of PointOfInterest.
// Handles node elements (direct lat/lon) and way/relation elements
// (coordinates in a nested "center": {...} sub-object).
static std::vector<PointOfInterest> parseOverpassResponse(const std::string& json)
{
    std::vector<PointOfInterest> results;

    size_t eaPos = json.find("\"elements\"");
    if (eaPos == std::string::npos) return results;
    size_t arrOpen = json.find('[', eaPos);
    if (arrOpen == std::string::npos) return results;

    // Find matching closing bracket
    int depth = 0;
    size_t arrClose = arrOpen;
    for (; arrClose < json.size(); ++arrClose) {
        if (json[arrClose] == '[') ++depth;
        else if (json[arrClose] == ']') { --depth; if (!depth) break; }
    }

    size_t pos = arrOpen + 1;
    while (pos < arrClose) {
        size_t elemStart = json.find('{', pos);
        if (elemStart == std::string::npos || elemStart >= arrClose) break;

        // Find matching closing brace (depth-tracked)
        int d = 0;
        size_t elemEnd = elemStart;
        for (; elemEnd < json.size(); ++elemEnd) {
            if (json[elemEnd] == '{') ++d;
            else if (json[elemEnd] == '}') { --d; if (!d) break; }
        }
        std::string elem = json.substr(elemStart, elemEnd - elemStart + 1);

        // Direct lat/lon (node elements)
        std::string lat_s = jsonField(elem, "lat");
        std::string lon_s = jsonField(elem, "lon");

        // Way/relation: coordinates are inside a nested "center" object
        if (lat_s.empty() || lon_s.empty()) {
            size_t cpos = elem.find("\"center\"");
            if (cpos != std::string::npos) {
                size_t cstart = elem.find('{', cpos);
                size_t cend   = elem.find('}', cstart);
                if (cstart != std::string::npos && cend != std::string::npos) {
                    std::string center = elem.substr(cstart, cend - cstart + 1);
                    lat_s = jsonField(center, "lat");
                    lon_s = jsonField(center, "lon");
                }
            }
        }

        // Extract "name" from the tags sub-object
        std::string name_s;
        size_t tpos = elem.find("\"tags\"");
        if (tpos != std::string::npos) {
            size_t tstart = elem.find('{', tpos);
            if (tstart != std::string::npos) {
                int d2 = 0;
                size_t tend = tstart;
                for (; tend < elem.size(); ++tend) {
                    if (elem[tend] == '{') ++d2;
                    else if (elem[tend] == '}') { --d2; if (!d2) break; }
                }
                std::string tags = elem.substr(tstart, tend - tstart + 1);
                name_s = jsonField(tags, "name");
            }
        }

        if (!lat_s.empty() && !lon_s.empty()) {
            PointOfInterest poi;
            poi.lat  = std::stod(lat_s);
            poi.lon  = std::stod(lon_s);
            poi.name = name_s.empty() ? ("(" + g_amenity + ")") : name_s;

            LatLng ll;
            ll.lat = poi.lat * M_PI / 180.0;
            ll.lng = poi.lon * M_PI / 180.0;
            if (latLngToCell(&ll, g_h3Res, &poi.cell) == E_SUCCESS)
                results.push_back(poi);
        }
        pos = elemEnd + 1;
    }
    return results;
}

// ── Distance helper ───────────────────────────────────────────────────────────

static double haversineKm(double lat1, double lon1, double lat2, double lon2)
{
    const double R = 6371.0;
    double dLat = (lat2 - lat1) * M_PI / 180.0;
    double dLon = (lon2 - lon1) * M_PI / 180.0;
    double a = sin(dLat / 2) * sin(dLat / 2) +
               cos(lat1 * M_PI / 180.0) * cos(lat2 * M_PI / 180.0) *
               sin(dLon / 2) * sin(dLon / 2);
    return R * 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
}

// ── Overpass query via PowerShell temp-file pattern ───────────────────────────
//
// WinHTTP's HTTPS stack has TLS handshake issues with overpass-api.de on some
// Windows setups.  Writing a PowerShell script to %TEMP% and invoking it with
// system() is the most reliable approach on Windows without extra dependencies.

static std::string queryOverpassApi(double lat, double lon, double radiusM)
{
    std::ostringstream qs;
    qs << std::fixed << std::setprecision(6);
    qs << "[out:json][timeout:60];"
       << "nwr[\"amenity\"=\"" << g_amenity << "\"]"
       << "(around:" << static_cast<int>(radiusM) << "," << lat << "," << lon << ");"
       << "out center;";
    std::string rawQuery = qs.str();

    const char* tmpEnv  = getenv("TEMP");
    std::string tmpDir  = tmpEnv ? tmpEnv : "C:\\Temp";
    std::string psScript   = tmpDir + "\\nf_overpass.ps1";
    std::string queryFile  = tmpDir + "\\nf_query.txt";
    std::string resultFile = tmpDir + "\\nf_result.json";

    // Write raw Overpass QL; PowerShell will URL-encode it
    {
        std::ofstream qf(queryFile);
        qf << rawQuery;
    }

    // PowerShell script tries two Overpass mirrors for reliability
    {
        std::ofstream pf(psScript);
        pf << "$servers = @(\n"
           << "  'https://overpass-api.de/api/interpreter',\n"
           << "  'https://overpass.kumi.systems/api/interpreter'\n"
           << ")\n"
           << "$rawQuery = Get-Content '" << queryFile << "' -Raw -Encoding UTF8\n"
           << "$encoded  = [System.Uri]::EscapeDataString($rawQuery.Trim())\n"
           << "$body     = 'data=' + $encoded\n"
           << "$result   = $null\n"
           << "foreach ($url in $servers) {\n"
           << "  try {\n"
           << "    $r = Invoke-WebRequest -Uri $url -Method POST `\n"
           << "         -Body $body `\n"
           << "         -ContentType 'application/x-www-form-urlencoded' `\n"
           << "         -UseBasicParsing -TimeoutSec 60\n"
           << "    if ($r.StatusCode -eq 200 -and $r.Content -like '*elements*') {\n"
           << "      $result = $r.Content; break\n"
           << "    }\n"
           << "  } catch { <# try next server #> }\n"
           << "}\n"
           << "if ($result) {\n"
           << "  [System.IO.File]::WriteAllText('" << resultFile
           <<      "', $result, [System.Text.Encoding]::UTF8)\n"
           << "} else {\n"
           << "  '' | Out-File '" << resultFile << "'\n"
           << "}\n";
    }

    std::string cmd =
        "powershell -NonInteractive -ExecutionPolicy Bypass -File \""
        + psScript + "\"";
    system(cmd.c_str());

    std::string body;
    std::ifstream rf(resultFile);
    if (rf) {
        std::ostringstream ss;
        ss << rf.rdbuf();
        body = ss.str();
    }
    return body;
}

// ── Main ──────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[])
{
    // Parse optional CLI arguments: [amenity] [radius_miles]
    if (argc >= 2) g_amenity  = argv[1];
    if (argc >= 3) g_radiusMi = std::stod(argv[2]);
    if (g_radiusMi <= 0) g_radiusMi = 1.0;

    double radiusM = g_radiusMi * 1609.344;  // miles to metres

    std::cout << "================================================\n";
    std::cout << "  Nearby Finder  (H3 + ip-api + OpenStreetMap)\n";
    std::cout << "================================================\n";
    std::cout << "  Amenity : " << g_amenity << "\n";
    std::cout << std::fixed << std::setprecision(1);
    std::cout << "  Radius  : " << g_radiusMi << " mile(s)"
              << "  (~" << static_cast<int>(radiusM) << " m)\n\n";

    // ── Step 1: Detect location ──────────────────────────────────────────
    std::cout << "[1/3] Detecting location via ip-api.com...\n";

    auto geoRes = httpGet(L"ip-api.com", L"/json/", false);
    if (!geoRes.ok || geoRes.body.empty()) {
        std::cerr << "ERROR: Could not reach ip-api.com"
                  << " (WinHTTP error " << geoRes.winErr << ")\n";
        return 1;
    }

    std::string lat_s   = jsonField(geoRes.body, "lat");
    std::string lon_s   = jsonField(geoRes.body, "lon");
    std::string city    = jsonField(geoRes.body, "city");
    std::string country = jsonField(geoRes.body, "country");
    std::string ip      = jsonField(geoRes.body, "query");

    if (lat_s.empty() || lon_s.empty()) {
        std::cerr << "ERROR: Could not parse lat/lon from response:\n"
                  << geoRes.body.substr(0, 300) << "\n";
        return 1;
    }

    double lat = std::stod(lat_s);
    double lon = std::stod(lon_s);

    std::cout << std::fixed << std::setprecision(6);
    std::cout << "  IP       : " << ip << "\n";
    std::cout << "  Location : " << city << ", " << country << "\n";
    std::cout << "  Lat/Lon  : " << lat << " / " << lon << "\n\n";

    // ── Step 2: H3 cell ──────────────────────────────────────────────────
    std::cout << "[2/3] Computing H3 cell (res " << g_h3Res << ")...\n";

    LatLng ll;
    ll.lat = lat * M_PI / 180.0;
    ll.lng = lon * M_PI / 180.0;

    H3Index myCell = 0;
    if (latLngToCell(&ll, g_h3Res, &myCell) != E_SUCCESS) {
        std::cerr << "ERROR: latLngToCell failed.\n";
        return 1;
    }

    char cellStr[32];
    snprintf(cellStr, sizeof(cellStr), "%016llx", (unsigned long long)myCell);
    std::cout << "  Your cell : " << cellStr << "\n";

    // Show immediate neighbors (k=1 ring)
    int64_t kSize = 0;
    maxGridDiskSize(1, &kSize);
    std::vector<H3Index> ring((size_t)kSize);
    gridDisk(myCell, 1, ring.data());
    std::cout << "  k=1 ring  :";
    for (auto& c : ring) {
        if (!c) continue;
        char cs[32];
        snprintf(cs, sizeof(cs), "%016llx", (unsigned long long)c);
        std::cout << " " << cs;
        if (c == myCell) std::cout << "(you)";
    }
    std::cout << "\n\n";

    // ── Step 3: Overpass query ───────────────────────────────────────────
    std::cout << "[3/3] Querying OpenStreetMap for \""
              << g_amenity << "\" within "
              << std::fixed << std::setprecision(1) << g_radiusMi
              << " mi (~" << static_cast<int>(radiusM) << " m)...\n";
    std::cout << "  (Running PowerShell - may take ~15 s)\n";

    std::string ovBody = queryOverpassApi(lat, lon, radiusM);

    if (ovBody.empty() || ovBody.find("\"elements\"") == std::string::npos) {
        std::cerr << "ERROR: No valid Overpass response.\n";
        if (!ovBody.empty())
            std::cerr << "Preview: " << ovBody.substr(0, 400) << "\n";
        return 1;
    }

    auto pois = parseOverpassResponse(ovBody);

    std::cout << "\n";
    if (pois.empty()) {
        std::cout << "  No \"" << g_amenity << "\" found within "
                  << g_radiusMi << " mile(s).\n";
    } else {
        // Sort ascending by distance from current location
        std::sort(pois.begin(), pois.end(),
            [&](const PointOfInterest& a, const PointOfInterest& b) {
                return haversineKm(lat, lon, a.lat, a.lon)
                     < haversineKm(lat, lon, b.lat, b.lon);
            });

        std::cout << "  Found " << pois.size() << " location(s):\n\n";
        std::cout << "  #  | Distance  | H3 Cell (res " << g_h3Res
                  << ")   | Name\n";
        std::cout << "  ---|-----------|-------------------"
                  << "|-------------------------------\n";

        for (size_t i = 0; i < pois.size(); ++i) {
            const auto& p = pois[i];
            double distKm = haversineKm(lat, lon, p.lat, p.lon);
            double distMi = distKm * 0.621371;

            char cs[32];
            snprintf(cs, sizeof(cs), "%016llx", (unsigned long long)p.cell);

            std::cout << "  " << std::setw(2) << (i + 1)
                      << " | "
                      << std::fixed << std::setprecision(2)
                      << std::setw(5) << distMi << " mi"
                      << " | " << cs
                      << " | " << p.name << "\n";

            // Highlight if sharing the same H3 cell as the user
            if (p.cell == myCell)
                std::cout << "     |           |"
                          << "                   | *** Same H3 cell as you!\n";
        }
    }

    std::cout << "\n================================================\n";
    std::cout << "Done.\n";
    return 0;
}
