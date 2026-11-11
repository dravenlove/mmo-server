#!/bin/sh
set -eu

# Install only the new bridge and one domain-specific Caddy include.
# Existing gateway/backend containers, source checkout and the original :80 site are retained.
stage=${1:-/home/ubuntu/axiomancy-deploy-20261111}
test -f "$stage/websocket-bridge"
test -f "$stage/mmo-websocket.service"
test -f "$stage/gate.draven.world.caddy"
install -d -m 755 /opt/mmo-websocket /etc/caddy/conf.d /srv/axiomancy
if [ ! -f /etc/caddy/Caddyfile.before-axiomancy-20261111 ]; then
    cp -p /etc/caddy/Caddyfile /etc/caddy/Caddyfile.before-axiomancy-20261111
fi
if [ -f /opt/mmo-websocket/websocket-bridge ]; then
    cp -p /opt/mmo-websocket/websocket-bridge /opt/mmo-websocket/websocket-bridge.previous
fi
install -m 755 "$stage/websocket-bridge" /opt/mmo-websocket/websocket-bridge
install -m 644 "$stage/mmo-websocket.service" /etc/systemd/system/mmo-websocket.service
install -m 644 "$stage/gate.draven.world.caddy" /etc/caddy/conf.d/axiomancy.caddy
if ! grep -Fq 'import /etc/caddy/conf.d/*.caddy' /etc/caddy/Caddyfile; then
    printf '\nimport /etc/caddy/conf.d/*.caddy\n' >> /etc/caddy/Caddyfile
fi
caddy validate --config /etc/caddy/Caddyfile --adapter caddyfile
systemctl daemon-reload
systemctl enable --now mmo-websocket.service
systemctl restart mmo-websocket.service
curl --fail --silent --show-error --retry 5 --retry-connrefused --retry-delay 1 http://127.0.0.1:7001/healthz
systemctl reload caddy
systemctl is-active mmo-websocket.service caddy.service
sha256sum /opt/mmo-websocket/websocket-bridge
if [ ! -f /srv/axiomancy/index.html ]; then
    cat > /srv/axiomancy/index.html <<'HTML'
<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width">
<title>万象法则 · 联网入口</title><style>body{max-width:760px;margin:12vh auto;padding:24px;background:#101d26;color:#dfe9ef;font:18px/1.8 sans-serif}a{color:#65d8c1}code{color:#ffc67b}</style>
<h1>万象法则 · 联网入口</h1><p>WebSocket 接入地址：<code>wss://gate.draven.world/ws</code></p>
<p><a href="/healthz">检查接入服务</a></p><p>Unity WebGL 客户端产物尚未上传。源代码已准备联网移动与 AOI 显示，完成手动 Unity 云端构建后，将完整产物部署到本目录即可运行。</p>
</html>
HTML
fi
