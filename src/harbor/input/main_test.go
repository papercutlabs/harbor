package main

import (
	"context"
	uv "github.com/charmbracelet/ultraviolet"
	"io"
	"os"
	"testing"
	"time"
)

func TestStreamDecoder(t *testing.T) {
	cases := []struct {
		name   string
		chunks []string
		check  func(uv.Event) bool
	}{
		{"fragmented CSI", []string{"\x1b", "[", "D"}, func(e uv.Event) bool { k, ok := e.(uv.KeyPressEvent); return ok && k.Code == uv.KeyLeft }},
		{"fragmented UTF8", []string{"\xe7", "\x95", "\x8c"}, func(e uv.Event) bool { k, ok := e.(uv.KeyPressEvent); return ok && k.Code == '界' }},
		{"modified arrow", []string{"\x1b[1;5C"}, func(e uv.Event) bool {
			k, ok := e.(uv.KeyPressEvent)
			return ok && k.Code == uv.KeyRight && k.Mod == uv.ModCtrl
		}},
		{"command O", []string{"\x1b[111;9u"}, func(e uv.Event) bool {
			k := translate(e)
			return k.kind == 1 && k.code == 'o' && k.mods == 32
		}},
		{"legacy option slash", []string{"\x1b/"}, func(e uv.Event) bool {
			k := translate(e)
			return k.kind == 1 && k.code == '/' && k.mods == 2
		}},
		{"enhanced option slash", []string{"\x1b[47;3u"}, func(e uv.Event) bool {
			k := translate(e)
			return k.kind == 1 && k.code == '/' && k.mods == 2
		}},
		{"plain slash", []string{"/"}, func(e uv.Event) bool {
			k := translate(e)
			return k.kind == 2
		}},
		{"kitty repeat", []string{"\x1b[57350;1:2u"}, func(e uv.Event) bool { k, ok := e.(uv.KeyPressEvent); return ok && k.Code == uv.KeyLeft }},
		{"kitty release", []string{"\x1b[57350;1:3u"}, func(e uv.Event) bool { _, ok := e.(uv.KeyReleaseEvent); return ok && translate(e).kind == 0 }},
		{"unknown CSI", []string{"\x1b[99999z"}, func(e uv.Event) bool { return translate(e).kind == 0 }},
		{"escape", []string{"\x1b"}, func(e uv.Event) bool { k, ok := e.(uv.KeyPressEvent); return ok && k.Code == uv.KeyEscape }},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			r, w := io.Pipe()
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()
			defer r.Close()
			defer w.Close()
			events := make(chan uv.Event, 16)
			d := uv.NewTerminalReader(r, "xterm-256color")
			d.EscTimeout = 50 * time.Millisecond
			go d.StreamEvents(ctx, events)
			go func() {
				for _, chunk := range tc.chunks {
					w.Write([]byte(chunk))
					time.Sleep(5 * time.Millisecond)
				}
			}()
			select {
			case e := <-events:
				if !tc.check(e) {
					t.Fatalf("unexpected event %#v", e)
				}
			case <-time.After(time.Second):
				t.Fatal("no event")
			}
		})
	}
}

// These are real wire encodings, not constructed key events: Option is ModAlt
// in the adapter ABI, even where its wire form is Escape-b/f instead of an arrow.
func TestWordNavigationDecoder(t *testing.T) {
	cases := []struct {
		wire       string
		code, mods int
	}{
		{"\x1bb", 'b', 2}, {"\x1bf", 'f', 2},
		{"\x1b[1;3D", -1, 2}, {"\x1b[1;3C", -2, 2},
		{"\x1b[57350;3u", -1, 2}, {"\x1b[57351;3u", -2, 2},
		{"\x1b[57350;3:2u", -1, 2}, {"\x1b[57351;3:2u", -2, 2},
		{"\x1b[98;3u", 'b', 2}, {"\x1b[102;3u", 'f', 2},
		{"\x1b[27;3;98~", 'b', 2}, {"\x1b[27;3;102~", 'f', 2},
		{"\x1b[D", -1, 0}, {"\x1bOC", -2, 0},
		{"\x1b[1;5D", -1, 4}, {"\x1b[1;7C", -2, 6},
		{"\x1b[57350;9u", -1, 32}, {"\x1b[57351;4u", -2, 3},
		// Kitty Super+Backspace and legacy Ctrl+U represent Cmd+Backspace.
		// Xterm modifier 9 is Meta in this decoder, distinct from Kitty Super. Option/plain differ.
		{"\x1b[127;9u", 127, 32}, {"\x1b[27;9;127~", 127, 8}, {"\x15", 'u', 4},
		{"\x1b[127;3u", 127, 2}, {"\x7f", 127, 0},
	}
	for _, tc := range cases {
		t.Run(tc.wire, func(t *testing.T) {
			r, w := io.Pipe()
			defer r.Close()
			defer w.Close()
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()
			ch := make(chan uv.Event, 16)
			d := uv.NewTerminalReader(r, "xterm-256color")
			go d.StreamEvents(ctx, ch)
			go w.Write([]byte(tc.wire))
			select {
			case event := <-ch:
				got := translate(event)
				if got.kind != 1 || int(got.code) != tc.code || int(got.mods) != tc.mods {
					t.Fatalf("wire %q: got %#v; want key %d mods %d", tc.wire, got, tc.code, tc.mods)
				}
			case <-time.After(time.Second):
				t.Fatal("no decoded event")
			}
		})
	}
}

// The login UI temporarily owns stdin. Starting/stopping the decoder must not
// close stdin, retain a reader goroutine, or deliver an old queue after resume.
func TestInputOwnershipHandoff(t *testing.T) {
	old := os.Stdin
	r, w, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	defer r.Close()
	defer w.Close()
	defer func() { os.Stdin = old }()
	os.Stdin = r
	for i := 0; i < 3; i++ {
		if HarborInputStart() != 1 {
			t.Fatal("start failed")
		}
		w.Write([]byte("\x1b[D"))
		e := HarborInputPoll(1000)
		if e.kind != 1 || e.code != -1 {
			t.Fatalf("unexpected input: %#v", e)
		}
		stopped := make(chan struct{})
		go func() { HarborInputStop(); close(stopped) }()
		select {
		case <-stopped:
		case <-time.After(time.Second):
			t.Fatal("decoder did not release stdin")
		}
	}
	if _, err := w.Write([]byte("login")); err != nil {
		t.Fatal(err)
	}
	buf := make([]byte, 5)
	if _, err := io.ReadFull(r, buf); err != nil || string(buf) != "login" {
		t.Fatalf("stdin handoff: %q %v", buf, err)
	}
}
