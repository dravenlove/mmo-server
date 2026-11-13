package main

import (
	"bytes"
	"context"
	"crypto/rand"
	"crypto/sha256"
	"database/sql"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/http/httptest"
	"os"
	"os/exec"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/go-sql-driver/mysql"
	"golang.org/x/crypto/bcrypt"
	"mmo/internal/authsvc"
)

const fixtureKey = "native-account-cross-language-internal-test-key"

func syntheticPost(t *testing.T, base, operation string, body any) (int, map[string]any) {
	t.Helper()
	payload, _ := json.Marshal(body)
	request, _ := http.NewRequest(http.MethodPost, base+"/v1/"+operation, bytes.NewReader(payload))
	request.Header.Set("Content-Type", "application/json")
	request.Header.Set("X-Internal-Token", fixtureKey)
	response, err := (&http.Client{Timeout: 5 * time.Second}).Do(request)
	if err != nil {
		t.Fatal("isolated auth request failed")
	}
	defer response.Body.Close()
	var result map[string]any
	if json.NewDecoder(response.Body).Decode(&result) != nil {
		t.Fatal("isolated auth returned invalid JSON")
	}
	return response.StatusCode, result
}

func mysqlFixtureConfig(user, password, host, port, database string) *mysql.Config {
	c := mysql.NewConfig()
	c.Net, c.Addr, c.User, c.Passwd, c.DBName = "tcp", net.JoinHostPort(host, port), user, password, database
	c.Timeout, c.ReadTimeout, c.WriteTimeout = 3*time.Second, 3*time.Second, 3*time.Second
	return c
}

func isolatedConnection(t *testing.T, config *mysql.Config) *sql.DB {
	t.Helper()
	connector, err := mysql.NewConnector(config)
	if err != nil {
		t.Fatal("invalid isolated MySQL config")
	}
	db := sql.OpenDB(connector)
	if err = db.Ping(); err != nil {
		db.Close()
		t.Fatal("isolated MySQL unavailable")
	}
	return db
}

func startNativeAccount(t *testing.T, binary string, config authsvc.Config) (*exec.Cmd, string, func()) {
	t.Helper()
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	address := listener.Addr().String()
	listener.Close()
	var clean []string
	for _, item := range os.Environ() {
		key := strings.SplitN(item, "=", 2)[0]
		if !strings.HasPrefix(key, "MYSQL_") && !strings.HasPrefix(key, "AUTH_") && key != "INTERNAL_TOKEN" {
			clean = append(clean, item)
		}
	}
	command := exec.Command(binary)
	command.Env = append(clean, "INTERNAL_TOKEN="+fixtureKey, "AUTH_DB_DRIVER=mysql", "AUTH_LISTEN="+address,
		"MYSQL_HOST="+config.Host, "MYSQL_PORT="+config.Port, "MYSQL_USER="+config.User,
		"MYSQL_PASSWORD="+config.Password, "MYSQL_DATABASE="+config.Database, "MYSQL_TLS=false")
	command.Stdout, command.Stderr = io.Discard, io.Discard
	if err = command.Start(); err != nil {
		t.Fatal("cannot start isolated native AccountService")
	}
	finished := make(chan error, 1)
	go func() { finished <- command.Wait() }()
	stopped := false
	stop := func() {
		if stopped {
			return
		}
		stopped = true
		_ = command.Process.Signal(os.Interrupt)
		select {
		case <-finished:
		case <-time.After(5 * time.Second):
			_ = command.Process.Kill()
			<-finished
		}
	}
	for n := 0; n < 100; n++ {
		select {
		case <-finished:
			stopped = true
			t.Fatal("isolated native AccountService exited during startup")
		default:
		}
		response, err := (&http.Client{Timeout: 100 * time.Millisecond}).Get("http://" + address + "/healthz")
		if err == nil {
			response.Body.Close()
			if response.StatusCode == 200 {
				return command, "http://" + address, stop
			}
		}
		time.Sleep(50 * time.Millisecond)
	}
	stop()
	t.Fatal("isolated native AccountService startup timed out")
	return nil, "", nil
}

// 只读取明确的 MMO_TEST_* 配置，每次创建并销毁自己的随机库和限权用户。
func TestMySQLNativeAccountCompatibility(t *testing.T) {
	binary := os.Getenv("NATIVE_ACCOUNT_BINARY")
	host, port := os.Getenv("MMO_TEST_MYSQL_HOST"), os.Getenv("MMO_TEST_MYSQL_PORT")
	if binary == "" || host == "" {
		t.Skip("requires isolated MMO_TEST_MYSQL_* and NATIVE_ACCOUNT_BINARY")
	}
	if host != "127.0.0.1" && host != "localhost" {
		t.Fatal("test fixture only accepts a loopback MySQL endpoint")
	}
	if port == "" {
		t.Fatal("explicit isolated MySQL test port required")
	}
	if value, err := strconv.Atoi(port); err != nil || value < 1 || value > 65535 {
		t.Fatal("invalid isolated MySQL port")
	}
	admin := isolatedConnection(t, mysqlFixtureConfig(os.Getenv("MMO_TEST_MYSQL_USER"), os.Getenv("MMO_TEST_MYSQL_PASSWORD"), host, port, ""))
	defer admin.Close()
	random := make([]byte, 6)
	if _, err := rand.Read(random); err != nil {
		t.Fatal("random source unavailable")
	}
	suffix := hex.EncodeToString(random)
	database, user := "mmo_native_test_"+suffix, "mmo_test_auth_"+suffix
	password := "synthetic@:/?&=+$" + suffix
	// 下面只拼接本测试生成的固定前缀与十六进制随机串，不接受外部账号或 SQL。
	account := "'" + user + "'@'%'"
	if _, err := admin.Exec("CREATE DATABASE `" + database + "`"); err != nil {
		t.Fatal("cannot create isolated test database")
	}
	defer admin.Exec("DROP DATABASE `" + database + "`")
	if dbBinary := os.Getenv("NATIVE_DB_TEST_BINARY"); dbBinary != "" {
		command := exec.Command(dbBinary, "--mysql")
		command.Env = append(os.Environ(), "MMO_TEST_MYSQL_DATABASE="+database)
		// DB 测试只使用本次随机库，日志不包含密码/票据。
		output, err := command.CombinedOutput()
		if err != nil {
			t.Fatalf("shared native Store MySQL invariants failed: %s", string(output))
		}
		t.Log(strings.TrimSpace(string(output)))
	}
	if gameBinary := os.Getenv("NATIVE_GAME_TEST_BINARY"); gameBinary != "" {
		// 业务回归创建完整 roles schema，与下面的最小身份导入夹具分库隔离。
		gameDatabase := database + "_game"
		if _, err := admin.Exec("CREATE DATABASE `" + gameDatabase + "`"); err != nil {
			t.Fatal("cannot create isolated game compatibility database")
		}
		defer admin.Exec("DROP DATABASE `" + gameDatabase + "`")
		gameUser := "mmo_test_game_" + suffix
		gameAccount := "'" + gameUser + "'@'%'"
		if _, err := admin.Exec("CREATE USER " + gameAccount + " IDENTIFIED BY '" + password + "'"); err != nil {
			t.Fatal("cannot create isolated limited game user")
		}
		defer admin.Exec("DROP USER " + gameAccount)
		if _, err := admin.Exec("GRANT SELECT,INSERT,UPDATE,CREATE,INDEX,REFERENCES ON `" + gameDatabase + "`.* TO " + gameAccount); err != nil {
			t.Fatal("cannot grant isolated game permissions")
		}
		command := exec.Command(gameBinary, "--mysql")
		command.Env = append(os.Environ(), "MMO_TEST_MYSQL_DATABASE="+gameDatabase, "MMO_TEST_MYSQL_USER="+gameUser, "MMO_TEST_MYSQL_PASSWORD="+password)
		output, err := command.CombinedOutput()
		if err != nil {
			t.Fatalf("native GameService MySQL invariants failed: %s", string(output))
		}
		t.Log(strings.TrimSpace(string(output)))
		limited := isolatedConnection(t, mysqlFixtureConfig(gameUser, password, host, port, gameDatabase))
		defer limited.Close()
		if _, err := limited.Exec("DELETE FROM roles WHERE 1=0"); err == nil {
			t.Fatal("game fixture unexpectedly has DELETE permission")
		}
	}
	if _, err := admin.Exec("CREATE USER " + account + " IDENTIFIED BY '" + password + "'"); err != nil {
		t.Fatal("cannot create isolated limited user")
	}
	defer admin.Exec("DROP USER " + account)
	if _, err := admin.Exec("GRANT SELECT,INSERT,UPDATE,CREATE,INDEX ON `" + database + "`.* TO " + account); err != nil {
		t.Fatal("cannot grant isolated minimal privileges")
	}
	app := isolatedConnection(t, mysqlFixtureConfig(user, password, host, port, database))
	defer app.Close()
	if _, err := app.Exec("CREATE TABLE roles(role_id VARBINARY(128),name VARBINARY(96),token_hash BINARY(64),coins BIGINT) ENGINE=InnoDB"); err != nil {
		t.Fatal("cannot create synthetic legacy fixture")
	}
	legacy := strings.Repeat("a", 64)
	digest := sha256.Sum256([]byte(legacy))
	if _, err := app.Exec("INSERT INTO roles VALUES(?,?,?,77)", strings.Repeat("1", 32), "原角色", hex.EncodeToString(digest[:])); err != nil {
		t.Fatal("cannot initialize synthetic legacy fixture")
	}
	cfg := authsvc.Config{Token: fixtureKey, Driver: "mysql", Host: host, Port: port, User: user, Password: password, Database: database, TLS: "false"}
	goAuth, err := authsvc.New(cfg)
	if err != nil {
		t.Fatal("Go reference Auth failed to start")
	}
	defer goAuth.Close()
	goHTTP := httptest.NewServer(goAuth.Handler())
	defer goHTTP.Close()
	code, old := syntheticPost(t, goHTTP.URL, "account_register", map[string]string{"name": "GoHashRole", "password": "联调-password-123!"})
	if code != 200 {
		t.Fatal("Go synthetic registration failed")
	}
	goHTTP.Close()
	// Go 固定会话持锁时 C++ 必须拒绝启动，不能另取锁名并写库。
	ctx, cancel := context.WithTimeout(context.Background(), 8*time.Second)
	defer cancel()
	blocked := exec.CommandContext(ctx, binary)
	blocked.Env = append(os.Environ(), "INTERNAL_TOKEN="+fixtureKey, "AUTH_DB_DRIVER=mysql", "AUTH_LISTEN=127.0.0.1:8082",
		"MYSQL_HOST="+host, "MYSQL_PORT="+port, "MYSQL_USER="+user, "MYSQL_PASSWORD="+password,
		"MYSQL_PASSWORD_FILE=", "MYSQL_DATABASE="+database, "MYSQL_TLS=false")
	blocked.Stdout, blocked.Stderr = io.Discard, io.Discard
	if err = blocked.Run(); err == nil || ctx.Err() != nil {
		goAuth.Close()
		t.Fatal("native AccountService bypassed Go exclusive lock or failed to exit")
	}
	if err = goAuth.Close(); err != nil {
		t.Fatal("Go reference Auth failed to release lock")
	}
	_, base, stop := startNativeAccount(t, binary, cfg)
	defer stop()
	if code, _ = syntheticPost(t, base, "authenticate", map[string]any{"token": old["token"]}); code != 200 {
		t.Fatal("C++ rejected Go-issued ticket")
	}
	if code, _ = syntheticPost(t, base, "account_login", map[string]string{"name": "GoHashRole", "password": "联调-password-123!"}); code != 200 {
		t.Fatal("C++ rejected existing Go UTF8 bcrypt hash")
	}
	if other, err := authsvc.New(cfg); err == nil {
		other.Close()
		t.Fatal("Go Auth bypassed C++ exclusive lock")
	}
	code, created := syntheticPost(t, base, "account_register", map[string]string{"name": "CppHashRole", "password": "联调-password-123!"})
	if code != 200 {
		t.Fatal("C++ minimal-privilege registration failed")
	}
	var hash, stable string
	if err = app.QueryRow("SELECT p.password_hash,a.token_hash FROM auth_passwords p JOIN auth_accounts a ON a.role_id=p.role_id WHERE a.role_id=?", created["role_id"]).Scan(&hash, &stable); err != nil || bcrypt.CompareHashAndPassword([]byte(hash), []byte("联调-password-123!")) != nil {
		t.Fatal("Go rejected C++ generated bcrypt hash")
	}
	temporary := sha256.Sum256([]byte(created["token"].(string)))
	if stable == hex.EncodeToString(temporary[:]) {
		t.Fatal("temporary ticket overwrote permanent identity")
	}
	first := created["token"]
	var latest any
	for n := 0; n < 5; n++ {
		code, ticket := syntheticPost(t, base, "account_login", map[string]string{"name": "CppHashRole", "password": "联调-password-123!"})
		if code != 200 {
			t.Fatal("minimal-privilege ticket reuse failed")
		}
		latest = ticket["token"]
	}
	if code, _ = syntheticPost(t, base, "authenticate", map[string]any{"token": first}); code != 401 {
		t.Fatal("oldest ticket was not invalidated")
	}
	if code, _ = syntheticPost(t, base, "authenticate", map[string]any{"token": latest}); code != 200 {
		t.Fatal("latest ticket was rejected")
	}
	var count int
	if err = app.QueryRow("SELECT COUNT(*) FROM auth_login_tokens WHERE role_id=?", created["role_id"]).Scan(&count); err != nil || count != 5 {
		t.Fatal("ticket table was not bounded")
	}
	if _, err = app.Exec("DELETE FROM auth_login_tokens WHERE 1=0"); err == nil {
		t.Fatal("fixture unexpectedly grants DELETE")
	}
	if code, _ = syntheticPost(t, base, "account_bind", map[string]string{"token": legacy, "password": "联调-password-123!"}); code != 200 {
		t.Fatal("legacy bind failed")
	}
	if code, _ = syntheticPost(t, base, "authenticate", map[string]string{"token": legacy}); code != 200 {
		t.Fatal("binding invalidated permanent token")
	}
	var coins int
	if err = app.QueryRow("SELECT coins FROM roles").Scan(&coins); err != nil || coins != 77 {
		t.Fatal("Auth modified game role data")
	}
	// 强制丢失命名锁的持有会话，旧进程必须 fail closed，不能自动重连继续写。
	databaseDigest := sha256.Sum256([]byte(strings.ToLower(database)))
	lock := "mmo-auth:" + hex.EncodeToString(databaseDigest[:24])
	var owner uint64
	if err = admin.QueryRow("SELECT IS_USED_LOCK(?)", lock).Scan(&owner); err != nil || owner == 0 {
		t.Fatal("native writer does not hold expected Go-compatible lock")
	}
	if _, err = admin.Exec(fmt.Sprintf("KILL CONNECTION %d", owner)); err != nil {
		t.Fatal("cannot test isolated connection loss")
	}
	response, err := (&http.Client{Timeout: 2 * time.Second}).Get(base + "/healthz")
	if err == nil {
		response.Body.Close()
		if response.StatusCode == 200 {
			t.Fatal("native auth silently reconnected after ownership loss")
		}
	}
	stop()
	goAuth, err = authsvc.New(cfg)
	if err != nil {
		t.Fatal("Go rollback failed on C++ schema and data")
	}
	defer goAuth.Close()
	goHTTP = httptest.NewServer(goAuth.Handler())
	defer goHTTP.Close()
	if code, _ = syntheticPost(t, goHTTP.URL, "authenticate", map[string]any{"token": latest}); code != 200 {
		t.Fatal("Go rollback rejected C++ issued ticket")
	}
	if code, _ = syntheticPost(t, goHTTP.URL, "account_login", map[string]string{"name": "CppHashRole", "password": "联调-password-123!"}); code != 200 {
		t.Fatal("Go rollback rejected C++ account")
	}
	t.Log("Go/C++ bcrypt and tickets, same-lock exclusion, minimal privileges, no DELETE, fixed-connection loss and rollback passed")
}
