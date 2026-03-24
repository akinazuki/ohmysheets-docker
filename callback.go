package main

/*
#include <stdlib.h>
*/
import "C"
import (
	"fmt"
	"os"
	"sync"
	"unsafe"
)

// Progress represents a single progress event from the analysis engine.
type Progress struct {
	Page       int    // 0-based page index
	TotalPages int    // total number of pages
	Stage      int    // analysis stage (2-8)
	Name       string // human-readable stage name, may be empty
}

// PageDone is sent when a page's analysis is complete, with its MIDI data.
type PageDone struct {
	Page       int    // 0-based page index
	TotalPages int    // total number of pages
	MIDI       []byte // per-page MIDI data
	Notes      int    // number of notes in this page
}

// Global channels, protected by mutex.
// Set by Analyze before calling C, cleared after.
var (
	progressMu sync.Mutex
	progressCh chan<- Progress
	pageDoneCh chan<- PageDone
)

//export goLogCallback
func goLogCallback(msg *C.char, _ unsafe.Pointer) {
	fmt.Fprintln(os.Stderr, C.GoString(msg))
}

//export goProgressCallback
func goProgressCallback(page C.int, totalPages C.int, stage C.int, name *C.char, _ unsafe.Pointer) {
	p := Progress{
		Page:       int(page),
		TotalPages: int(totalPages),
		Stage:      int(stage),
	}
	if name != nil {
		p.Name = C.GoString(name)
	}

	// Send to channel if one is registered
	progressMu.Lock()
	ch := progressCh
	progressMu.Unlock()

	if ch != nil {
		select {
		case ch <- p:
		default: // non-blocking: drop if consumer is slow
		}
	}
}

//export goPageDoneCallback
func goPageDoneCallback(page C.int, totalPages C.int, midiData *C.uchar, midiLen C.int, notes C.int, _ unsafe.Pointer) {
	progressMu.Lock()
	ch := pageDoneCh
	progressMu.Unlock()

	if ch == nil {
		return
	}

	// Copy MIDI data into Go memory (C data is only valid during this call)
	midi := make([]byte, int(midiLen))
	copy(midi, unsafe.Slice((*byte)(unsafe.Pointer(midiData)), int(midiLen)))

	select {
	case ch <- PageDone{
		Page:       int(page),
		TotalPages: int(totalPages),
		MIDI:       midi,
		Notes:      int(notes),
	}:
	default:
	}
}
