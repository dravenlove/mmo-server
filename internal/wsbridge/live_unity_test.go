package wsbridge

import (
	"context"
	"net/http/httptest"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

// Explicit opt-in: registers at most two reusable integration profiles on the supplied gateway.
// The bridge is an in-process fixture and closes with the bounded test; no background service is left running.
func TestLiveUnityClient(t *testing.T) {
	upstream := os.Getenv("MMO_LIVE_GATEWAY")
	clientProject := os.Getenv("UNITY_CLIENT_PROJECT")
	if upstream == "" || clientProject == "" {
		t.Skip("set MMO_LIVE_GATEWAY and UNITY_CLIENT_PROJECT to enable real-server integration")
	}
	s, err := New(Config{Upstream: upstream, AllowedOrigins: []string{"http://127.0.0.1:8088"}})
	if err != nil {
		t.Fatal(err)
	}
	server := httptest.NewServer(s)
	defer server.Close()
	defer s.Close()
	ctx, cancel := context.WithTimeout(context.Background(), 80*time.Second)
	defer cancel()
	endpoint := "ws" + strings.TrimPrefix(server.URL, "http") + "/ws"
	cmd := exec.CommandContext(ctx, "dotnet", "run", "--project", filepath.Join(clientProject, "Tests", "Networking", "Networking.csproj"), "--configuration", "Release", "--", "--live", endpoint)
	cmd.Dir = clientProject
	output, err := cmd.CombinedOutput()
	t.Log(string(output))
	if err != nil {
		t.Fatalf("real Unity core / WebSocket / TCP integration failed: %v", err)
	}
}
