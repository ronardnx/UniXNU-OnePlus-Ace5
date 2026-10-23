# QMUX 与 QRTR 的实际报文转换

`ios16/modem/giulia_modem_qmux.h` 实现封包、拆包、虚拟客户端、事务转换与响应路由。
它使用 `gmr_identity` 的运行代次、订阅代次和服务端点代次，但不维护第二套蜂窝业务状态。
对未知服务消息，只验证协议结构并转发；收到成功 result TLV 不会把 SIM、IMS 或数据承载设为可用。

固定依据为 libqmi `d9e8af5cae7fdf9749baf87606568d3af4d03082`：

- [qmi-message.c](https://github.com/linux-mobile-broadband/libqmi/blob/d9e8af5cae7fdf9749baf87606568d3af4d03082/src/libqmi-glib/qmi-message.c)、[qmi-message.h](https://github.com/linux-mobile-broadband/libqmi/blob/d9e8af5cae7fdf9749baf87606568d3af4d03082/src/libqmi-glib/qmi-message.h)：实际帧结构与 marker。
- [qmi-enums-private.h](https://github.com/linux-mobile-broadband/libqmi/blob/d9e8af5cae7fdf9749baf87606568d3af4d03082/src/libqmi-glib/qmi-enums-private.h)：CTL 和服务消息的不同 flags。
- [qmi-service-ctl.json](https://github.com/linux-mobile-broadband/libqmi/blob/d9e8af5cae7fdf9749baf87606568d3af4d03082/data/qmi-service-ctl.json)、[qmi-errors.h](https://github.com/linux-mobile-broadband/libqmi/blob/d9e8af5cae7fdf9749baf87606568d3af4d03082/src/libqmi-glib/qmi-errors.h)：客户端管理消息与错误码。

## 线格式

| 层 | 实际字段 |
| --- | --- |
| QMUX，6 字节 | marker `1`；LE16 length 为总长减 1；sender `0` 或 `0x80`；service U8；CID U8 |
| CTL，6 字节 | flags U8；transaction U8；message LE16；TLV 长度 LE16 |
| 非 CTL 服务，7 字节 | flags U8；transaction LE16；message LE16；TLV 长度 LE16 |
| QRTR 数据报 | 只有上述非 CTL QMI 负载；QMUX 前缀不会发到远端 |

CTL 的 response/indication flags 是 `1/2`，其他服务是 `2/4`。
`gmux_decode` 要求一个完整帧，检查两层长度、方向、TLV 边界和重复 TLV。
compound 消息和 libqmi 内部伪 QRTR marker `2` 明确返回不支持。
ABP 的流分段/拼帧必须在调用该接口之前完成。

## 客户端与事务路径

一个 `gmux_context` 对应一个经过证据确认的 Apple QMUX transport/订阅。
`gmux_set_endpoint` 接收实际 QRTR 发现的服务地址和代次。标准 QMUX 只有 U8 service；
大于 255 的 QRTR 服务不会被截断映射。

CTL Allocate CID `0x0022` 的输入为 TLV `1` 的单字节 service。
核心分配虚拟 CID，并产生真实 QMUX 成功响应以及 `GMUX_CLIENT_ALLOCATED` 标志。
此时只完成了虚拟编号分配。适配层必须创建该客户端独占的真实 QRTR socket，调用
`gmux_bind_client` 记录实际 local port，再把成功帧发布到 Apple。
未绑定客户端不能发送服务请求，两个客户端不能共用同一个 local port。

CTL Release CID `0x0023` 的输入为 TLV `1` 的 `{service, CID}`。
尚有绑定 socket 时返回 `GMR_BUSY`，输出包含需要关闭的 port 和 generation，不生成成功回复。
适配层实际关闭并排空 socket 后调用 `gmux_unbind_client`，再提交原 release 请求。
解绑会取消旧事务和响应缓存并提升 client generation；确认释放后才生成成功响应。
未知 CTL 操作返回协议 `NOT_SUPPORTED=94`，不伪造版本列表、Sync 或数据格式协商。

非 CTL 请求保留完整 QMI 消息和未知 TLV，只重写 16 位 wire transaction。
pending 记录同时包含 host 的 `(service, CID, transaction)`、实际 local port、
client generation、服务端点代次和 `gmr_identity`。响应必须匹配这些关联，并通过公共
result/framing 校验后，才恢复原 host transaction 和 QMUX 前缀。
indication 通过收到数据的真实 socket 路由，不从共享 server 地址猜测客户端。

## 重传、取消和 SSR

调用者为每个逻辑请求提供单调 cookie，同一请求的重传保持 cookie 不变。
pending 重传返回 busy，完成后的重传返回缓存中的同一回复；同 cookie 修改字节会被拒绝。
有界缓存淘汰后，低于 highwater 的旧 cookie 返回 stale，不再次发送有副作用的请求。
相同 host transaction 的新 cookie，必须等旧请求完成或取消后才可使用。
wire transaction 和虚拟 CID 在同一个 epoch 内不回绕复用。

`gmux_cancel` 撤销本地等待与响应接收资格；它不声称远端操作已回滚。
端点更换或消失会取消关联客户端及响应缓存。`gmux_reset` 立即切换 epoch 并失效所有
旧事务，同时保留需要实际关闭的旧 socket 记录；这些端口未解绑前禁止提交新请求。
切换订阅而 epoch 未变时，可以只推进 slot generation；此时保留 cookie、CID 和 wire
transaction 的 highwater，避免旧 host 帧与新订阅复用编号发生别名。

## 接入与统一验证

统一系统场景使用固定 CTL allocate/release 字节 oracle，检查同一服务多个客户端、
host/wire 事务转换、真实消息往返、重复请求、错误来源、旧客户端代次、取消及 SSR。
该模块没有独立测试入口。

此实现解决 QMUX/QRTR 的报文与路由机制。Apple `qmux1/qmux2` 的 ABP provider、
共享内存/通知、实际 socket 与 GLINK、以及 CommCenter 的完整业务协议仍须分别接入。
编解码成功不能使 managed 构建门槛变为通过。
