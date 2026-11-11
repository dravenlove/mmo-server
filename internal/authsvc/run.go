package authsvc

import (
	"context"
	"log"

	"net/http"
	"os"
	"os/signal"
	"strings"
	"syscall"
	"time"
)

func env(key, fallback string) string {
	if value := os.Getenv(key); value != "" {
		return value
	}
	return fallback
}
func Run() {
	password := os.Getenv("MYSQL_PASSWORD")
	if file := os.Getenv("MYSQL_PASSWORD_FILE"); file != "" {
		data, err := os.ReadFile(file)
		if err != nil {
			log.Fatal("无法读取MYSQL_PASSWORD_FILE")
		}
		password = strings.TrimRight(string(data), "\r\n")
	}
	service, err := New(Config{Token: os.Getenv("INTERNAL_TOKEN"), Driver: env("AUTH_DB_DRIVER", "mysql"), Path: env("AUTH_DB", "data/auth.db"), Host: os.Getenv("MYSQL_HOST"), Port: env("MYSQL_PORT", "3306"), User: os.Getenv("MYSQL_USER"), Password: password, Database: os.Getenv("MYSQL_DATABASE"), TLS: env("MYSQL_TLS", "false")})
	if err != nil {
		log.Fatal(err)
	}
	defer service.Close()
	server := &http.Server{Addr: env("AUTH_LISTEN", "127.0.0.1:8082"), Handler: service.Handler(), ReadHeaderTimeout: 3 * time.Second, ReadTimeout: 5 * time.Second, WriteTimeout: 10 * time.Second, IdleTimeout: 30 * time.Second, MaxHeaderBytes: 16 * 1024}
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	go func() {
		ticker := time.NewTicker(5 * time.Second)
		defer ticker.Stop()
		for {
			select {
			case <-ctx.Done():
				return
			case <-ticker.C:
				if service.Health() != nil {
					log.Print("鉴权数据库连接失效，停止进程以重新取得独占锁")
					stop()
					return
				}
			}
		}
	}()
	go func() {
		<-ctx.Done()
		c, cancel := context.WithTimeout(context.Background(), 8*time.Second)
		defer cancel()
		_ = server.Shutdown(c)
	}()
	log.Printf("auth listening=%s; account authentication only", server.Addr)
	if err = server.ListenAndServe(); err != nil && err != http.ErrServerClosed {
		log.Print(err)
		os.Exit(1)
	}
}
