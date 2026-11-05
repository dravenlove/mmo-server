package backend

import (
	"context"
	"crypto/sha256"
	"database/sql"
	"errors"
	"fmt"
	"net"
	"strconv"
	"strings"
	"time"

	"github.com/go-sql-driver/mysql"
)

func mysqlConfig(input MySQLConfig) (*mysql.Config, error) {
	input.Host = strings.TrimSpace(input.Host)
	if strings.TrimSpace(input.Host) == "" || input.User == "" || input.Database == "" {
		return nil, fmt.Errorf("MySQL 模式必须配置 MYSQL_HOST、MYSQL_USER、MYSQL_DATABASE")
	}
	if strings.ContainsAny(input.Host, "/@?#\\ \t\r\n") || (strings.Contains(input.Host, ":") && net.ParseIP(input.Host) == nil) {
		return nil, fmt.Errorf("MYSQL_HOST 只填写主机名或 IP，端口请使用 MYSQL_PORT")
	}
	if input.Port == "" {
		input.Port = "3306"
	}
	port, err := strconv.Atoi(input.Port)
	if err != nil || port < 1 || port > 65535 {
		return nil, fmt.Errorf("MYSQL_PORT 必须为 1..65535")
	}
	if input.TLS == "" {
		input.TLS = "false"
	}
	if input.TLS != "false" && input.TLS != "true" {
		return nil, fmt.Errorf("MYSQL_TLS 只支持 false 或 true（校验证书）")
	}
	cfg := mysql.NewConfig()
	cfg.Net, cfg.Addr = "tcp", net.JoinHostPort(input.Host, input.Port)
	cfg.User, cfg.Passwd, cfg.DBName = input.User, input.Password, input.Database
	cfg.TLSConfig = input.TLS
	cfg.Timeout, cfg.ReadTimeout, cfg.WriteTimeout = 5*time.Second, 5*time.Second, 5*time.Second
	// SQLite 返回匹配行数；MySQL 默认返回实际变化行数，会误拒绝静止角色的检查点。
	cfg.ClientFoundRows = true
	cfg.RejectReadOnly = true
	if err := cfg.Apply(mysql.Charset("utf8mb4", "")); err != nil {
		return nil, err
	}
	return cfg, nil
}

func openMySQLStore(input MySQLConfig) (*store, error) {
	cfg, err := mysqlConfig(input)
	if err != nil {
		return nil, err
	}
	connector, err := mysql.NewConnector(cfg)
	if err != nil {
		return nil, fmt.Errorf("MySQL 配置无效: %w", err)
	}
	pool := sql.OpenDB(connector)
	pool.SetMaxOpenConns(1)
	pool.SetMaxIdleConns(0)
	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
	defer cancel()
	conn, err := pool.Conn(ctx)
	if err != nil {
		pool.Close()
		return nil, fmt.Errorf("MySQL 连接失败（检查地址、账号和数据库）: %w", err)
	}
	s := &store{Conn: conn, pool: pool}
	fail := func(err error) (*store, error) { s.Close(); return nil, err }
	var version string
	if err = conn.QueryRowContext(ctx, "SELECT VERSION()").Scan(&version); err != nil {
		return fail(err)
	}
	var major, minor, patch int
	if _, err = fmt.Sscanf(version, "%d.%d.%d", &major, &minor, &patch); err != nil ||
		strings.Contains(strings.ToLower(version), "mariadb") || major < 8 || (major == 8 && minor == 0 && patch < 16) {
		return fail(fmt.Errorf("需要 MySQL 8.0.16 或更高版本，当前版本 %s", version))
	}
	// 锁与业务 SQL 固定在同一会话。断线后 sql.Conn 不会从池中重新取连接，
	// 因此旧 Backend 不会在锁已被新进程取得后继续写入；需重启进程重新获取锁。
	sum := sha256.Sum256([]byte(strings.ToLower(input.Database)))
	lockName := fmt.Sprintf("mmo-backend:%x", sum[:24])
	var locked sql.NullInt64
	if err = conn.QueryRowContext(ctx, "SELECT GET_LOCK(?,0)", lockName).Scan(&locked); err != nil {
		return fail(fmt.Errorf("MySQL 独占锁获取失败: %w", err))
	}
	if !locked.Valid || locked.Int64 != 1 {
		return fail(fmt.Errorf("MySQL 数据库已被其他 Backend 占用或无法加锁"))
	}
	for _, statement := range mysqlSchema {
		if _, err = conn.ExecContext(ctx, statement); err != nil {
			return fail(fmt.Errorf("MySQL 建表失败: %w", err))
		}
	}
	var transactionalTables int
	if err = conn.QueryRowContext(ctx, `SELECT COUNT(*) FROM information_schema.TABLES
	 WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME IN ('roles','claims','migrations') AND ENGINE='InnoDB'`).Scan(&transactionalTables); err != nil {
		return fail(err)
	}
	if transactionalTables != 3 {
		return fail(fmt.Errorf("MySQL 业务表必须使用 InnoDB，不能使用不支持事务的已有表"))
	}
	return s, nil
}

// VARBINARY 保留 SQLite 的字节比较语义，避免 MySQL 默认排序规则合并大小写或重音名。
var mysqlSchema = []string{
	`CREATE TABLE IF NOT EXISTS roles (
	 role_id VARBINARY(128) PRIMARY KEY, name VARBINARY(96) NOT NULL UNIQUE,
	 token_hash BINARY(64) NOT NULL UNIQUE, scene_id VARBINARY(64) NOT NULL,
	 owner_epoch BIGINT NOT NULL, epoch_counter BIGINT NOT NULL, session_epoch BIGINT NOT NULL,
	 x DOUBLE NOT NULL, y DOUBLE NOT NULL, coins BIGINT NOT NULL CHECK(coins>=0)
	) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4`,
	`CREATE TABLE IF NOT EXISTS claims (
	 operation_id VARBINARY(160) PRIMARY KEY, role_id VARBINARY(128) NOT NULL,
	 FOREIGN KEY (role_id) REFERENCES roles(role_id)
	) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4`,
	`CREATE TABLE IF NOT EXISTS migrations (
	 role_id VARBINARY(128) NOT NULL, request_id VARBINARY(128) NOT NULL,
	 transfer_id VARBINARY(128) NOT NULL UNIQUE, target_scene_id VARBINARY(64) NOT NULL,
	 status VARBINARY(16) NOT NULL, source_json JSON NOT NULL, target_json JSON NOT NULL,
	 pending_role VARBINARY(128) GENERATED ALWAYS AS
	 (CASE WHEN status NOT IN ('DONE','ABORTED') THEN role_id ELSE NULL END) STORED,
	 PRIMARY KEY (role_id,request_id), UNIQUE KEY one_pending_transfer (pending_role),
	 FOREIGN KEY (role_id) REFERENCES roles(role_id)
	) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4`,
}

func uniqueViolation(err error) bool {
	var mysqlError *mysql.MySQLError
	if errors.As(err, &mysqlError) {
		return mysqlError.Number == 1062
	}
	var sqliteError interface{ Code() int }
	return errors.As(err, &sqliteError) && (sqliteError.Code() == 1555 || sqliteError.Code() == 2067)
}
