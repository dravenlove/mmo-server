package gateway

import (
	"bytes"
	"context"
	"encoding/json"
	"io"
	"log"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync/atomic"
	"testing"
	"time"
)

// 请求可能已在 Scene 提交。传输失败必须报告失败，不能自动重放 POST。
func TestUpstreamFailuresReportPhaseWithoutReplayingOrLoggingSecrets(t *testing.T) {
	waitForDisconnect := func(r *http.Request) {
		select {
		case <-r.Context().Done():
		case <-time.After(time.Second): // 即使失败也要让测试服务能在有界时间内退出。
		}
	}
	for _, test := range []struct {
		name, phase, message string
		handle               func(http.ResponseWriter, *http.Request)
	}{
		{"headers_timeout", "request", "service unavailable or timed out; retry with the same operation id",
			func(w http.ResponseWriter, r *http.Request) { waitForDisconnect(r) }},
		{"body_timeout", "read_body", "service response interrupted",
			func(w http.ResponseWriter, r *http.Request) {
				w.Write([]byte(`{"accepted":`))
				w.(http.Flusher).Flush()
				waitForDisconnect(r)
			}},
		{"truncated_body", "read_body", "service response interrupted",
			func(w http.ResponseWriter, r *http.Request) {
				w.Header().Set("Content-Length", "100")
				w.Write([]byte(`{"accepted":true}`))
			}},
		{"invalid_json", "decode", "invalid service response",
			func(w http.ResponseWriter, r *http.Request) { w.Write([]byte(`{bad`)) }},
	} {
		t.Run(test.name, func(t *testing.T) {
			var calls atomic.Int64
			upstream := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				calls.Add(1)
				io.Copy(io.Discard, r.Body)
				test.handle(w, r)
			}))
			defer upstream.Close()
			backendURL, service := upstream.URL, "backend"
			if test.phase == "read_body" {
				backendURL, service = "http://backend.invalid:8081", "scene"
			}
			s, err := New(Config{Token: strings.Repeat("private-internal-key", 2), BackendURL: backendURL})
			if err != nil {
				t.Fatal(err)
			}
			defer s.client.CloseIdleConnections()
			var logged bytes.Buffer
			previous := log.Writer()
			log.SetOutput(&logged)
			defer log.SetOutput(previous)
			ctx, cancel := context.WithTimeout(context.Background(), 150*time.Millisecond)
			defer cancel()
			var response map[string]any
			err = s.call(ctx, upstream.URL, "/v1/cast", map[string]any{"token": "private-player-token", "seq": 8}, &response)
			if err == nil || err.Error() != test.message || response != nil {
				t.Fatalf("传输失败被当作成功，或客户端错误改变: response=%v error=%v", response, err)
			}
			if calls.Load() != 1 {
				t.Fatalf("不确定结果的 POST 被自动重放: %d", calls.Load())
			}
			for _, field := range []string{"service=" + service, "endpoint=", "path=/v1/cast", "phase=" + test.phase, "elapsed_ms=", "error="} {
				if !strings.Contains(logged.String(), field) {
					t.Fatalf("诊断日志缺少 %q: %s", field, &logged)
				}
			}
			if strings.Contains(logged.String(), "private-") || strings.Contains(logged.String(), `"seq"`) {
				t.Fatalf("诊断日志包含凭据或动作正文: %s", &logged)
			}
		})
	}
}

func TestHealthySceneCallDoesNotLogFailure(t *testing.T) {
	scene := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		json.NewEncoder(w).Encode(map[string]bool{"accepted": true})
	}))
	defer scene.Close()
	s, err := New(Config{Token: strings.Repeat("k", 32), BackendURL: "http://backend.invalid:8081"})
	if err != nil {
		t.Fatal(err)
	}
	defer s.client.CloseIdleConnections()
	var logged bytes.Buffer
	previous := log.Writer()
	log.SetOutput(&logged)
	defer log.SetOutput(previous)
	var result map[string]bool
	if err := s.sceneCall(context.Background(), Route{URL: scene.URL}, "/v1/move", map[string]int{"seq": 1}, &result); err != nil || !result["accepted"] {
		t.Fatalf("正常场景请求失败: %v, %v", result, err)
	}
	if logged.Len() != 0 {
		t.Fatalf("成功请求误报失败: %s", &logged)
	}
}
