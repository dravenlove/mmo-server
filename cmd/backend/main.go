package main

import (
	"context"
	"log"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"

	"mmo/internal/backend"
)

func env(key, fallback string) string {
	if value := os.Getenv(key); value != "" {
		return value
	}
	return fallback
}

func main() {
	scenes, first, err := backend.ParseScenes(env("SCENES", "scene-1=http://127.0.0.1:9001,scene-2=http://127.0.0.1:9002"))
	if err != nil {
		log.Fatal(err)
	}
	service, err := backend.New(backend.Config{Token: os.Getenv("INTERNAL_TOKEN"), DBPath: env("BACKEND_DB", "data/mmo.db"), Scenes: scenes, DefaultScene: first})
	if err != nil {
		log.Fatal(err)
	}
	defer service.Close()
	recoverCtx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	if err = service.Recover(recoverCtx); err != nil {
		log.Printf("部分迁移等待场景恢复，相关角色会在后续请求中重试: %v", err)
	}
	cancel()
	server := &http.Server{Addr: env("BACKEND_LISTEN", "127.0.0.1:8081"), Handler: service.Handler(), ReadHeaderTimeout: 3 * time.Second, ReadTimeout: 5 * time.Second, WriteTimeout: 20 * time.Second, IdleTimeout: 30 * time.Second, MaxHeaderBytes: 16 * 1024}
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	go func() {
		<-ctx.Done()
		shutdownCtx, c := context.WithTimeout(context.Background(), 8*time.Second)
		defer c()
		if e := server.Shutdown(shutdownCtx); e != nil {
			log.Printf("shutdown: %v", e)
		}
	}()
	log.Printf("backend listening on %s; default scene=%s", server.Addr, first)
	if err = server.ListenAndServe(); err != nil && err != http.ErrServerClosed {
		log.Printf("backend stopped: %v", err)
		os.Exit(1)
	}
}
