package main

import (
	"context"
	"flag"
	"log"
	"mmo/internal/wsbridge"
	"net/http"
	"os"
	"os/signal"
	"strings"
	"syscall"
	"time"
)

func env(name, fallback string) string {
	if value := os.Getenv(name); value != "" {
		return value
	}
	return fallback
}

func main() {
	listen := flag.String("listen", env("WS_LISTEN", "127.0.0.1:7001"), "WebSocket HTTP listener")
	upstream := flag.String("upstream", env("WS_UPSTREAM", "127.0.0.1:7000"), "fixed TCP gateway")
	origins := flag.String("origins", env("WS_ALLOWED_ORIGINS", "http://127.0.0.1:8088,http://localhost:8088"), "comma-separated browser origins")
	upstreamTLS := flag.Bool("upstream-tls", env("WS_UPSTREAM_TLS", "false") == "true", "verify TLS to gateway")
	flag.Parse()
	bridge, err := wsbridge.New(wsbridge.Config{Upstream: *upstream, UpstreamTLS: *upstreamTLS, AllowedOrigins: strings.Split(*origins, ",")})
	if err != nil {
		log.Fatal(err)
	}
	server := &http.Server{Addr: *listen, Handler: bridge, ReadHeaderTimeout: 5 * time.Second, IdleTimeout: 60 * time.Second, MaxHeaderBytes: 8192}
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	go func() {
		<-ctx.Done()
		bridge.Close()
		shutdown, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		server.Shutdown(shutdown)
	}()
	log.Printf("websocket bridge listening on %s; upstream %s", *listen, *upstream)
	if err = server.ListenAndServe(); err != nil && err != http.ErrServerClosed {
		log.Fatal(err)
	}
}
