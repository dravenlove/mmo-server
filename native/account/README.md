# C++ AccountService

这个服务是 `internal/authsvc` 的 C++ 替代实现，直接读取和写入原账号表，不依赖 Go 转发。它只负责账号身份与登录票据；角色归属、金币、移动和战斗由 GameService / Scene 处理。

## 构建与启动

由上层 `native/CMakeLists.txt` 构建 `native-account`。Linux 依赖共享的 OpenSSL、SQLite、MariaDB Connector/C 客户端库及 libcrypt（需要 bcrypt 支持）。Connector/C 是客户端库；服务端仍要求 MySQL 8.0.16 及以上，不支持 MariaDB 服务端。

配置项兼容原 Auth：

| 配置 | 默认值 / 用途 |
| --- | --- |
| `INTERNAL_TOKEN` | 必填，至少 32 字符；内部 HTTP 鉴权 |
| `AUTH_LISTEN` | `127.0.0.1:8082` |
| `AUTH_DB_DRIVER` | `mysql`，或本机测试使用 `sqlite` |
| `AUTH_DB` | SQLite 路径，默认 `data/auth.db` |
| `MYSQL_HOST` / `MYSQL_PORT` | 主机与独立端口，默认端口 `3306` |
| `MYSQL_DATABASE` / `MYSQL_USER` | 使用现有库与应用账号 |
| `MYSQL_PASSWORD` | 密码；不打印到日志 |
| `MYSQL_PASSWORD_FILE` | 可选密码文件，优先于环境变量；仅移除末尾 CR/LF |
| `MYSQL_TLS` | `false` 或 `true`；`true` 验证服务端证书 |

当前原生连接层要求 MySQL 库名使用 ASCII。它对非 ASCII 库名启动失败，避免 C++ 与旧 Go 对 Unicode 大小写转换不同而生成不一致的命名锁；不要通过更换锁名绕过此检查。

进程直接运行 `native-account`，也兼容旧容器的 `-role auth` 参数。监听地址建议继续保持私有网络或既有加密隧道；公网玩家仍通过受信任 TLS 的 Gateway 访问，不直接暴露这个内部服务。

接口保持不变：`GET /healthz`，以及带 `X-Internal-Token` 的 `POST /v1/register`、`authenticate`、`status`、`account_register`、`account_login`、`account_bind`。请求与响应字段见 [原账号协议](../../docs/ACCOUNT_LOGIN.md)。新实现额外拒绝重复 JSON 字段与超过 16 层的嵌套，避免不同解析器对同一请求产生不同理解。

## 原账号和数据兼容

- 沿用 `auth_accounts`、`auth_passwords`、`auth_login_tokens`，不重命名表、不重建玩家账号。启动时幂等导入旧 `roles` 中的身份，冲突则整批回滚，拒绝覆盖。
- 名字保留 UTF-8 字节比较、大小写敏感和 1～24 Unicode 字符限制。密码仍按 8～72 UTF-8 字节检查，不修剪空白，拒绝控制字符。
- libcrypt 正确验证原 Go `$2a$` bcrypt 哈希。新密码用 `$2b$10$`；Go 的原 bcrypt 实现也能验证，因此回退不需要重设密码。
- 原永久 token 和稳定 `token_hash` 不轮换。临时票据独立保存 SHA-256 哈希，有效 24 小时；最多五条，通过 UPDATE 复用最早记录，**不需要 DELETE 权限**。`legacy_token_hash` 仍只在受内部鉴权保护的响应中传给 GameService。
- 密码计算最多同时两个，在数据库互斥锁外执行；账号每分钟六次、全局六十次请求的固定窗口及最多 1,024 个桶保持原约定。
- MySQL 使用固定连接持有原 `mmo-auth:` 命名锁。旧 Go Auth 仍持锁时不能启动 C++ Auth；连接丢失后不重连写入，健康检查失败，进程退出等待重新获取所有权。
- C++ Auth / Backend 可共用 SQLite 文件但写入不同表；SQLite 文件锁分职责。旧 Go Auth 没有同等文件锁，**迁移时仍必须先停止旧 Auth，不能靠 SQLite 锁证明新旧 Auth 可以并行写入**。正式部署继续优先 MySQL 的同名会话锁。

HTTP 请求体上限 64 KiB，总请求头上限 16 KiB，固定 8 个 HTTP 线程与 64 项等待队列，每连接只处理一条请求。内部密码与数据库错误不进入客户端响应或请求日志。

## 隔离验证

`native-account-unit` 验证 UTF-8、控制字符、72 字节边界、Go ASCII/中文 `$2a$` 测试向量、bcrypt 随机盐和严格 JSON。`tests/http_tests.py <native-account路径>` 在临时 SQLite 文件上验证旧身份导入、票据、绑定、过期、限流、重启及冲突回滚。

测试用的固定合成密码可以双向检查哈希兼容：

```bash
/path/to/native-account-unit --emit-bcrypt-fixture | go run native/account/tests/go_compat/hash_compat.go
```

MySQL 验证只接收明确的本机隔离实例，创建随机 `mmo_native_test_*` 库与无 DELETE 权限的测试用户，结束后清理自己创建的对象：

```bash
python3 native/account/tests/mysql_tests.py \
  --binary /path/to/native-account \
  --db-test-binary /path/to/native-db-tests \
  --host 127.0.0.1 --port 13360 \
  --admin-user root --admin-password-file /private/test-mysql-password
```

这项测试包含原 Go Auth 建号后由 C++ 登录、C++ 建号后 Go 验证、同名锁竞争、五票据复用、固定连接丢失，以及回退 Go 后继续登录。`--db-test-binary` 还验证匹配行数与写入所有权丢失。**这里的路径、端口与密码文件均属于隔离测试实例，不能指向真实业务数据库或隧道。**

代码和隔离测试通过不等于已经替换线上 Auth；正式切换仍由集成负责人统一备份、核对现有镜像与账号数据后操作。
