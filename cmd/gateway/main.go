package main

import (
	"context"
	"log"
	"mmo/internal/gateway"
	"os"
	"os/signal"
	"syscall"
)

func main() {
	s, err := gateway.FromEnv()
	if err != nil {
		log.Fatal(err)
	}
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	if err = s.Run(ctx); err != nil {
		log.Fatal(err)
	}
}
