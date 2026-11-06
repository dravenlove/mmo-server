package main

import (
	"bufio"
	"crypto/tls"
	"crypto/x509"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"math"
	"net"
	"os"
	"strconv"
	"strings"
	"time"
)

type reply struct {
	ID    string          `json:"id"`
	Op    string          `json:"op"`
	OK    bool            `json:"ok"`
	Data  json.RawMessage `json:"data"`
	Error string          `json:"error"`
}
type credentials struct {
	Token  string `json:"token"`
	RoleID string `json:"role_id"`
}
type client struct {
	conn      net.Conn
	responses chan reply
	errs      chan error
	counter   int64
	seq       int64
}

func (c *client) exchange(req map[string]any) (reply, error) {
	c.counter++
	id := strconv.FormatInt(c.counter, 10)
	req["id"] = id
	c.conn.SetWriteDeadline(time.Now().Add(3 * time.Second))
	if err := json.NewEncoder(c.conn).Encode(req); err != nil {
		return reply{}, err
	}
	timer := time.NewTimer(25 * time.Second)
	defer timer.Stop()
	for {
		select {
		case r := <-c.responses:
			if r.ID == id {
				if !r.OK {
					return r, errors.New(r.Error)
				}
				return r, nil
			}
		case e := <-c.errs:
			return reply{}, e
		case <-timer.C:
			return reply{}, errors.New("请求超时；迁移重试请使用相同 request_id")
		}
	}
}

func main() {
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, "client:", err)
		os.Exit(1)
	}
}
func run() error {
	addr := flag.String("addr", "127.0.0.1:7000", "网关地址")
	name := flag.String("name", "player", "首次注册时的角色名")
	path := flag.String("token-file", "player-token.json", "登录凭据文件，请勿共享")
	scene := flag.String("scene", "", "登录后传送到指定场景")
	script := flag.String("script", "", "执行 JSONL 命令文件后退出")
	secure := flag.Bool("tls", false, "使用 TLS 并验证服务器证书")
	ca := flag.String("ca", "", "附加信任的 CA PEM 文件")
	watch := flag.Bool("watch", false, "打印场景状态推送")
	flag.Parse()
	dialer := &net.Dialer{Timeout: 5 * time.Second}
	var conn net.Conn
	var err error
	if *secure {
		cfg := &tls.Config{MinVersion: tls.VersionTLS12}
		if *ca != "" {
			pem, e := os.ReadFile(*ca)
			if e != nil {
				return e
			}
			pool, e := x509.SystemCertPool()
			if e != nil || pool == nil {
				pool = x509.NewCertPool()
			}
			if !pool.AppendCertsFromPEM(pem) {
				return errors.New("CA 文件没有有效证书")
			}
			cfg.RootCAs = pool
		}
		conn, err = tls.DialWithDialer(dialer, "tcp", *addr, cfg)
	} else {
		if *ca != "" {
			return errors.New("-ca 需要同时指定 -tls")
		}
		conn, err = dialer.Dial("tcp", *addr)
	}
	if err != nil {
		return err
	}
	defer conn.Close()
	c := &client{conn: conn, responses: make(chan reply, 32), errs: make(chan error, 1)}
	go func() {
		s := bufio.NewScanner(conn)
		s.Buffer(make([]byte, 4096), 1<<20)
		for s.Scan() {
			var r reply
			if e := json.Unmarshal(s.Bytes(), &r); e != nil {
				c.errs <- e
				return
			}
			if r.ID != "" {
				select {
				case c.responses <- r:
				default:
					c.errs <- errors.New("响应队列溢出")
					return
				}
			} else if *watch || !r.OK {
				fmt.Println(string(s.Bytes()))
			}
		}
		e := s.Err()
		if e == nil {
			e = io.EOF
		}
		c.errs <- e
	}()
	var cred credentials
	data, err := os.ReadFile(*path)
	if err == nil {
		if err = json.Unmarshal(data, &cred); err != nil {
			return fmt.Errorf("凭据文件无效: %w", err)
		}
		if cred.Token == "" {
			return errors.New("凭据文件缺少 token")
		}
	} else if os.IsNotExist(err) {
		// 注册前预留私有文件，避免因为路径不可写而丢失新账号的唯一凭据。
		f, e := os.OpenFile(*path, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0600)
		if e != nil {
			return fmt.Errorf("尚未注册：无法创建凭据文件: %w", e)
		}
		registered := false
		defer func() {
			f.Close()
			if !registered {
				os.Remove(*path)
			}
		}()
		r, e := c.exchange(map[string]any{"op": "register", "name": *name})
		if e != nil {
			return e
		}
		if e = json.Unmarshal(r.Data, &cred); e != nil {
			return e
		}
		registered = true
		e = json.NewEncoder(f).Encode(cred)
		if e == nil {
			e = f.Sync()
		}
		ce := f.Close()
		if e != nil {
			return e
		}
		if ce != nil {
			return ce
		}
		fmt.Printf("已注册角色，登录凭据保存到 %s\n", *path)
	} else {
		return err
	}
	r, err := c.exchange(map[string]any{"op": "login", "token": cred.Token})
	if err != nil {
		return err
	}
	fmt.Printf("登录成功: %s\n", r.Data)
	if *scene != "" {
		requestID := fmt.Sprintf("client-%d", time.Now().UnixNano())
		fmt.Printf("transfer request_id=%s（失败重试请复用此值）\n", requestID)
		r, err = c.exchange(map[string]any{"op": "transfer", "target_scene_id": *scene, "request_id": requestID})
		if err != nil {
			return err
		}
		fmt.Println(string(r.Data))
	}
	execute := func(req map[string]any) error {
		if req["op"] == "sleep" {
			ms, ok := req["ms"].(float64)
			if !ok || ms < 0 || ms > 10000 {
				return errors.New("sleep.ms 需要为 0..10000")
			}
			time.Sleep(time.Duration(ms) * time.Millisecond)
			return nil
		}
		if req["op"] == "move" || req["op"] == "cast" || req["op"] == "revive" {
			if explicit, exists := req["seq"]; exists {
				value, ok := explicit.(float64)
				if !ok || value < 1 || value > 9007199254740991 || math.Trunc(value) != value {
					return errors.New("seq 需要为有效的正整数")
				}
				if int64(value) > c.seq {
					c.seq = int64(value)
				}
			} else {
				c.seq++
				req["seq"] = c.seq
			}
		}
		if req["op"] == "transfer" {
			if _, exists := req["request_id"]; !exists {
				req["request_id"] = fmt.Sprintf("client-%d", time.Now().UnixNano())
			}
			fmt.Printf("transfer request_id=%v（失败重试请复用此值）\n", req["request_id"])
		}
		r, e := c.exchange(req)
		if e != nil {
			return e
		}
		fmt.Printf("%s: %s\n", r.Op, r.Data)
		return nil
	}
	if *script != "" {
		f, e := os.Open(*script)
		if e != nil {
			return e
		}
		defer f.Close()
		s := bufio.NewScanner(f)
		line := 0
		for s.Scan() {
			line++
			if strings.TrimSpace(s.Text()) == "" {
				continue
			}
			var req map[string]any
			if e = json.Unmarshal(s.Bytes(), &req); e != nil {
				return fmt.Errorf("脚本第 %d 行: %w", line, e)
			}
			if e = execute(req); e != nil {
				return fmt.Errorf("脚本第 %d 行: %w", line, e)
			}
		}
		return s.Err()
	}
	fmt.Println("命令: view | move dx dy | cast 技能ID [怪物ID] | revive | transfer scene-1 [request_id] | claim | ping | quit。技能: strike / whirlwind / charge / mend。")
	lines := make(chan string)
	go func() {
		s := bufio.NewScanner(os.Stdin)
		for s.Scan() {
			lines <- s.Text()
		}
		close(lines)
	}()
	ping := time.NewTicker(20 * time.Second)
	defer ping.Stop()
	for {
		select {
		case e := <-c.errs:
			return e
		case <-ping.C:
			if _, e := c.exchange(map[string]any{"op": "ping"}); e != nil {
				return e
			}
		case line, ok := <-lines:
			if !ok {
				return nil
			}
			fields := strings.Fields(line)
			if len(fields) == 0 {
				continue
			}
			if fields[0] == "quit" {
				return nil
			}
			req := map[string]any{"op": fields[0]}
			switch fields[0] {
			case "view", "claim", "ping", "revive":
				if len(fields) != 1 {
					fmt.Println("参数过多")
					continue
				}
			case "cast":
				if len(fields) != 2 && len(fields) != 3 {
					fmt.Println("用法: cast strike npc-1 | cast whirlwind | cast charge npc-1 | cast mend")
					continue
				}
				req["skill_id"] = fields[1]
				if len(fields) == 3 {
					req["target_id"] = fields[2]
				}
			case "move":
				if len(fields) != 3 {
					fmt.Println("用法: move dx dy，分量 -1..1")
					continue
				}
				x, e1 := strconv.ParseFloat(fields[1], 64)
				y, e2 := strconv.ParseFloat(fields[2], 64)
				if e1 != nil || e2 != nil {
					fmt.Println("无效方向")
					continue
				}
				req["dx"], req["dy"] = x, y
			case "transfer":
				if len(fields) != 2 && len(fields) != 3 {
					fmt.Println("用法: transfer scene-2 [request_id]")
					continue
				}
				req["target_scene_id"] = fields[1]
				if len(fields) == 3 {
					req["request_id"] = fields[2]
				}
			default:
				fmt.Println("未知命令")
				continue
			}
			if e := execute(req); e != nil {
				fmt.Fprintln(os.Stderr, "请求失败:", e)
			}
		}
	}
}
