import csv, json, requests

AIRLINE_SRC = "https://raw.githubusercontent.com/opentraveldata/opentraveldata/master/opentraveldata/optd_airlines.csv"
AIRCRAFT_SRC = "https://raw.githubusercontent.com/opentraveldata/opentraveldata/master/opentraveldata/optd_aircraft.csv"

def load_rows(url):
    r = requests.get(url, timeout=20)
    r.raise_for_status()
    return list(csv.DictReader(r.text.splitlines(), delimiter="^"))

def airline_name(r):
    for field in ("name", "name2", "alias"):
        v = (r.get(field) or "").strip()
        if v:
            return v
    return ""

def aircraft_name(r):
    for field in ("name", "model", "model_name", "text", "type"):
        v = (r.get(field) or "").strip()
        if v:
            return v
    # fallback manufacturer + model
    v = " ".join(filter(None, [(r.get("manufacturer") or "").strip(), (r.get("model") or "").strip()]))
    return v.strip()

def build_lookup(rows, code_field, name_fn, min_len=3, max_len=4):
    out = {}
    for r in rows:
        code = (r.get(code_field) or "").strip().upper()
        if not (min_len <= len(code) <= max_len):
            continue
        name = name_fn(r)
        if name:
            out[code] = name
    return out

if __name__ == "__main__":
    airlines = build_lookup(load_rows(AIRLINE_SRC), "3char_code", airline_name)
    aircraft = build_lookup(load_rows(AIRCRAFT_SRC), "icao_code", aircraft_name)
    print(f"airlines={len(airlines)}, aircraft={len(aircraft)}")
    with open("airlines.json", "w", encoding="utf-8") as f:
        json.dump(airlines, f, ensure_ascii=False, sort_keys=True, indent=2)
    with open("aircraft.json", "w", encoding="utf-8") as f:
        json.dump(aircraft, f, ensure_ascii=False, sort_keys=True, indent=2)
