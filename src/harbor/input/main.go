// Harbor's wire decoder is Ultraviolet. This bridge only translates typed events.
package main

/*
#include <stdlib.h>
#include "../input.h"
*/
import "C"
import (
	"context"
	uv "github.com/charmbracelet/ultraviolet"
	"github.com/muesli/cancelreader"
	"os"
	"time"
)

var events chan uv.Event
var cancel context.CancelFunc
var reader cancelreader.CancelReader
var done chan struct{}
var pending uv.Event

//export HarborInputStart
func HarborInputStart() C.int {
	var err error
	reader, err = cancelreader.NewReader(os.Stdin)
	if err != nil {
		return 0
	}
	var ctx context.Context
	ctx, cancel = context.WithCancel(context.Background())
	events = make(chan uv.Event, 256)
	pending = nil
	d := uv.NewTerminalReader(reader, os.Getenv("TERM"))
	d.EscTimeout = 100 * time.Millisecond
	ch := events
	done = make(chan struct{})
	finished := done
	go func() { defer close(finished); defer close(ch); _ = d.StreamEvents(ctx, ch) }()
	return 1
}

//export HarborInputStop
func HarborInputStop() {
	if cancel != nil {
		cancel()
		reader.Cancel()
		<-done
		reader.Close()
		cancel = nil
	}
}

func translate(e uv.Event) (out C.HarborInputEvent) {
	switch e := e.(type) {
	case uv.KeyPressEvent:
		k := uv.Key(e)
		out.kind = 1
		out.code = C.int(k.Code)
		out.mods = C.int(k.Mod)
		special := map[rune]int{uv.KeyLeft: -1, uv.KeyRight: -2, uv.KeyUp: -3, uv.KeyDown: -4, uv.KeyHome: -5, uv.KeyEnd: -6, uv.KeyPgUp: -7, uv.KeyPgDown: -8, uv.KeyDelete: -9, uv.KeyInsert: -10, uv.KeyKpEnter: 13}
		if code, ok := special[k.Code]; ok {
			out.code = C.int(code)
		} else if k.Code > 0x10ffff {
			out.kind = 0
		}
		if k.Text != "" && k.Mod&(uv.ModCtrl|uv.ModAlt|uv.ModMeta|uv.ModSuper) == 0 {
			out.kind = 2
			out.text = C.CString(k.Text)
		}
	case uv.FocusEvent:
		out.kind = 5
	case uv.PasteEvent:
		out.kind = 3
		out.text = C.CString(e.Content)
	case uv.MouseClickEvent:
		out = mouse(uv.Mouse(e), 1)
	case uv.MouseReleaseEvent:
		out = mouse(uv.Mouse(e), 2)
	case uv.MouseMotionEvent:
		out = mouse(uv.Mouse(e), 3)
	case uv.MouseWheelEvent:
		out = mouse(uv.Mouse(e), 1)
	}
	return
}
func mouse(m uv.Mouse, action int) (out C.HarborInputEvent) {
	out.kind = 4
	out.x = C.int(m.X)
	out.y = C.int(m.Y)
	out.button = C.int(m.Button)
	out.mods = C.int(m.Mod)
	out.action = C.int(action)
	return
}

// Batch only already queued, consecutive wheel ticks. No timer or synthetic
// momentum: isolated ticks stay precise and a direction change/key/click keeps
// its place. Rendering each tick separately lets a hard flick outlive its input.
func wheelBatch(e uv.Event) C.HarborInputEvent {
	wheel, ok := e.(uv.MouseWheelEvent)
	if !ok {
		return translate(e)
	}
	count := 1
	for count < 256 {
		select {
		case next, open := <-events:
			if !open {
				out := translate(e)
				out.code = C.int(count)
				return out
			}
			if other, ok := next.(uv.MouseWheelEvent); ok && other == wheel {
				count++
			} else {
				pending = next
				out := translate(e)
				out.code = C.int(count)
				return out
			}
		default:
			out := translate(e)
			out.code = C.int(count)
			return out
		}
	}
	out := translate(e)
	out.code = C.int(count)
	return out
}

//export HarborInputPoll
func HarborInputPoll(ms C.int) C.HarborInputEvent {
	if pending != nil {
		e := pending
		pending = nil
		return wheelBatch(e)
	}
	select {
	case e, ok := <-events:
		if !ok {
			return C.HarborInputEvent{kind: -1}
		}
		return wheelBatch(e)
	case <-time.After(time.Duration(ms) * time.Millisecond):
		return C.HarborInputEvent{}
	}
}
func main() {}
