package backend

import (
	"context"
	"database/sql"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"

	_ "modernc.org/sqlite"
)

const columns = "role_id,name,scene_id,owner_epoch,session_epoch,x,y,coins"

// 所有操作固定使用同一连接。MySQL 的独占锁与业务写入共用此会话，
// 连接丢失后不能由连接池偷偷换连接继续写入。
type store struct {
	*sql.Conn
	pool     *sql.DB
	fileLock *os.File
}

func (s *store) QueryRow(query string, args ...any) *sql.Row {
	return s.QueryRowContext(context.Background(), query, args...)
}

func (s *store) Ping() error { return s.PingContext(context.Background()) }

func (s *store) Close() error {
	err := s.Conn.Close()
	if errors.Is(err, sql.ErrConnDone) {
		err = nil
	}
	err = errors.Join(err, s.pool.Close())
	if s.fileLock != nil {
		err = errors.Join(err, s.fileLock.Close())
	}
	return err
}

func openStore(config Config) (*store, error) {
	if config.DBDriver == "mysql" {
		return openMySQLStore(config.MySQL)
	}
	return openSQLiteStore(config.DBPath)
}

type scanner interface{ Scan(...any) error }

func scanRole(row scanner) (Role, error) {
	var r Role
	err := row.Scan(&r.RoleID, &r.Name, &r.SceneID, &r.OwnerEpoch, &r.SessionEpoch, &r.X, &r.Y, &r.Coins)
	return r, err
}

func openSQLiteStore(path string) (*store, error) {
	abs, err := filepath.Abs(path)
	if err != nil {
		return nil, err
	}
	if err = os.MkdirAll(filepath.Dir(abs), 0700); err != nil {
		return nil, err
	}
	// 解析目录符号链接，让同一数据库的普通别名也竞争同一把文件锁。
	dir, err := filepath.EvalSymlinks(filepath.Dir(abs))
	if err != nil {
		return nil, err
	}
	abs = filepath.Join(dir, filepath.Base(abs))
	if actual, e := filepath.EvalSymlinks(abs); e == nil {
		abs = actual
	}
	lock, err := os.OpenFile(abs+".lock", os.O_CREATE|os.O_RDWR, 0600)
	if err != nil {
		return nil, err
	}
	if err = lockFile(lock); err != nil {
		lock.Close()
		return nil, fmt.Errorf("数据库已被其他 Backend 占用或无法加锁: %w", err)
	}
	db, err := sql.Open("sqlite", abs)
	if err != nil {
		lock.Close()
		return nil, err
	}
	db.SetMaxOpenConns(1)
	cleanup := func(e error) (*store, error) { db.Close(); lock.Close(); return nil, e }
	for _, statement := range []string{
		"PRAGMA journal_mode=WAL", "PRAGMA synchronous=FULL", "PRAGMA busy_timeout=5000", "PRAGMA foreign_keys=ON",
		`CREATE TABLE IF NOT EXISTS roles (
		 role_id TEXT PRIMARY KEY, name TEXT NOT NULL UNIQUE, token_hash TEXT NOT NULL UNIQUE,
		 scene_id TEXT NOT NULL, owner_epoch INTEGER NOT NULL, epoch_counter INTEGER NOT NULL,
		 session_epoch INTEGER NOT NULL, x REAL NOT NULL, y REAL NOT NULL, coins INTEGER NOT NULL CHECK(coins>=0))`,
		`CREATE TABLE IF NOT EXISTS claims (operation_id TEXT PRIMARY KEY, role_id TEXT NOT NULL REFERENCES roles(role_id))`,
		`CREATE TABLE IF NOT EXISTS combat_snapshots (
		 role_id TEXT PRIMARY KEY REFERENCES roles(role_id), owner_epoch INTEGER NOT NULL,
		 session_epoch INTEGER NOT NULL, revision INTEGER NOT NULL, state_json TEXT NOT NULL)`,
		`CREATE TABLE IF NOT EXISTS migrations (
		 role_id TEXT NOT NULL REFERENCES roles(role_id), request_id TEXT NOT NULL, transfer_id TEXT NOT NULL UNIQUE,
		 target_scene_id TEXT NOT NULL, status TEXT NOT NULL, source_json TEXT NOT NULL, target_json TEXT NOT NULL,
		 PRIMARY KEY(role_id,request_id))`,
		`CREATE UNIQUE INDEX IF NOT EXISTS one_pending_transfer ON migrations(role_id) WHERE status NOT IN ('DONE','ABORTED')`,
	} {
		if _, err = db.Exec(statement); err != nil {
			return cleanup(err)
		}
	}
	conn, err := db.Conn(context.Background())
	if err != nil {
		return cleanup(err)
	}
	db.SetMaxIdleConns(0)
	return &store{Conn: conn, pool: db, fileLock: lock}, nil
}

func (s *Server) role(ctx context.Context, id string) (Role, error) {
	r, err := scanRole(s.db.QueryRowContext(ctx, "SELECT "+columns+" FROM roles WHERE role_id=?", id))
	if errors.Is(err, sql.ErrNoRows) {
		return Role{}, &apiError{404, "角色不存在"}
	}
	if err == nil {
		err = s.loadCombat(ctx, &r)
	}
	return r, err
}

func (s *Server) validateSession(ctx context.Context, req sessionRequest) (Role, error) {
	if req.RoleID == "" || len(req.RoleID) > 128 || req.SessionEpoch <= 0 {
		return Role{}, bad("无效会话")
	}
	r, err := s.role(ctx, req.RoleID)
	if err != nil {
		return Role{}, err
	}
	if r.SessionEpoch != req.SessionEpoch {
		return Role{}, conflict("会话已失效，请重新登录")
	}
	return r, nil
}

func scanMigration(row scanner) (migration, error) {
	var m migration
	var source, target string
	err := row.Scan(&m.RoleID, &m.RequestID, &m.TransferID, &m.TargetSceneID, &m.Status, &source, &target)
	if err != nil {
		return m, err
	}
	if err = json.Unmarshal([]byte(source), &m.Source); err != nil {
		return m, err
	}
	if err = json.Unmarshal([]byte(target), &m.Target); err != nil {
		return m, err
	}
	return m, nil
}

const migrationColumns = "role_id,request_id,transfer_id,target_scene_id,status,source_json,target_json"

func (s *Server) pending(ctx context.Context, roleID string) (migration, error) {
	return scanMigration(s.db.QueryRowContext(ctx, "SELECT "+migrationColumns+" FROM migrations WHERE role_id=? AND status NOT IN ('DONE','ABORTED')", roleID))
}

func (s *Server) migration(ctx context.Context, roleID, requestID string) (migration, error) {
	return scanMigration(s.db.QueryRowContext(ctx, "SELECT "+migrationColumns+" FROM migrations WHERE role_id=? AND request_id=?", roleID, requestID))
}

func (s *Server) setStatus(ctx context.Context, m *migration, status string) error {
	if _, err := s.db.ExecContext(ctx, "UPDATE migrations SET status=? WHERE role_id=? AND request_id=?", status, m.RoleID, m.RequestID); err != nil {
		return err
	}
	m.Status = status
	return nil
}
