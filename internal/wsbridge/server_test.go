package wsbridge

import (
	"bufio"
	"bytes"
	"encoding/json"
	"log"
	"net"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/gorilla/websocket"
)

func echoGateway(t *testing.T) string {
	t.Helper()
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { listener.Close() })
	go func() {
		for {
			conn, err := listener.Accept()
			if err != nil {
				return
			}
			go func() {
				defer conn.Close()
				scanner := bufio.NewScanner(conn)
				scanner.Buffer(make([]byte, 4096), MaxFrame)
				for scanner.Scan() {
					conn.Write(append(append([]byte{}, scanner.Bytes()...), '\n'))
				}
			}()
		}
	}()
	return listener.Addr().String()
}

func testBridge(t *testing.T) (*Server, string) {
	t.Helper()
	s, err := New(Config{Upstream: echoGateway(t), AllowedOrigins: []string{"https://gate.draven.world"}})
	if err != nil {
		t.Fatal(err)
	}
	httpServer := httptest.NewServer(s)
	t.Cleanup(func() { s.Close(); httpServer.Close() })
	return s, "ws" + strings.TrimPrefix(httpServer.URL, "http") + "/ws"
}

func TestJSONLFraming(t *testing.T) {
	_, endpoint := testBridge(t)
	conn, _, err := websocket.DefaultDialer.Dial(endpoint, http.Header{"Origin": {"https://gate.draven.world"}})
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close()
	// Whitespace/newlines in a WS message must not split the upstream JSONL request.
	for _, request := range []string{"{\n\"id\":\"1\",\"op\":\"ping\"\n}", `{"id":"2","op":"move","seq":1,"dx":1,"dy":0}`} {
		if err = conn.WriteMessage(websocket.TextMessage, []byte(request)); err != nil {
			t.Fatal(err)
		}
		conn.SetReadDeadline(time.Now().Add(2 * time.Second))
		_, reply, err := conn.ReadMessage()
		if err != nil {
			t.Fatal(err)
		}
		if !json.Valid(reply) || strings.Contains(string(reply), "\n") {
			t.Fatalf("invalid forwarded frame: %q", reply)
		}
	}
}

func TestOriginAndQueryRejection(t *testing.T) {
	_, endpoint := testBridge(t)
	for _, candidate := range []struct{ origin, suffix string }{{"https://other.example", ""}, {"", ""}, {"https://gate.draven.world", "?token=do-not-log"}} {
		conn, response, err := websocket.DefaultDialer.Dial(endpoint+candidate.suffix, http.Header{"Origin": {candidate.origin}})
		if conn != nil {
			conn.Close()
		}
		if response != nil {
			response.Body.Close()
		}
		if err == nil {
			t.Fatal("disallowed browser request accepted")
		}
	}
}

func TestBinaryAndOversizedMessagesClose(t *testing.T) {
	_, endpoint := testBridge(t)
	for _, test := range []struct {
		kind      int
		payload   []byte
		closeCode int
	}{{websocket.BinaryMessage, []byte("hello"), websocket.CloseUnsupportedData}, {websocket.TextMessage, []byte(strings.Repeat("x", MaxFrame)), websocket.CloseMessageTooBig}} {
		conn, _, err := websocket.DefaultDialer.Dial(endpoint, http.Header{"Origin": {"https://gate.draven.world"}})
		if err != nil {
			t.Fatal(err)
		}
		conn.WriteMessage(test.kind, test.payload)
		conn.SetReadDeadline(time.Now().Add(2 * time.Second))
		if _, _, err = conn.ReadMessage(); !websocket.IsCloseError(err, test.closeCode) {
			t.Fatalf("expected close code %d, got %v", test.closeCode, err)
		}
		conn.Close()
	}
}

func TestShutdownClosesHijackedConnections(t *testing.T) {
	s, endpoint := testBridge(t)
	conn, _, err := websocket.DefaultDialer.Dial(endpoint, http.Header{"Origin": {"https://gate.draven.world"}})
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close()
	s.Close()
	conn.SetReadDeadline(time.Now().Add(2 * time.Second))
	if _, _, err = conn.ReadMessage(); err == nil {
		t.Fatal("connection survived shutdown")
	}
}

func TestConfigurationValidation(t *testing.T) {
	for _, cfg := range []Config{{Upstream: "http://example.com"}, {Upstream: "127.0.0.1:7000"}, {Upstream: "127.0.0.1:7000", AllowedOrigins: []string{"https://example.com/path"}}, {Upstream: "127.0.0.1:7000", AllowedOrigins: []string{"*"}}} {
		if _, err := New(cfg); err == nil {
			t.Fatal("invalid bridge configuration accepted")
		}
	}
}

func TestExactUTF8FrameBoundary(t *testing.T) {
	_, endpoint := testBridge(t)
	conn, _, err := websocket.DefaultDialer.Dial(endpoint, http.Header{"Origin": {"https://gate.draven.world"}})
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close()
	body := []byte(`{"value":"` + strings.Repeat("a", MaxFrame-1-len(`{"value":""}`)) + `"}`)
	if len(body) != 65535 {
		t.Fatal("invalid boundary fixture")
	}
	if err = conn.WriteMessage(websocket.TextMessage, body); err != nil {
		t.Fatal(err)
	}
	conn.SetReadDeadline(time.Now().Add(2 * time.Second))
	kind, reply, err := conn.ReadMessage()
	if err != nil || kind != websocket.TextMessage || len(reply) != len(body) {
		t.Fatalf("65535-byte JSON did not round-trip: %v bytes=%d", err, len(reply))
	}
}

type diagnosticBuffer struct {
	mu sync.Mutex
	b  bytes.Buffer
}

func (b *diagnosticBuffer) Write(p []byte) (int, error) {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.b.Write(p)
}

func (b *diagnosticBuffer) String() string {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.b.String()
}

func TestDiagnosticsExcludeCredentials(t *testing.T) {
	var output diagnosticBuffer
	s, err := New(Config{Upstream: echoGateway(t), AllowedOrigins: []string{"https://game.example"}, Logger: log.New(&output, "", 0)})
	if err != nil {
		t.Fatal(err)
	}
	h := httptest.NewServer(s)
	t.Cleanup(func() { s.Close(); h.Close() })
	endpoint := "ws" + strings.TrimPrefix(h.URL, "http") + "/ws"
	for _, candidate := range []struct{ origin, suffix string }{
		{"https://unexpected.example", ""},
		{"https://user:origin-secret@unexpected.example", ""},
		{"https://unexpected.example/?token=origin-secret", ""},
		{"https://game.example", "?token=query-secret"},
	} {
		conn, response, err := websocket.DefaultDialer.Dial(endpoint+candidate.suffix, http.Header{"Origin": {candidate.origin}, "Cookie": {"token=cookie-secret"}, "Authorization": {"Bearer header-secret"}})
		if conn != nil {
			conn.Close()
		}
		if response != nil {
			response.Body.Close()
		}
		if err == nil {
			t.Fatal("unsafe handshake accepted")
		}
	}
	conn, _, err := websocket.DefaultDialer.Dial(endpoint, http.Header{"Origin": {"https://game.example"}})
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close()
	if err := conn.WriteMessage(websocket.TextMessage, []byte(`{"token":"payload-secret"}`)); err != nil {
		t.Fatal(err)
	}
	conn.SetReadDeadline(time.Now().Add(2 * time.Second))
	if _, _, err := conn.ReadMessage(); err != nil {
		t.Fatal(err)
	}
	conn.WriteControl(websocket.CloseMessage, websocket.FormatCloseMessage(websocket.CloseNormalClosure, "close-secret"), time.Now().Add(time.Second))
	conn.ReadMessage()
	deadline := time.Now().Add(2 * time.Second)
	for !strings.Contains(output.String(), "event=closed") && time.Now().Before(deadline) {
		time.Sleep(time.Millisecond)
	}
	result := output.String()
	for _, want := range []string{"reason=origin_not_allowed status=403", "origin=\"https://unexpected.example\"", "origin=\"(invalid)\"", "reason=query_not_supported status=400", "event=connected", "reason=peer_closed", "peer_code=1000"} {
		if !strings.Contains(result, want) {
			t.Errorf("missing diagnostic %q: %s", want, result)
		}
	}
	if strings.Contains(result, "secret") || strings.Contains(result, "token=") {
		t.Fatalf("credentials leaked in diagnostic: %s", result)
	}
}

func TestInvalidTextHasExplicitCloseCode(t *testing.T) {
	_, endpoint := testBridge(t)
	conn, _, err := websocket.DefaultDialer.Dial(endpoint, http.Header{"Origin": {"https://gate.draven.world"}})
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close()
	conn.WriteMessage(websocket.TextMessage, []byte(`{"token":"do-not-log"`))
	conn.SetReadDeadline(time.Now().Add(2 * time.Second))
	_, _, err = conn.ReadMessage()
	if !websocket.IsCloseError(err, websocket.CloseInvalidFramePayloadData) {
		t.Fatalf("expected explicit 1007, got %v", err)
	}
}

func TestGatewayEOFHasExplicitCloseCode(t *testing.T) {
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer listener.Close()
	go func() {
		conn, err := listener.Accept()
		if err == nil {
			conn.Close()
		}
	}()
	s, err := New(Config{Upstream: listener.Addr().String(), AllowedOrigins: []string{"https://game.example"}})
	if err != nil {
		t.Fatal(err)
	}
	h := httptest.NewServer(s)
	defer h.Close()
	defer s.Close()
	conn, _, err := websocket.DefaultDialer.Dial("ws"+strings.TrimPrefix(h.URL, "http")+"/ws", http.Header{"Origin": {"https://game.example"}})
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close()
	conn.SetReadDeadline(time.Now().Add(2 * time.Second))
	_, _, err = conn.ReadMessage()
	if !websocket.IsCloseError(err, websocket.CloseInternalServerErr) {
		t.Fatalf("expected explicit 1011 on gateway EOF, got %v", err)
	}
}
