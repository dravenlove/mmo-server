package main

import (
	"context"
	"flag"
	"log"
	"net/http"
	"os"
	"os/signal"
	"strings"
	"syscall"
	"time"

	"mmo/internal/authsvc"
	"mmo/internal/backend"
)

func env(key, fallback string) string {
	if value := os.Getenv(key); value != "" {
		return value
	}
	return fallback
}

func main() {
	roleFlag := flag.String("role", "game", "服务职责：game 或 auth")
	driverFlag := flag.String("db-driver", env("BACKEND_DB_DRIVER", "sqlite"), "数据库类型：sqlite 或 mysql")
	pathFlag := flag.String("db-path", env("BACKEND_DB", "data/mmo.db"), "SQLite 数据库文件")
	hostFlag := flag.String("mysql-host", os.Getenv("MYSQL_HOST"), "MySQL 主机名或 IP，覆盖 MYSQL_HOST")
	portFlag := flag.String("mysql-port", env("MYSQL_PORT", "3306"), "MySQL 端口，覆盖 MYSQL_PORT")
	flag.Parse()
	if *roleFlag == "auth" {
		authsvc.Run()
		return
	}
	if *roleFlag != "game" {
		log.Fatal("role 只支持game或auth")
	}
	scenes, first, err := backend.ParseScenes(env("SCENES", "scene-1=http://127.0.0.1:9001,scene-2=http://127.0.0.1:9002"))
	if err != nil {
		log.Fatal(err)
	}
	dbDriver := strings.ToLower(strings.TrimSpace(*driverFlag))
	password := os.Getenv("MYSQL_PASSWORD")
	if path := os.Getenv("MYSQL_PASSWORD_FILE"); dbDriver == "mysql" && path != "" {
		data, e := os.ReadFile(path)
		if e != nil {
			log.Fatal("无法读取 MYSQL_PASSWORD_FILE")
		}
		password = strings.TrimRight(string(data), "\r\n")
	}
	service, err := backend.New(backend.Config{
		Token: os.Getenv("INTERNAL_TOKEN"), DBDriver: dbDriver,
		DBPath: *pathFlag, Scenes: scenes, DefaultScene: first,
		AuthURL: os.Getenv("AUTH_URL"),
		MySQL: backend.MySQLConfig{Host: *hostFlag, Port: *portFlag,
			User: os.Getenv("MYSQL_USER"), Password: password, Database: os.Getenv("MYSQL_DATABASE"),
			TLS: env("MYSQL_TLS", "false")},
	})
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
	if dbDriver == "mysql" {
		go func() {
			ticker := time.NewTicker(5 * time.Second)
			defer ticker.Stop()
			for {
				select {
				case <-ctx.Done():
					return
				case <-ticker.C:
					if e := service.Health(); e != nil {
						// 固定会话丢失后停止旧进程；由进程管理器重启并重新获取独占锁。
						log.Printf("MySQL 连接失效，Backend 停止以重新取得数据库独占锁: %v", e)
						stop()
						return
					}
				}
			}
		}()
	}
	go func() {
		<-ctx.Done()
		shutdownCtx, c := context.WithTimeout(context.Background(), 8*time.Second)
		defer c()
		if e := server.Shutdown(shutdownCtx); e != nil {
			log.Printf("shutdown: %v", e)
		}
	}()
	log.Printf("backend listening on %s; database=%s; default scene=%s", server.Addr, dbDriver, first)
	if err = server.ListenAndServe(); err != nil && err != http.ErrServerClosed {
		log.Printf("backend stopped: %v", err)
		os.Exit(1)
	}
}
