# IP 地址联调入口

更新：2026-11-12（Asia/Shanghai）。当前使用 **公网 IP + 受信任 WSS** 联调，协议仍为 `webgl-movement-v1`，业务程序和容器镜像保持 `webgl-20261111-v1`。实际 IP 由部署人员提供；本文不保存真实 VPS 地址。

## 客户端填写什么

以下 `203.0.113.10` 是示例，必须替换为实际游戏主机公网 IP。

| 用途 | 地址 |
| --- | --- |
| WebSocket | `wss://203.0.113.10/ws` |
| HTTPS 健康检查 | `https://203.0.113.10/healthz` |
| 同机网页 Origin | `https://203.0.113.10` |

客户端仍使用连接后的 `register` / `login`，已有凭据可继续登录；接口、场景、方向、AOI、序号和代次语义见 [完整交接协议](WEBGL_MOVEMENT_HANDOFF.md)。本轮只更改入口和访问许可，没有修改 Unity 客户端代码、数据库地址、角色存档或战斗场景。

Origin 是**实际运行游戏的网页**的协议、主机和端口，不能填写 WSS URL。当前增加 IP 站点的 HTTPS Origin，并保留原来已授权的域名 Origin。2026-11-12 按用户提供的地址栏来源加入 `https://cloud.unity.com`；随后用户从实际 WS 请求头确认游戏 iframe 来源为 `https://developer.unity3dusercontent.com`，故补充该精确 Origin。其他本地调试端口或网页地址须分别加入精确允许列表；Unity 预览以实际 WebSocket 请求头为准。HTTPS 页面可直接连接受信任的 IP WSS。

目前 IP 网站根目录仍是等待上传 Unity 构建产物的状态页；它根据当前网页地址显示 WSS，不代表 Unity 游戏已经发布。健康检查只表示转接进程响应，业务连通以角色验收为准。

## 浏览器报 1006 时

1006 表示浏览器未收到正常关闭帧，不能单凭它判断是 Origin、TLS、网络还是连接后的协议错误。先在 F12 → Network → WS 查看实际 Request URL、Status Code，以及展开请求标头后的 Origin；再看 Console 完整报错。不要发送玩家 token、Cookie 或消息内容。

游戏主机上的转接进程现在记录连接阶段和固定原因，可查看：

```bash
sudo journalctl -u mmo-websocket.service --since '5 minutes ago' --no-pager
```

| 日志 | 含义 |
| --- | --- |
| `handshake_rejected`、`origin_not_allowed`、403 | 实际 Origin 未在精确列表中；iframe 来源可能与地址栏不同 |
| `handshake_rejected`、`gateway_unavailable`、502 | 转接进程不能连接本机 movement Gateway |
| `connected` | 已升级为 WebSocket，不代表角色登录已成功 |
| `invalid_text_json`、1007 | 收到了不符合单条 JSON 对象要求的文本 |
| `text_json_required`、1003 | 收到了二进制消息，当前协议只接受文本 |
| `gateway_closed` / `gateway_read_failed` / `gateway_write_failed`、1011 | 本机 Gateway 链路断开，继续检查其容器日志 |
| `client_transport_closed`、`peer_code=1006` | 对端未发送关闭帧；结合浏览器 Console 判断 |

每条成功连接的日志以 `connection` 编号关联，关闭记录含持续毫秒数。`close_code=0` 表示本次诊断路径没有额外发送关闭帧（对端正常关闭时库会自动应答）；不将它作为浏览器的关闭码。日志只包含经过校验的 Origin、固定原因与关闭码，不记录消息、查询串、认证头或客户端关闭说明。没有 `connected` / 拒绝日志并不能证明客户端没有尝试；请求可能在到达转接进程前被 TLS、代理或浏览器安全策略阻止。

## TLS 与自动续期

IP 入口使用 Let's Encrypt 的公开可信 IP 证书，SAN 为公网 IP；客户端继续验证证书链与地址匹配。Let's Encrypt 的 IP 证书使用 `shortlived` 配置，证书有效期约 160 小时。[签发说明](https://letsencrypt.org/2026/01/15/6day-and-ip-general-availability/)、[配置资料](https://letsencrypt.org/docs/profiles/)

本次已在 Caddy 2.11.7 验证该配置。Caddy 常驻管理证书与续期。应保持 Caddy 服务、证书存储和验证所需的公网 80/443 可用；本配置只使用 HTTP-01 验证，IP 改变时须同步入口配置与 Origin，并重新申请对应 IP 的证书。已经核对 Caddy 的自动证书管理与续期信息，尚未经历完整的六日到期周期。[Caddy 自动 HTTPS](https://caddyserver.com/docs/automatic-https)

云主机的公网 IP 通常映射到私网网卡。客户端按 IP 建立 TLS 时通常不发送域名 SNI，因此主 Caddyfile 的全局 `default_sni` 必须使用实际公网 IP，避免选择到私网地址的证书。[Caddy 配置说明](https://caddyserver.com/docs/caddyfile/options#default-sni)

## 部署文件

| 文件 | 实际安装位置 |
| --- | --- |
| [IP 站点模板](../deploy/ip-webgl.caddy.example) | `/etc/caddy/conf.d/axiomancy-ip.caddy` |
| [IP 环境模板](../deploy/ip-ingress.env.example) | `/etc/mmo-ip-ingress.env` |
| [Caddy 服务环境模板](../deploy/caddy-ip.service.conf.example) | `/etc/systemd/system/caddy.service.d/mmo-ip.conf` |
| [Origin 配置模板](../deploy/websocket-ip.env.example) | 合并到已有 `/etc/mmo-websocket.env` |
| [等待构建的状态页](../deploy/webgl-status.html) | 仅空站点使用；已有 Unity 构建时保留实际产物 |

环境文件填写 `MMO_PUBLIC_IP=实际公网IP`，权限 `600`。主 `/etc/caddy/Caddyfile` 的**首个全局块**内加入：

```caddyfile
{
    default_sni {$MMO_PUBLIC_IP}
}
```

已经存在全局块时，将这一行并入它，不要创建第二个全局块。现有域名配置和其他站点继续保留。IP 站点模板显式指定 ACME `shortlived`，不依赖 Caddy 针对 IP 的默认本地 CA 行为。

在 WebSocket 环境里加入 `https://实际公网IP`，保留仍需授权的已有网页。先备份 Caddyfile、站点和环境文件，验证配置后：

```bash
sudo systemctl daemon-reload
sudo systemctl restart caddy.service
sudo systemctl restart mmo-websocket.service
sudo systemctl is-active caddy.service mmo-websocket.service
```

新增 systemd 环境配置后需要重启 Caddy 才能载入变量。入口切换可能断开 WebSocket，重连会复用原角色凭据；游戏容器无需重建。

本次私有备份位于游戏仓库 `backups/ip-wss-20261112/`。回退入口时恢复该目录中的原 Caddyfile、WebSocket 环境和状态页，并移除本次新增的 IP 站点与 Caddy drop-in，然后执行 daemon-reload 并重启 Caddy 与 WebSocket 服务。保留证书存储、数据卷和 MySQL；入口回退不需要恢复 SQL。

## 2026-11-12 验收范围

Windows 客户机禁用测试进程的 HTTP 代理，直接连接实际公网 IP，保持系统默认 TLS 验证：

- HTTPS 健康检查返回 200，证书 SAN 与 IP 匹配，无自签信任操作。
- WSS 完成精确 Origin、查询参数拒绝和未登录移动拒绝。
- 复用两个受控验收角色，完成登录、4 条移动在途、停止、250ms 输入失联、AOI 离开/返回、新会话与旧连接隔离、正常断线隐藏。
- 62 次移动回执中位约 49.75ms、最大约 51.61ms；114 次视图回执中位约 50ms、最大约 101.18ms。这是单个 Windows 网络测点的短时协议数据。

没有新增测试账号或修改现有玩家凭据。未进行 Unity 引擎构建、浏览器实际按键操作、输入到首帧测量或容量压测。原域名仍受其备案状态限制；IP 联调不等于原域名已放行。
