package main

import (
	"bytes"
	"container/list"
	"context"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"image"
	"image/color"
	_ "image/gif"
	_ "image/jpeg"
	_ "image/png"
	"io"
	"log"
	"net/http"
	"net/url"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/srwiley/oksvg"
	"github.com/srwiley/rasterx"
)

const (
	defaultListenAddr  = ":8085"
	maxDimension       = 1024
	defaultMdiSize     = 28
	defaultUserAgent   = "CoStar-ImageProxy/1.0"
	maxOutputDimension = 320
	defaultMdiBaseURL  = "https://cdn.jsdelivr.net/npm/@mdi/svg/svg"
)

type serverConfig struct {
	maxDownloadBytes int64
	mdiBaseURL       string
	userAgent        string
	cache            *responseCache
}

type fetchOptions struct {
	userAgent string
	referer   string
}

type cachedResponse struct {
	raw          []byte
	width        int
	height       int
	sourceFormat string
	sourceURL    string
	iconName     string
	storedAt     time.Time
}

type cacheItem struct {
	key   string
	value cachedResponse
}

type responseCache struct {
	mu         sync.Mutex
	ttl        time.Duration
	maxEntries int
	ll         *list.List
	items      map[string]*list.Element
}

type loggingResponseWriter struct {
	http.ResponseWriter
	status int
	bytes  int
}

func (w *loggingResponseWriter) WriteHeader(code int) {
	w.status = code
	w.ResponseWriter.WriteHeader(code)
}

func (w *loggingResponseWriter) Write(p []byte) (int, error) {
	if w.status == 0 {
		w.status = http.StatusOK
	}
	n, err := w.ResponseWriter.Write(p)
	w.bytes += n
	return n, err
}

func newResponseCache(ttl time.Duration, maxEntries int) *responseCache {
	if maxEntries < 0 {
		maxEntries = 0
	}
	if ttl < 0 {
		ttl = 0
	}
	return &responseCache{
		ttl:        ttl,
		maxEntries: maxEntries,
		ll:         list.New(),
		items:      make(map[string]*list.Element),
	}
}

func (c *responseCache) get(key string) (cachedResponse, bool) {
	if c == nil || c.maxEntries == 0 {
		return cachedResponse{}, false
	}
	now := time.Now()

	c.mu.Lock()
	defer c.mu.Unlock()

	el, ok := c.items[key]
	if !ok {
		return cachedResponse{}, false
	}
	item := el.Value.(*cacheItem)
	if c.ttl > 0 && now.Sub(item.value.storedAt) > c.ttl {
		c.removeElement(el)
		return cachedResponse{}, false
	}

	c.ll.MoveToFront(el)
	return item.value, true
}

func (c *responseCache) set(key string, value cachedResponse) {
	if c == nil || c.maxEntries == 0 {
		return
	}
	if value.storedAt.IsZero() {
		value.storedAt = time.Now()
	}

	c.mu.Lock()
	defer c.mu.Unlock()

	if el, ok := c.items[key]; ok {
		item := el.Value.(*cacheItem)
		item.value = value
		c.ll.MoveToFront(el)
		return
	}

	el := c.ll.PushFront(&cacheItem{key: key, value: value})
	c.items[key] = el
	for len(c.items) > c.maxEntries {
		back := c.ll.Back()
		if back == nil {
			break
		}
		c.removeElement(back)
	}
}

func (c *responseCache) removeElement(el *list.Element) {
	if c == nil || el == nil {
		return
	}
	c.ll.Remove(el)
	item := el.Value.(*cacheItem)
	delete(c.items, item.key)
}

func main() {
	listenAddr := flag.String("listen", defaultListenAddr, "HTTP listen address")
	maxBytes := flag.Int64("max-bytes", 8*1024*1024, "Maximum source image download bytes")
	mdiBase := flag.String("mdi-base", defaultMdiBaseURL, "Upstream base URL for MDI SVG icons")
	userAgent := flag.String("user-agent", defaultUserAgent, "Default upstream User-Agent for source image fetches")
	cacheTTL := flag.Duration("cache-ttl", 10*time.Minute, "In-memory cache TTL duration (e.g. 10m, 30s, 0 to disable TTL)")
	cacheMaxEntries := flag.Int("cache-max-entries", 256, "Maximum in-memory cache entries (0 disables caching)")
	flag.Parse()

	cfg := serverConfig{
		maxDownloadBytes: *maxBytes,
		mdiBaseURL:       strings.TrimRight(strings.TrimSpace(*mdiBase), "/"),
		userAgent:        strings.TrimSpace(*userAgent),
		cache:            newResponseCache(*cacheTTL, *cacheMaxEntries),
	}

	mux := http.NewServeMux()
	mux.HandleFunc("/", rootHandler)
	mux.HandleFunc("/healthz", healthHandler)
	mux.HandleFunc("/cmh", cfg.convertHandler)
	mux.HandleFunc("/mdi", cfg.mdiHandler)

	srv := &http.Server{
		Addr:              *listenAddr,
		Handler:           loggingMiddleware(mux),
		ReadHeaderTimeout: 5 * time.Second,
	}

	log.Printf("image_proxy listening on %s", *listenAddr)
	if err := srv.ListenAndServe(); err != nil && !errors.Is(err, http.ErrServerClosed) {
		log.Fatalf("server failed: %v", err)
	}
}

func loggingMiddleware(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		start := time.Now()
		lrw := &loggingResponseWriter{ResponseWriter: w}
		next.ServeHTTP(lrw, r)
		status := lrw.status
		if status == 0 {
			status = http.StatusOK
		}
		requestURI := r.URL.Path
		if r.URL.RawQuery != "" {
			requestURI += "?" + r.URL.RawQuery
		}
		log.Printf("%s %s status=%d bytes=%d from=%s dur=%s", r.Method, requestURI, status,
			lrw.bytes, r.RemoteAddr, time.Since(start).Truncate(time.Millisecond))
	})
}

func rootHandler(w http.ResponseWriter, _ *http.Request) {
	w.Header().Set("Content-Type", "text/plain; charset=utf-8")
	_, _ = w.Write([]byte(
		"GET /cmh?url=<image_url>&size=<n|WxH>\n" +
			"GET /mdi?icon=<mdi_name>&size=<n|WxH>&color=<RRGGBB>\n" +
			"Returns raw rgb565 little-endian bytes.\n" +
			"Response includes X-Cache: HIT or MISS.\n" +
			"/cmh supports optional ua= and referer= query params.\n",
	))
}

func healthHandler(w http.ResponseWriter, _ *http.Request) {
	w.Header().Set("Content-Type", "text/plain; charset=utf-8")
	_, _ = w.Write([]byte("ok"))
}

func (cfg serverConfig) convertHandler(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		writeError(w, http.StatusMethodNotAllowed, "method not allowed")
		return
	}

	srcURL := strings.TrimSpace(r.URL.Query().Get("url"))
	if srcURL == "" {
		writeError(w, http.StatusBadRequest, "missing url query parameter")
		return
	}

	width, height, err := parseTargetSize(r.URL.Query())
	if err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}

	if width > maxOutputDimension || height > maxOutputDimension {
		resp := buildOversizeFallback(width, height)
		writeRawResponse(w, resp, false)
		return
	}

	uaOverride := sanitizeHeaderValue(r.URL.Query().Get("ua"))
	refererOverride := sanitizeHeaderValue(r.URL.Query().Get("referer"))
	cacheKey := buildCMHCacheKey(srcURL, width, height, uaOverride, refererOverride)
	if cached, ok := cfg.cache.get(cacheKey); ok {
		writeRawResponse(w, cached, true)
		return
	}

	ctx, cancel := context.WithTimeout(r.Context(), 20*time.Second)
	defer cancel()

	body, fetchURL, err := cfg.fetchSourceBytes(ctx, srcURL, fetchOptions{
		userAgent: firstNonEmpty(uaOverride, cfg.userAgent, defaultUserAgent),
		referer:   refererOverride,
	})
	if err != nil {
		writeError(w, http.StatusBadGateway, err.Error())
		return
	}
	srcImg, srcFmt, err := decodeImageBytes(body, width, height)
	if err != nil {
		writeError(w, http.StatusBadGateway, err.Error())
		return
	}
	if srcImg.Bounds().Dx() > maxOutputDimension || srcImg.Bounds().Dy() > maxOutputDimension {
		resp := buildOversizeFallback(width, height)
		writeRawResponse(w, resp, false)
		return
	}

	dst := resizeNearest(srcImg, width, height)
	raw := encodeRGB565LE(dst)

	resp := cachedResponse{
		raw:          raw,
		width:        width,
		height:       height,
		sourceFormat: srcFmt,
		sourceURL:    fetchURL,
		storedAt:     time.Now(),
	}
	cfg.cache.set(cacheKey, resp)
	writeRawResponse(w, resp, false)
}

func (cfg serverConfig) mdiHandler(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		writeError(w, http.StatusMethodNotAllowed, "method not allowed")
		return
	}

	iconRaw := strings.TrimSpace(r.URL.Query().Get("icon"))
	if iconRaw == "" {
		writeError(w, http.StatusBadRequest, "missing icon query parameter")
		return
	}
	iconName, err := normalizeMdiIconName(iconRaw)
	if err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}

	width, height := defaultMdiSize, defaultMdiSize
	if hasExplicitSize(r.URL.Query()) {
		width, height, err = parseTargetSize(r.URL.Query())
		if err != nil {
			writeError(w, http.StatusBadRequest, err.Error())
			return
		}
	}

	if width > maxOutputDimension || height > maxOutputDimension {
		resp := buildOversizeFallback(width, height)
		resp.iconName = "mdi:oversize-fallback"
		writeRawResponse(w, resp, false)
		return
	}

	color, err := normalizeHexColor(r.URL.Query().Get("color"))
	if err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}

	srcURL := fmt.Sprintf("%s/%s.svg", cfg.mdiBaseURL, url.PathEscape(iconName))
	cacheKey := buildMDICacheKey(cfg.mdiBaseURL, iconName, color, width, height)
	if cached, ok := cfg.cache.get(cacheKey); ok {
		writeRawResponse(w, cached, true)
		return
	}

	ctx, cancel := context.WithTimeout(r.Context(), 20*time.Second)
	defer cancel()

	body, fetchURL, err := cfg.fetchSourceBytes(ctx, srcURL, fetchOptions{
		userAgent: firstNonEmpty(cfg.userAgent, defaultUserAgent),
	})
	if err != nil {
		writeError(w, http.StatusBadGateway, err.Error())
		return
	}
	srcImg, srcFmt, err := decodeImageBytes(body, width, height)
	// Some SVG sources (notably Iconify compact paths) are not compatible with oksvg.
	// Retry with parser-compatible MDI SVG source to keep /mdi resilient.
	if err != nil && cfg.mdiBaseURL != defaultMdiBaseURL {
		fallbackURL := fmt.Sprintf("%s/%s.svg", defaultMdiBaseURL, url.PathEscape(iconName))
		fallbackBody, fallbackFetchURL, fallbackErr := cfg.fetchSourceBytes(ctx, fallbackURL, fetchOptions{
			userAgent: firstNonEmpty(cfg.userAgent, defaultUserAgent),
		})
		if fallbackErr == nil {
			fallbackImg, fallbackFmt, fallbackDecodeErr := decodeImageBytes(fallbackBody, width, height)
			if fallbackDecodeErr == nil {
				srcImg = fallbackImg
				srcFmt = fallbackFmt
				fetchURL = fallbackFetchURL
				err = nil
			}
		}
	}
	if err != nil {
		writeError(w, http.StatusBadGateway, err.Error())
		return
	}
	dst := resizeNearest(srcImg, width, height)
	if color != "" {
		r, g, b := hexColorRGB(color)
		tintNRGBA(dst, r, g, b)
	}
	raw := encodeRGB565LE(dst)

	resp := cachedResponse{
		raw:          raw,
		width:        width,
		height:       height,
		sourceFormat: srcFmt,
		sourceURL:    fetchURL,
		iconName:     "mdi:" + iconName,
		storedAt:     time.Now(),
	}
	cfg.cache.set(cacheKey, resp)
	writeRawResponse(w, resp, false)
}

func buildCMHCacheKey(srcURL string, width, height int, userAgent, referer string) string {
	return "cmh|" + srcURL + "|" + strconv.Itoa(width) + "x" + strconv.Itoa(height) + "|ua=" + userAgent + "|ref=" + referer
}

func buildMDICacheKey(base, icon, color string, width, height int) string {
	return "mdi|" + base + "|" + icon + "|" + color + "|" + strconv.Itoa(width) + "x" + strconv.Itoa(height)
}

func writeRawResponse(w http.ResponseWriter, resp cachedResponse, cacheHit bool) {
	w.Header().Set("Content-Type", "application/octet-stream")
	w.Header().Set("Cache-Control", "no-store")
	w.Header().Set("X-Image-Format", "rgb565le")
	w.Header().Set("X-Width", strconv.Itoa(resp.width))
	w.Header().Set("X-Height", strconv.Itoa(resp.height))
	if resp.sourceFormat != "" {
		w.Header().Set("X-Source-Format", resp.sourceFormat)
	}
	if resp.sourceURL != "" {
		w.Header().Set("X-Source-URL", resp.sourceURL)
	}
	if resp.iconName != "" {
		w.Header().Set("X-Icon", resp.iconName)
	}
	if resp.sourceFormat == "fallback-x" {
		w.Header().Set("X-Fallback", "oversize-request")
	}
	if cacheHit {
		w.Header().Set("X-Cache", "HIT")
	} else {
		w.Header().Set("X-Cache", "MISS")
	}
	if !resp.storedAt.IsZero() {
		ageSec := int(time.Since(resp.storedAt).Seconds())
		if ageSec < 0 {
			ageSec = 0
		}
		w.Header().Set("X-Cache-Age-Seconds", strconv.Itoa(ageSec))
	}
	w.Header().Set("Content-Length", strconv.Itoa(len(resp.raw)))
	_, _ = w.Write(resp.raw)
}

func clampOutputSize(width, height int) (int, int) {
	if width > maxOutputDimension {
		width = maxOutputDimension
	}
	if height > maxOutputDimension {
		height = maxOutputDimension
	}
	if width < 1 {
		width = 1
	}
	if height < 1 {
		height = 1
	}
	return width, height
}

func buildOversizeFallback(requestedW, requestedH int) cachedResponse {
	w, h := clampOutputSize(requestedW, requestedH)
	raw := generateXIconRGB565LE(w, h)
	return cachedResponse{
		raw:          raw,
		width:        w,
		height:       h,
		sourceFormat: "fallback-x",
		sourceURL:    fmt.Sprintf("fallback://oversize/%dx%d", requestedW, requestedH),
		storedAt:     time.Now(),
	}
}

func absInt(v int) int {
	if v < 0 {
		return -v
	}
	return v
}

func rgb565LE(r, g, b uint8) (byte, byte) {
	v := (uint16(r&0xF8) << 8) | (uint16(g&0xFC) << 3) | (uint16(b) >> 3)
	return byte(v & 0xFF), byte((v >> 8) & 0xFF)
}

func hexColorRGB(hex string) (uint8, uint8, uint8) {
	if len(hex) != 6 {
		return 0xFF, 0xFF, 0xFF
	}
	rv, err := strconv.ParseUint(hex[0:2], 16, 8)
	if err != nil {
		return 0xFF, 0xFF, 0xFF
	}
	gv, err := strconv.ParseUint(hex[2:4], 16, 8)
	if err != nil {
		return 0xFF, 0xFF, 0xFF
	}
	bv, err := strconv.ParseUint(hex[4:6], 16, 8)
	if err != nil {
		return 0xFF, 0xFF, 0xFF
	}
	return uint8(rv), uint8(gv), uint8(bv)
}

func tintNRGBA(img *image.NRGBA, r, g, b uint8) {
	if img == nil {
		return
	}
	for i := 0; i+3 < len(img.Pix); i += 4 {
		if img.Pix[i+3] == 0 {
			continue
		}
		img.Pix[i] = r
		img.Pix[i+1] = g
		img.Pix[i+2] = b
	}
}

func generateXIconRGB565LE(width, height int) []byte {
	out := make([]byte, width*height*2)

	// Dark background with a red X.
	bgLo, bgHi := rgb565LE(8, 8, 8)
	fgLo, fgHi := rgb565LE(255, 70, 70)

	maxWH := width
	if height > maxWH {
		maxWH = height
	}
	thickness := maxWH / 20
	if thickness < 2 {
		thickness = 2
	}
	scale := maxWH
	if scale < 1 {
		scale = 1
	}
	xDen := width - 1
	yDen := height - 1
	if xDen < 1 {
		xDen = 1
	}
	if yDen < 1 {
		yDen = 1
	}

	for y := 0; y < height; y++ {
		for x := 0; x < width; x++ {
			diagA := absInt(x*yDen - y*xDen)
			diagB := absInt(x*yDen - (height-1-y)*xDen)
			useFg := diagA <= thickness*scale || diagB <= thickness*scale
			i := (y*width + x) * 2
			if useFg {
				out[i] = fgLo
				out[i+1] = fgHi
			} else {
				out[i] = bgLo
				out[i+1] = bgHi
			}
		}
	}

	return out
}

func parseTargetSize(values url.Values) (int, int, error) {
	if size := strings.TrimSpace(values.Get("size")); size != "" {
		w, h, err := parseSize(size)
		if err != nil {
			return 0, 0, err
		}
		return w, h, nil
	}

	wRaw := strings.TrimSpace(values.Get("w"))
	hRaw := strings.TrimSpace(values.Get("h"))
	if wRaw == "" || hRaw == "" {
		return 0, 0, errors.New("missing size (use size=28 or size=320x240)")
	}

	w, err := strconv.Atoi(wRaw)
	if err != nil {
		return 0, 0, errors.New("invalid w parameter")
	}
	h, err := strconv.Atoi(hRaw)
	if err != nil {
		return 0, 0, errors.New("invalid h parameter")
	}
	if err := validateDimensions(w, h); err != nil {
		return 0, 0, err
	}
	return w, h, nil
}

func parseSize(raw string) (int, int, error) {
	raw = strings.ToLower(strings.TrimSpace(raw))
	if raw == "" {
		return 0, 0, errors.New("size cannot be empty")
	}

	for _, sep := range []string{"x", ","} {
		if strings.Contains(raw, sep) {
			parts := strings.SplitN(raw, sep, 2)
			if len(parts) != 2 {
				return 0, 0, errors.New("invalid size format")
			}
			w, err := strconv.Atoi(strings.TrimSpace(parts[0]))
			if err != nil {
				return 0, 0, errors.New("invalid size width")
			}
			h, err := strconv.Atoi(strings.TrimSpace(parts[1]))
			if err != nil {
				return 0, 0, errors.New("invalid size height")
			}
			if err := validateDimensions(w, h); err != nil {
				return 0, 0, err
			}
			return w, h, nil
		}
	}

	n, err := strconv.Atoi(raw)
	if err != nil {
		return 0, 0, errors.New("invalid size parameter")
	}
	if err := validateDimensions(n, n); err != nil {
		return 0, 0, err
	}
	return n, n, nil
}

func validateDimensions(w, h int) error {
	if w <= 0 || h <= 0 {
		return errors.New("size must be positive")
	}
	if w > maxDimension || h > maxDimension {
		return fmt.Errorf("size exceeds max %d pixels per axis", maxDimension)
	}
	return nil
}

func hasExplicitSize(values url.Values) bool {
	if strings.TrimSpace(values.Get("size")) != "" {
		return true
	}
	return strings.TrimSpace(values.Get("w")) != "" || strings.TrimSpace(values.Get("h")) != ""
}

func normalizeMdiIconName(raw string) (string, error) {
	icon := strings.TrimSpace(raw)
	if strings.HasPrefix(strings.ToLower(icon), "mdi:") {
		icon = icon[4:]
	}
	icon = strings.TrimSpace(icon)
	if icon == "" {
		return "", errors.New("icon cannot be empty")
	}

	for _, r := range icon {
		if (r >= 'a' && r <= 'z') || (r >= '0' && r <= '9') || r == '-' {
			continue
		}
		return "", errors.New("icon must match [a-z0-9-] (example: weather-partly-cloudy)")
	}
	return icon, nil
}

func normalizeHexColor(raw string) (string, error) {
	color := strings.TrimSpace(raw)
	if color == "" {
		return "", nil
	}
	color = strings.TrimPrefix(color, "#")
	if len(color) != 6 {
		return "", errors.New("color must be 6 hex digits (RRGGBB)")
	}
	for _, r := range color {
		if (r >= '0' && r <= '9') || (r >= 'a' && r <= 'f') || (r >= 'A' && r <= 'F') {
			continue
		}
		return "", errors.New("color must be 6 hex digits (RRGGBB)")
	}
	return strings.ToUpper(color), nil
}

func sanitizeHeaderValue(raw string) string {
	value := strings.TrimSpace(raw)
	if value == "" {
		return ""
	}
	value = strings.ReplaceAll(value, "\r", "")
	value = strings.ReplaceAll(value, "\n", "")
	if len(value) > 256 {
		value = value[:256]
	}
	return strings.TrimSpace(value)
}

func firstNonEmpty(values ...string) string {
	for _, value := range values {
		trimmed := strings.TrimSpace(value)
		if trimmed != "" {
			return trimmed
		}
	}
	return ""
}

func (cfg serverConfig) fetchSourceBytes(ctx context.Context, rawURL string, opts fetchOptions) ([]byte, string, error) {
	parsed, err := url.Parse(rawURL)
	if err != nil {
		return nil, "", fmt.Errorf("invalid url: %w", err)
	}
	if parsed.Scheme != "http" && parsed.Scheme != "https" {
		return nil, "", errors.New("url must use http or https")
	}

	req, err := http.NewRequestWithContext(ctx, http.MethodGet, rawURL, nil)
	if err != nil {
		return nil, "", fmt.Errorf("build request: %w", err)
	}
	req.Header.Set("Accept", "image/*,*/*;q=0.8")
	if ua := firstNonEmpty(sanitizeHeaderValue(opts.userAgent), sanitizeHeaderValue(cfg.userAgent), defaultUserAgent); ua != "" {
		req.Header.Set("User-Agent", ua)
	}
	if referer := sanitizeHeaderValue(opts.referer); referer != "" {
		req.Header.Set("Referer", referer)
	}

	client := &http.Client{Timeout: 15 * time.Second}
	resp, err := client.Do(req)
	if err != nil {
		return nil, "", fmt.Errorf("fetch failed: %w", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		return nil, "", fmt.Errorf("fetch failed: HTTP %d", resp.StatusCode)
	}

	limited := io.LimitReader(resp.Body, cfg.maxDownloadBytes+1)
	body, err := io.ReadAll(limited)
	if err != nil {
		return nil, "", fmt.Errorf("read failed: %w", err)
	}
	if int64(len(body)) > cfg.maxDownloadBytes {
		return nil, "", fmt.Errorf("source exceeds %d bytes", cfg.maxDownloadBytes)
	}
	return body, resp.Request.URL.String(), nil
}

func decodeImageBytes(body []byte, width, height int) (image.Image, string, error) {
	if len(body) == 0 {
		return nil, "", errors.New("decode failed: empty payload")
	}
	img, fmtName, err := image.Decode(bytes.NewReader(body))
	if err == nil {
		return img, fmtName, nil
	}

	svgImg, svgErr := decodeSVGBytes(body, width, height)
	if svgErr == nil {
		return svgImg, "svg", nil
	}

	return nil, "", fmt.Errorf("decode failed: %v; svg fallback failed: %v", err, svgErr)
}

func decodeSVGBytes(svg []byte, width, height int) (image.Image, error) {
	if width <= 0 || height <= 0 {
		return nil, errors.New("invalid raster target size")
	}
	icon, err := oksvg.ReadIconStream(bytes.NewReader(svg))
	if err != nil {
		return nil, err
	}
	icon.SetTarget(0, 0, float64(width), float64(height))
	dst := image.NewRGBA(image.Rect(0, 0, width, height))
	scanner := rasterx.NewScannerGV(width, height, dst, dst.Bounds())
	raster := rasterx.NewDasher(width, height, scanner)
	icon.Draw(raster, 1.0)
	return dst, nil
}

func resizeNearest(src image.Image, outW, outH int) *image.NRGBA {
	dst := image.NewNRGBA(image.Rect(0, 0, outW, outH))
	b := src.Bounds()
	srcW := b.Dx()
	srcH := b.Dy()

	for y := 0; y < outH; y++ {
		sy := b.Min.Y + (y*srcH)/outH
		for x := 0; x < outW; x++ {
			sx := b.Min.X + (x*srcW)/outW
			c := color.NRGBAModel.Convert(src.At(sx, sy)).(color.NRGBA)
			dst.SetNRGBA(x, y, c)
		}
	}
	return dst
}

func encodeRGB565LE(img image.Image) []byte {
	b := img.Bounds()
	out := make([]byte, 0, b.Dx()*b.Dy()*2)

	for y := b.Min.Y; y < b.Max.Y; y++ {
		for x := b.Min.X; x < b.Max.X; x++ {
			c := color.NRGBAModel.Convert(img.At(x, y)).(color.NRGBA)
			r := c.R
			g := c.G
			bl := c.B

			if c.A < 0xFF {
				r = uint8((uint16(r) * uint16(c.A)) / 0xFF)
				g = uint8((uint16(g) * uint16(c.A)) / 0xFF)
				bl = uint8((uint16(bl) * uint16(c.A)) / 0xFF)
			}

			v := (uint16(r&0xF8) << 8) | (uint16(g&0xFC) << 3) | (uint16(bl) >> 3)
			out = append(out, byte(v&0xFF), byte((v>>8)&0xFF))
		}
	}
	return out
}

func writeError(w http.ResponseWriter, status int, message string) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(map[string]string{"error": message})
}
