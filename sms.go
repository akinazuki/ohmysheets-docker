package main

/*
#cgo CFLAGS: -I.
#cgo LDFLAGS: -L/app/lib -landroid_stubs -lsource-lib -llept -ljpgt -lpngt -ldl -lm -Wl,-rpath,/app/lib

#include "sms_lib.h"
#include <stdlib.h>

// Defined in bridge.c
extern int bridge_init(void);
extern SmsResult bridge_analyze(void **pix_pages, int n);
extern void *bridge_pix_read(const char *path);
extern void *bridge_pix_read_mem(const unsigned char *data, int len);
extern int bridge_pix_width(void *p);
extern int bridge_pix_height(void *p);
extern void bridge_pix_free(void *p);
*/
import "C"
import (
	"fmt"
	"unsafe"
)

// Pix is an opaque handle to a Leptonica PIX image.
type Pix struct {
	ptr unsafe.Pointer
}

// PixRead loads an image from a file path. Caller must call Free().
func PixRead(path string) (*Pix, error) {
	cPath := C.CString(path)
	defer C.free(unsafe.Pointer(cPath))
	p := C.bridge_pix_read(cPath)
	if p == nil {
		return nil, fmt.Errorf("cannot read image: %s", path)
	}
	return &Pix{ptr: p}, nil
}

// PixReadMem loads an image from a byte slice. Caller must call Free().
func PixReadMem(data []byte) (*Pix, error) {
	if len(data) == 0 {
		return nil, fmt.Errorf("empty image data")
	}
	p := C.bridge_pix_read_mem((*C.uchar)(unsafe.Pointer(&data[0])), C.int(len(data)))
	if p == nil {
		return nil, fmt.Errorf("cannot decode image from memory (%d bytes)", len(data))
	}
	return &Pix{ptr: p}, nil
}

// Width returns image width in pixels.
func (p *Pix) Width() int { return int(C.bridge_pix_width(p.ptr)) }

// Height returns image height in pixels.
func (p *Pix) Height() int { return int(C.bridge_pix_height(p.ptr)) }

// Free releases the underlying C PIX memory.
func (p *Pix) Free() {
	if p.ptr != nil {
		C.bridge_pix_free(p.ptr)
		p.ptr = nil
	}
}

// AnalyzeResult holds the analysis output.
type AnalyzeResult struct {
	MIDI       []byte
	TotalNotes int
	TotalBars  int
	NumStaves  int
}

// EngineInit initializes the SMS engine. Must be called once before Analyze.
func EngineInit() error {
	rc := C.bridge_init()
	if rc != 0 {
		return fmt.Errorf("sms_init failed (code %d)", int(rc))
	}
	return nil
}

// Analyze runs the SMS engine on pre-loaded PIX pages
// and returns the MIDI data as a byte slice.
// If progress is non-nil, progress events are sent to it during analysis.
// The channel is closed when Analyze returns.
func Analyze(pages []*Pix, progress chan<- Progress) (*AnalyzeResult, error) {
	if len(pages) == 0 {
		return nil, fmt.Errorf("no input pages")
	}

	// Register progress channel for the C callback
	progressMu.Lock()
	progressCh = progress
	progressMu.Unlock()
	defer func() {
		progressMu.Lock()
		progressCh = nil
		progressMu.Unlock()
		if progress != nil {
			close(progress)
		}
	}()

	ptrs := make([]unsafe.Pointer, len(pages))
	for i, p := range pages {
		if p == nil || p.ptr == nil {
			return nil, fmt.Errorf("page %d: nil PIX", i)
		}
		ptrs[i] = p.ptr
	}

	result := C.bridge_analyze(&ptrs[0], C.int(len(pages)))

	if result.result_code != 0 {
		return nil, fmt.Errorf("analysis failed (code %d): %s",
			int(result.result_code), C.GoString(&result.error_msg[0]))
	}

	midiLen := int(result.midi_len)
	midi := make([]byte, midiLen)
	copy(midi, unsafe.Slice((*byte)(unsafe.Pointer(result.midi_data)), midiLen))
	C.free(unsafe.Pointer(result.midi_data))

	return &AnalyzeResult{
		MIDI:       midi,
		TotalNotes: int(result.total_notes),
		TotalBars:  int(result.total_bars),
		NumStaves:  int(result.num_staves),
	}, nil
}
