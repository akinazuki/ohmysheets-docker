package main

import (
	"fmt"
	"os"
	"strings"
)

func main() {
	args := os.Args[1:]
	if len(args) == 0 {
		fmt.Fprintln(os.Stderr, "Usage: sms <image.png> [image2 ...] [output.mid]")
		fmt.Fprintln(os.Stderr, "  Single page:  sms page.png out.mid")
		fmt.Fprintln(os.Stderr, "  Multi-page:   sms p0.png p1.png p2.png out.mid")
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

	// Progress channel — print to stderr as stages complete
	progCh := make(chan Progress, 8)
	go func() {
		for p := range progCh {
			if p.Name != "" {
				fmt.Fprintf(os.Stderr, "  Page %d/%d [%d/7] %s\n",
					p.Page+1, p.TotalPages, p.Stage, p.Name)
			}
		}
	}()

	res, err := Analyze(pages, progCh)
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
