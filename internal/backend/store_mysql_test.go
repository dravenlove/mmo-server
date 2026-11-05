package backend

import (
	"context"
	"crypto/rand"
	"database/sql"
	"fmt"
	"net/http"
	"os"
	"strings"
	"testing"
	"time"

	"github.com/go-sql-driver/mysql"
)

// 仅显式设置 MMO_TEST_MYSQL_HOST 时启用；每个用例创建并清理自己的随机测试库。
// 测试账号需有创建/删除测试库权限，不读取 MYSQL_HOST 等实际部署配置。
func testMySQLConfig(t *testing.T) (MySQLConfig, bool) {
	t.Helper()
	input := MySQLConfig{Host: os.Getenv("MMO_TEST_MYSQL_HOST"), Port: os.Getenv("MMO_TEST_MYSQL_PORT"),
		User: os.Getenv("MMO_TEST_MYSQL_USER"), Password: os.Getenv("MMO_TEST_MYSQL_PASSWORD"), Database: "mysql"}
	if input.Host == "" {
		return MySQLConfig{}, false
	}
	cfg, err := mysqlConfig(input)
	if err != nil {
		t.Fatal(err)
	}
	connector, err := mysql.NewConnector(cfg)
	if err != nil {
		t.Fatal(err)
	}
	admin := sql.OpenDB(connector)
	var random [8]byte
	if _, err = rand.Read(random[:]); err != nil {
		admin.Close()
		t.Fatal(err)
	}
	name := fmt.Sprintf("mmo_test_%x", random)
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	if _, err = admin.ExecContext(ctx, "CREATE DATABASE `"+name+"` CHARACTER SET utf8mb4"); err != nil {
		admin.Close()
		t.Fatal(err)
	}
	t.Cleanup(func() {
		ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		defer cancel()
		if _, err := admin.ExecContext(ctx, "DROP DATABASE `"+name+"`"); err != nil {
			t.Errorf("清理本次创建的测试库失败: %v", err)
		}
		admin.Close()
	})
	input.Database = name
	return input, true
}

func TestMySQLConfigValidation(t *testing.T) {
	valid := MySQLConfig{Host: "2001:db8::1", User: "mmo", Password: "a:@/?&=+ $'", Database: "mmo"}
	cfg, err := mysqlConfig(valid)
	if err != nil {
		t.Fatal(err)
	}
	if cfg.Addr != "[2001:db8::1]:3306" || cfg.Passwd != valid.Password || !cfg.ClientFoundRows {
		t.Fatal("地址、密码或匹配行数语义错误")
	}
	for _, input := range []MySQLConfig{
		{User: "mmo", Database: "mmo"}, {Host: "localhost", Database: "mmo"},
		{Host: "localhost", User: "mmo"},
		{Host: "localhost", User: "mmo", Database: "mmo", Port: "3306/other"},
		{Host: "localhost", User: "mmo", Database: "mmo", Port: "65536"},
		{Host: "localhost", User: "mmo", Database: "mmo", TLS: "skip-verify"},
		{Host: "10.20.0.30:3306", User: "mmo", Database: "mmo"},
		{Host: "mysql://localhost", User: "mmo", Database: "mmo"},
	} {
		if _, err := mysqlConfig(input); err == nil {
			t.Fatal("接受了无效 MySQL 配置")
		}
	}
}

func TestUnchangedCheckpointAndBinaryNames(t *testing.T) {
	f := newFixture(t)
	_, route := registerLogin(t, f, "Alice")
	// 坐标未改变时 MySQL 也应视为归属匹配，不返回错误的 409。
	wantPost(t, f.s, "checkpoint", roleResult{route.Role}, 200, nil)
	wantPost(t, f.s, "checkpoint", roleResult{route.Role}, 200, nil)
	for _, name := range []string{"alice", "Alíce", strings.Repeat("🐉", 24)} {
		registerLogin(t, f, name)
	}
	wantPost(t, f.s, "register", map[string]string{"name": strings.Repeat("🐉", 25)}, 400, nil)
	wantPost(t, f.s, "register", map[string]string{"name": "alice"}, 409, nil)
}

func TestPendingMigrationDatabaseConstraint(t *testing.T) {
	f := newFixture(t)
	_, route := registerLogin(t, f, "Alice")
	request := transferRequest{Ref: route.Role.Ref(), TargetSceneID: "scene-2", RequestID: "first"}
	m, err := f.s.beginMigration(context.Background(), route.Role, request)
	if err != nil {
		t.Fatal(err)
	}
	request.RequestID = "second"
	if _, err = f.s.beginMigration(context.Background(), route.Role, request); err == nil {
		t.Fatal("同一角色允许了两条未完成迁移")
	}
	if err = f.s.setStatus(context.Background(), &m, "ABORTED"); err != nil {
		t.Fatal(err)
	}
	if _, err = f.s.beginMigration(context.Background(), route.Role, request); err != nil {
		t.Fatalf("终态迁移仍占用唯一约束: %v", err)
	}
	_, err = f.s.db.ExecContext(context.Background(), "INSERT INTO claims(operation_id,role_id) VALUES(?,?)", "bad-claim", "missing-role")
	if err == nil || uniqueViolation(err) {
		t.Fatal("外键错误不能被当作重复奖励忽略")
	}
}

func TestMySQLLostSessionCannotResumeWriting(t *testing.T) {
	if os.Getenv("MMO_TEST_MYSQL_HOST") == "" {
		t.Skip("需要隔离的 MySQL 测试实例")
	}
	f := newFixture(t)
	_, _ = registerLogin(t, f, "Alice")
	var id int64
	if err := f.s.db.QueryRow("SELECT CONNECTION_ID()").Scan(&id); err != nil {
		t.Fatal(err)
	}
	cfg, err := mysqlConfig(f.cfg.MySQL)
	if err != nil {
		t.Fatal(err)
	}
	connector, err := mysql.NewConnector(cfg)
	if err != nil {
		t.Fatal(err)
	}
	admin := sql.OpenDB(connector)
	defer admin.Close()
	if _, err = admin.Exec(fmt.Sprintf("KILL CONNECTION %d", id)); err != nil {
		t.Fatal(err)
	}
	if err = f.s.Health(); err == nil {
		t.Fatal("已断开的独占会话仍被视为健康")
	}
	other, err := New(f.cfg)
	if err != nil {
		t.Fatalf("新 Backend 无法取得已释放的锁: %v", err)
	}
	defer other.Close()
	wantPost(t, other, "register", map[string]string{"name": "NewWriter"}, 200, nil)
	code, _ := post(f.s, "register", map[string]string{"name": "OldWriter"})
	if code == http.StatusOK {
		t.Fatal("旧 Backend 断线后自行重连写入")
	}
	var count int
	if err = admin.QueryRow("SELECT COUNT(*) FROM roles WHERE name=?", "OldWriter").Scan(&count); err != nil || count != 0 {
		t.Fatal("旧写入者未被隔离")
	}
}
