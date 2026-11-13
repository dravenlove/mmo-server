# 本项目的协议选择

目前以 Unity WebGL 联调为主，继续使用 **WSS**；以后扩展 Windows、Android 原生客户端时，再评估 UDP 状态快照与 KCP 可靠消息。

| 协议 | 特点 | 项目用途 |
| --- | --- | --- |
| TCP | 可靠有序字节流，需自行处理消息边界；前方丢包会阻塞后方数据交付。 | 现有原生入口及服务内部通信；公网敏感数据还需 TLS。 |
| UDP | 数据报，不保证送达、顺序和去重；新包不必等待旧包重传。 | 未来原生客户端的位置快照，需处理序号、过期包、认证、拥塞和分片预算。 |
| WebSocket / WSS | RFC 6455 基于 TCP，提供双向消息，WSS 增加 TLS；仍有 TCP 顺序等待。 | 当前浏览器 WebGL 公网入口。它与 TCP 不是同一层次的替代选项。 |
| KCP | 通常接在 UDP 上的可靠 ARQ 算法，带重传和有序交付；不自带完整连接、安全与拥塞治理。 | 未来原生客户端的可选可靠消息通道；单有序通道仍可能等待丢包。 |

浏览器不能直接访问原生 TCP/UDP 套接字，常规 KCP 客户端无法直接照搬到 WebGL。浏览器可另评估 WebTransport/WebRTC，但需额外服务端适配和兼容性验证，不是更换一个地址就能接入。

当前先完成登录、技能、AOI、快照体积和队列管理，客户端后续采用本地预测、服务器纠正与远端插值。20Hz 场景每 Tick 为 50ms，Tick 等待本身就可能占 0～50ms；ping 低不等于按键到画面响应快。

原生版可把位置/方向快照放在允许丢弃旧包的 UDP 通道，重要操作走可靠通道，配合业务操作 ID、确认与服务端去重。可靠传输不等于伤害或交易只结算一次。不要把所有高频快照放进同一可靠顺序队列。

引入 KCP 前，用相同链路测试 RTT、抖动、1%/3%/5% 丢包，比较输入确认、首个可见状态、P95/P99 和真实传输字节。积极重传可能增加带宽与 CPU，在带宽不足时也可能更糟。协议切换不会缩短地理距离，也不能自动解决场景堵塞、数据库等待、积压或浏览器渲染慢。

依据：[Unity Web networking](https://docs.unity3d.com/6000.0/Documentation/Manual/webgl-networking.html)、[WebSocket RFC 6455](https://www.rfc-editor.org/rfc/rfc6455.html)、[TCP RFC 9293](https://www.rfc-editor.org/rfc/rfc9293.html)、[UDP RFC 768](https://www.rfc-editor.org/rfc/rfc768)、[KCP 作者仓库](https://github.com/skywind3000/kcp)。
