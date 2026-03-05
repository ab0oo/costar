# DSL Transforms And Visibility

This document covers recently added DSL runtime features in
`idf/main/DslWidgetRuntimeEspIdf.cpp`.

## `filter_gte`

Keep rows where a numeric field is greater-than-or-equal to a threshold.

```json
{
  "op": "filter_gte",
  "from": "rows",
  "by": "km",
  "min": "50",
  "unit": "km"
}
```

Fields:
- `from` (string, required): transform array name.
- `by` (string, required): numeric field in each row.
- `min` (number/string/expression, required): threshold.
- `unit` (string, optional): `km` (default), `mi`, `nm`.
  - For `mi` and `nm`, threshold is converted to km before compare.

## `filter_between`

Keep rows where a numeric field is within a range.

```json
{
  "op": "filter_between",
  "from": "rows",
  "by": "lat",
  "min": "24.40",
  "max": "49.38",
  "inclusive": true
}
```

Fields:
- `from` (string, required)
- `by` (string, required)
- `min` (number/string/expression, required)
- `max` (number/string/expression, required)
- `inclusive` (bool, optional, default `true`)
- `unit` (string, optional): same conversion behavior as `filter_gte`.

Notes:
- If `min > max`, runtime swaps them.
- Rows with missing/non-numeric `by` are dropped.

## `project_latlon`

Project latitude/longitude into screen-space coordinates.

```json
{
  "op": "project_latlon",
  "from": "rows",
  "lat_path": "lat",
  "lon_path": "lon",
  "x_field": "x",
  "y_field": "y",
  "lon_min": -124.85,
  "lon_max": -66.89,
  "lat_min": 24.40,
  "lat_max": 49.38,
  "x_min": 0,
  "x_max": 319,
  "y_min": 0,
  "y_max": 239,
  "clamp": true
}
```

Fields:
- `from` (string, required)
- `lat_path` (string, optional, default `lat`)
- `lon_path` (string, optional, default `lon`)
- `x_field` (string, optional, default `x`)
- `y_field` (string, optional, default `y`)
- `to_x_field` / `to_y_field` (legacy aliases accepted)
- `lon_min`, `lon_max`, `lat_min`, `lat_max` (required): source bounds.
- `x_min`, `x_max`, `y_min`, `y_max` (optional): destination bounds.
- `clamp` (bool, optional, default `true`): clamp normalized coordinates to [0..1].

Projection used:
- `nx = (lon - lon_min) / (lon_max - lon_min)`
- `ny = (lat_max - lat) / (lat_max - lat_min)`
- `x = x_min + nx * (x_max - x_min)`
- `y = y_min + ny * (y_max - y_min)`

## Node `visible_if`

Node-level conditional render gate.

```json
{
  "type": "circle",
  "visible_if": "qx{{i}}",
  "x": "qx{{i}}",
  "y": "qy{{i}}",
  "r": 2,
  "bg": "#FF0000",
  "color": "#FF0000"
}
```

Evaluation behavior:
- Empty/missing `visible_if` => visible.
- String booleans accepted (`true/false`, `yes/no`, `on/off`).
- Numeric expression accepted; non-zero => visible.
- If expression is a key token, runtime checks bound values and known tokens.
- Unknown/empty token evaluates false.

## Node `bitmap1`

Render a 1-bit packed bitmap (no compression, no RLE).

```json
{
  "type": "bitmap1",
  "x": 0,
  "y": 0,
  "w": 320,
  "h": 240,
  "path": "/maps/us_outline_320x240_1b.raw",
  "color": "#1A1A1A",
  "bg": "#FFFFFF"
}
```

Format:
- Row-major bit-packed bytes.
- `stride = (w + 7) / 8` bytes per row.
- Total bytes = `stride * h`.
- Bit order inside each byte: MSB first (`0x80` for left-most pixel).
- Bit `1` uses `color`, bit `0` uses `bg`.

Notes:
- Current runtime expects local LittleFS path for `bitmap1` (HTTP path is ignored).
