# DSL Minimal Transforms Spec (for JSON-first data shaping)

Status: proposed  
Goal: enable `adsb_nearest` and similar widgets without adding one-off runtime sources.

## Principles

1. Keep DSL product-facing and stable.
2. Add a small transform pipeline, not a general programming language.
3. Prefer composable data transforms over special-case C++ source handlers.
4. Limit scope aggressively to avoid "every feature implemented" drift.

## Scope

This spec adds a `data.transforms` pipeline that runs after fetch and before `data.fields`.

- Input: parsed source JSON (object or array).
- Output: transformed JSON context used by `data.fields` path resolution.
- Existing `data.fields` and `ui.nodes` remain unchanged.

## Proposed `data.transforms` (Minimal Set)

1. `coalesce`
- Purpose: fallback value selection (`flight || callsign || hex`).
- Shape:
```json
{ "op": "coalesce", "to": "field_name", "paths": ["a.b", "a.c", "a.d"], "default": "?" }
```

2. `map`
- Purpose: project each array item into a normalized row object.
- Shape:
```json
{
  "op": "map",
  "from": "ac",
  "to": "rows",
  "fields": {
    "flight": { "coalesce": ["flight", "callsign", "hex"], "default": "?" },
    "type": { "coalesce": ["t", "type"], "default": "?" },
    "dest": { "coalesce": ["destination", "route", "to"], "default": "?" }
  }
}
```

3. `compute_distance`
- Purpose: add distance for each row using `dst` when present, else haversine.
- Shape:
```json
{
  "op": "compute_distance",
  "from": "rows",
  "to_field": "km",
  "prefer_nm_path": "dst",
  "lat_path": "lat",
  "lon_path": "lon",
  "origin_lat": "{{geo.lat}}",
  "origin_lon": "{{geo.lon}}"
}
```

4. `sort`
- Purpose: stable ordering.
- Shape:
```json
{ "op": "sort", "from": "rows", "by": "km", "numeric": true, "order": "asc" }
```

5. `take`
- Purpose: top N rows.
- Shape:
```json
{ "op": "take", "from": "rows", "count": 5 }
```

6. `format`
- Purpose: unit and display formatting into strings.
- Shape:
```json
{
  "op": "format",
  "from": "rows",
  "to_field": "distance_text",
  "expr": "if_eq(pref.distance_unit,'mi', km*0.621371, km)",
  "round": 1,
  "suffix": "{{if_eq(pref.distance_unit,'mi','mi','km')}}"
}
```

7. `join_fields`
- Purpose: compose final row text.
- Shape:
```json
{
  "op": "join_fields",
  "from": "rows",
  "to_field": "line",
  "parts": ["flight", " ", "distance_text", " ", "alt_text", " ", "type", "->", "dest"]
}
```

8. `index_rows`
- Purpose: emit `row1..rowN` and optional subfields for current UI contracts.
- Shape:
```json
{
  "op": "index_rows",
  "from": "rows",
  "count": 5,
  "fields": ["line", "flight", "distance_text", "alt_text", "type", "dest"],
  "prefix_map": {
    "line": "row",
    "flight": "flight",
    "distance_text": "distance",
    "alt_text": "altitude",
    "type": "type",
    "dest": "destination"
  },
  "fill_empty": true
}
```

## Guardrails (to prevent feature hell)

1. No user-defined loops beyond `map` over one array input.
2. No nested `map` in v1.
3. No arbitrary script/eval blocks.
4. Max transforms per widget: 16.
5. Max intermediate rows: 256.
6. All ops must be deterministic and side-effect free.
7. Any new op must justify at least 2 real widgets and not be expressible by existing ops.

## Non-Goals (v1)

- General SQL-like grouping/aggregation.
- Regex/complex text processing.
- Cross-widget joins.
- Arbitrary dynamic object mutation.

## Example: `adsb_nearest` fully declarative

High-level pipeline:
1. `map ac -> rows` with coalesced flight/type/dest and normalized altitude text.
2. `compute_distance` (use `dst` nm if available; else haversine from geo).
3. `sort rows by km asc`.
4. `take 5`.
5. `format distance_text` based on `pref.distance_unit`.
6. `join_fields -> line`.
7. `index_rows -> row1..row5` (+ `flight1..`, `distance1..`, etc).
8. `fields.count = rows_count`.

This reproduces current behavior while removing `source: adsb_nearest` from C++.

## Rollout Plan

1. Implement transform engine behind feature flag.
2. Convert `adsb_nearest` JSON to new pipeline while preserving current UI field names.
3. Run side-by-side compare for one cycle:
- old source output vs transform output (`row1..row5`, `count`, per-row fields).
4. Remove C++ one-off source once parity is confirmed on hardware.
