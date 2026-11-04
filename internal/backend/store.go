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

type scanner interface{ Scan(...any) error }

func scanRole(row scanner) (Role, error) {
	var r Role
	err := row.Scan(&r.RoleID, &r.Name, &r.SceneID, &r.OwnerEpoch, &r.SessionEpoch, &r.X, &r.Y, &r.Coins)
	return r, err
}

func openStore(path string) (*sql.DB, *os.File, error) {
	abs, err := filepath.Abs(path)
	if err != nil {
		return nil, nil, err
	}
	if err = os.MkdirAll(filepath.Dir(abs), 0700); err != nil {
		return nil, nil, err
	}
	// 解析目录符号链接，让同一数据库的普通别名也竞争同一把文件锁。
	dir, err := filepath.EvalSymlinks(filepath.Dir(abs))
	if err != nil {
		return nil, nil, err
	}
	abs = filepath.Join(dir, filepath.Base(abs))
	if actual, e := filepath.EvalSymlinks(abs); e == nil {
		abs = actual
	}
	lock, err := os.OpenFile(abs+".lock", os.O_CREATE|os.O_RDWR, 0600)
	if err != nil {
		return nil, nil, err
	}
	if err = lockFile(lock); err != nil {
		lock.Close()
		return nil, nil, fmt.Errorf("数据库已被其他 Backend 占用或无法加锁: %w", err)
	}
	db, err := sql.Open("sqlite", abs)
	if err != nil {
		lock.Close()
		return nil, nil, err
	}
	db.SetMaxOpenConns(1)
	cleanup := func(e error) (*sql.DB, *os.File, error) { db.Close(); lock.Close(); return nil, nil, e }
	for _, statement := range []string{
		"PRAGMA journal_mode=WAL", "PRAGMA synchronous=FULL", "PRAGMA busy_timeout=5000", "PRAGMA foreign_keys=ON",
		`CREATE TABLE IF NOT EXISTS roles (
		 role_id TEXT PRIMARY KEY, name TEXT NOT NULL UNIQUE, token_hash TEXT NOT NULL UNIQUE,
		 scene_id TEXT NOT NULL, owner_epoch INTEGER NOT NULL, epoch_counter INTEGER NOT NULL,
		 session_epoch INTEGER NOT NULL, x REAL NOT NULL, y REAL NOT NULL, coins INTEGER NOT NULL CHECK(coins>=0))`,
		`CREATE TABLE IF NOT EXISTS claims (operation_id TEXT PRIMARY KEY, role_id TEXT NOT NULL REFERENCES roles(role_id))`,
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
	return db, lock, nil
}

func (s *Server) role(ctx context.Context, id string) (Role, error) {
	r, err := scanRole(s.db.QueryRowContext(ctx, "SELECT "+columns+" FROM roles WHERE role_id=?", id))
	if errors.Is(err, sql.ErrNoRows) {
		return Role{}, &apiError{404, "角色不存在"}
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
