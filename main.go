package main

import (
	"encoding/base64"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
	"strings"

	"github.com/gin-gonic/gin"
)

func main() {
	args := os.Args[1:]

	if len(args) >= 1 && args[0] == "serve" {
		addr := ":8080"
		if len(args) >= 2 {
			addr = args[1]
		}
		serve(addr)
		return
	}

	runCLI(args)
}

// --- CLI mode ---

func runCLI(args []string) {
	if len(args) == 0 {
		fmt.Fprintln(os.Stderr, "Usage: sms <image.png> [image2 ...] [output.mid]")
		fmt.Fprintln(os.Stderr, "       sms serve [:addr]")
		os.Exit(1)
	}

	images := args
	midPath := "/app/output/output.mid"

	if last := args[len(args)-1]; strings.HasSuffix(strings.ToLower(last), ".mid") {
		midPath = last
		images = args[:len(args)-1]
	}

	if len(images) == 0 {
		fmt.Fprintln(os.Stderr, "Error: no input images")
		os.Exit(1)
	}

	if err := EngineInit(); err != nil {
		fmt.Fprintf(os.Stderr, "Error: %v\n", err)
		os.Exit(1)
	}

	pages := make([]*Pix, len(images))
	for i, path := range images {
		pix, err := PixRead(path)
		if err != nil {
			fmt.Fprintf(os.Stderr, "Error: %v\n", err)
			os.Exit(1)
		}
		defer pix.Free()
		fmt.Fprintf(os.Stderr, "Loaded: %s (%dx%d)\n", path, pix.Width(), pix.Height())
		pages[i] = pix
	}

	progCh := make(chan Progress, 8)
	go func() {
		for p := range progCh {
			if p.Name != "" {
				fmt.Fprintf(os.Stderr, "  Page %d/%d [%d/7] %s\n",
					p.Page+1, p.TotalPages, p.Stage, p.Name)
			}
		}
	}()

	res, err := Analyze(pages, progCh, nil)
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error: %v\n", err)
		os.Exit(1)
	}

	if err := os.WriteFile(midPath, res.MIDI, 0644); err != nil {
		fmt.Fprintf(os.Stderr, "Error writing %s: %v\n", midPath, err)
		os.Exit(1)
	}

	fmt.Fprintf(os.Stderr, "Output: %s (%d bytes, %d notes, %d bars, %d staves)\n",
		midPath, len(res.MIDI), res.TotalNotes, res.TotalBars, res.NumStaves)
}

// --- HTTP server mode (gin) ---

func serve(addr string) {
	if err := EngineInit(); err != nil {
		log.Fatalf("Engine init failed: %v", err)
	}
	log.Printf("SMS engine initialized")

	r := gin.Default()

	r.GET("/health", func(c *gin.Context) {
		c.JSON(http.StatusOK, gin.H{"status": "ok"})
	})

	r.POST("/analyze", handleAnalyze)

	log.Printf("Listening on %s", addr)
	r.Run(addr)
}

// POST /analyze
// Content-Type: multipart/form-data
// Fields: "images" (one or more image files)
// Response: SSE stream with progress events, then final result with MIDI data
//
// SSE events:
//
//	event: progress
//	data: {"page":0,"total_pages":2,"stage":3,"name":"Detecting staves"}
//
//	event: result
//	data: {"total_notes":120,"total_bars":32,"num_staves":2,"midi":"<base64>"}
//
//	event: error
//	data: {"error":"..."}
func handleAnalyze(c *gin.Context) {
	form, err := c.MultipartForm()
	if err != nil {
		c.JSON(http.StatusBadRequest, gin.H{"error": fmt.Sprintf("invalid form: %v", err)})
		return
	}

	files := form.File["images"]
	if len(files) == 0 {
		c.JSON(http.StatusBadRequest, gin.H{"error": "no 'images' field"})
		return
	}

	// Set SSE headers early so we can stream during image loading
	c.Header("Content-Type", "text/event-stream")
	c.Header("Cache-Control", "no-cache")
	c.Header("Connection", "keep-alive")
	c.Status(http.StatusOK)

	w := c.Writer

	writeSSE := func(event string, data any) {
		j, _ := json.Marshal(data)
		fmt.Fprintf(w, "event: %s\ndata: %s\n\n", event, j)
		w.Flush()
	}

	pages := make([]*Pix, 0, len(files))
	defer func() {
		for _, p := range pages {
			p.Free()
		}
	}()

	for i, fh := range files {
		writeSSE("loading", gin.H{
			"page":        i,
			"total_pages": len(files),
			"filename":    fh.Filename,
		})

		f, err := fh.Open()
		if err != nil {
			writeSSE("error", gin.H{"error": fmt.Sprintf("image %d: %v", i, err)})
			return
		}
		data, err := io.ReadAll(f)
		f.Close()
		if err != nil {
			writeSSE("error", gin.H{"error": fmt.Sprintf("image %d: read error: %v", i, err)})
			return
		}

		pix, err := PixReadMem(data)
		if err != nil {
			writeSSE("error", gin.H{"error": fmt.Sprintf("image %d (%s): %v", i, fh.Filename, err)})
			return
		}

		writeSSE("loaded", gin.H{
			"page":        i,
			"total_pages": len(files),
			"filename":    fh.Filename,
			"width":       pix.Width(),
			"height":      pix.Height(),
		})

		log.Printf("  Page %d: %s (%dx%d)", i+1, fh.Filename, pix.Width(), pix.Height())
		pages = append(pages, pix)
	}

	// Progress & page-done channels → SSE
	progCh := make(chan Progress, 16)
	pdCh := make(chan PageDone, 4)
	done := make(chan struct{})

	go func() {
		progOpen, pdOpen := true, true
		for progOpen || pdOpen {
			select {
			case p, ok := <-progCh:
				if !ok {
					progOpen = false
					continue
				}
				writeSSE("progress", gin.H{
					"page":        p.Page,
					"total_pages": p.TotalPages,
					"stage":       p.Stage,
					"name":        p.Name,
				})
			case pd, ok := <-pdCh:
				if !ok {
					pdOpen = false
					continue
				}
				writeSSE("page_done", gin.H{
					"page":        pd.Page,
					"total_pages": pd.TotalPages,
					"notes":       pd.Notes,
					"midi":        base64.StdEncoding.EncodeToString(pd.MIDI),
				})
			}
		}
		close(done)
	}()

	res, err := Analyze(pages, progCh, pdCh)
	<-done // wait for all SSE events to be flushed

	if err != nil {
		writeSSE("error", gin.H{"error": err.Error()})
		return
	}

	log.Printf("  Result: %d bytes, %d notes, %d bars, %d staves",
		len(res.MIDI), res.TotalNotes, res.TotalBars, res.NumStaves)

	writeSSE("result", gin.H{
		"total_notes": res.TotalNotes,
		"total_bars":  res.TotalBars,
		"num_staves":  res.NumStaves,
		"midi":        base64.StdEncoding.EncodeToString(res.MIDI),
	})
}
