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
	Stage      int    // analysis stage (2-7)
	Name       string // human-readable stage name, may be empty
}

// Global progress channel, protected by mutex.
// Set by Analyze before calling C, cleared after.
var (
	progressMu sync.Mutex
	progressCh chan<- Progress
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
