package wsbridge

import (
	"bytes"
	"net/http"
	"strings"
	"testing"
	"time"

	"github.com/gorilla/websocket"
)

func TestNegotiatedCompressionPreservesJSONAndBoundsDecodedMessage(t *testing.T) {
	_, endpoint := testBridge(t)
	dialer := websocket.Dialer{EnableCompression: true}
	connection, response, err := dialer.Dial(endpoint, http.Header{"Origin": {"https://gate.draven.world"}})
	if err != nil {
		t.Fatal(err)
	}
	defer connection.Close()
	if !strings.Contains(response.Header.Get("Sec-WebSocket-Extensions"), "permessage-deflate") {
		t.Fatal("compression was not negotiated")
	}
	message := []byte(`{"id":"compression","payload":"` + strings.Repeat("repeat-", 3000) + `"}`)
	if err = connection.WriteMessage(websocket.TextMessage, message); err != nil {
		t.Fatal(err)
	}
	connection.SetReadDeadline(time.Now().Add(2 * time.Second))
	_, reply, err := connection.ReadMessage()
	if err != nil || !bytes.Equal(reply, message) {
		t.Fatalf("compressed JSON changed: %v", err)
	}
	oversized := []byte(`{"id":"bomb","payload":"` + strings.Repeat("a", MaxFrame*4) + `"}`)
	if err = connection.WriteMessage(websocket.TextMessage, oversized); err != nil {
		t.Fatal(err)
	}
	connection.SetReadDeadline(time.Now().Add(2 * time.Second))
	_, _, err = connection.ReadMessage()
	if !websocket.IsCloseError(err, websocket.CloseMessageTooBig) {
		t.Fatalf("oversized decoded message accepted: %v", err)
	}
}
