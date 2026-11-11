package wsbridge

import (
	"bufio"
	"encoding/json"
	"net"
	"net/http"
	"net/http/httptest"
	"strings"
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
		kind    int
		payload []byte
	}{{websocket.BinaryMessage, []byte("hello")}, {websocket.TextMessage, []byte(strings.Repeat("x", MaxFrame))}} {
		conn, _, err := websocket.DefaultDialer.Dial(endpoint, http.Header{"Origin": {"https://gate.draven.world"}})
		if err != nil {
			t.Fatal(err)
		}
		conn.WriteMessage(test.kind, test.payload)
		conn.SetReadDeadline(time.Now().Add(2 * time.Second))
		if _, _, err = conn.ReadMessage(); err == nil {
			t.Fatal("invalid frame was forwarded")
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
