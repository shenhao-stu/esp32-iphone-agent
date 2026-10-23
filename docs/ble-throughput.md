# BLE 吞吐与 MTU 决策（2026-10-23）

能优化，但不能把请求 MTU、PHY 标称速率当作实际应用吞吐。当前代码请求 ATT MTU 517、
DLE 251 字节、连接间隔 15 ms、peripheral latency 0；PHY 保持 1M，因为旧代码记录了强制
2M 时弱信号反复断线的问题。iOS 最终协商结果与同机 Wi-Fi/BLE 共存仍需实测。

## 本轮收口

- MTU 按实际接收该通道的 peer 计算。仅订阅 status 的低 MTU Mac 不再限制 iPhone 的视频分包；
  video/reply 还要求 encrypted + bonded。两个真正的视频接收者仍按较小上限发送，以保留同一帧序列。
- notification value 遵守 `min(ATT_MTU - 3, 512)`，再扣去本协议的 3 字节视频头；
  MTU 517 的视频净负载上限是 509 字节，MTU 247 是 241 字节。起始包另含 4 字节时间戳。
  不依赖 NimBLE 自动截断；发送前重新检查 peer 上限，超长必须明确失败。
- 完整状态属性与短通知分离。各目标分别选择完整或短心跳，低 MTU Mac 不抑制手机通知；短心跳保留当次 lag/fps，
  GATT long read 仍返回完整状态。MTU 改变时禁止截断 JSON。新增实际视频 MTU 指标后，完整状态最大数值样本 493 字节，仍在 512 字节属性上限内。
- 心跳 ping 已不再触发视频流量限制；HID 命令、停止与回复仍优先，不能只为跑满速率而延迟停止。

这些证明分包与调度边界正确，不是已在 iPhone 测得带宽提升。

## 下一次真机比较

使用同一手机、供电与场景，先完成 BLE HID/镜像闭环，再比较 1M/2M。每组记录实际 MTU、
PHY、连接间隔、AirPlay 输入与 BLE 输出 kbps、lag、丢包、重连次数及停止 p95 延迟。
分别测无视频基准、静态界面、滚动页面、键鼠与视频并发；Mac 不旁听视频。

MTU 247 的 ATT 包可放进一个 DLE 251 字节载荷（另有4字节L2CAP头）；更大 ATT 包需要链路
分片，不能假定517必然胜过247。2M可缩短空中时间，但无线干扰和共存调度可能抵消收益。
优先选择无积压且停止及时的配置，不以单次峰值决定默认值。

若BLE仍不足，候选是视频改走 Wi-Fi/TCP，BLE保留HID；这是新传输方案，需重新验证iOS后台、
局域网权限、鉴权和AirPlay并发，不在本轮宣称已经实现。不能把ESP32-S3硬件升级成不支持的PHY。

## 一手资料

- [Espressif ESP32-S3](https://www.espressif.com/products/socs/esp32-s3)：硬件支持2M PHY。
- [Espressif BLE connection](https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32s3/api-guides/ble/get-started/ble-connection.html)：连接事件、DLE251及ATT/L2CAP分片。
- [Apple maximumWriteValueLength](https://developer.apple.com/documentation/corebluetooth/cbperipheral/maximumwritevaluelength(for:))：按系统报告的写入长度使用CoreBluetooth。
- 固件 agent 另核对本机 NimBLE 源码与 Bluetooth ATT 属性512字节上限；回归及构建证据见
  `work/integration-20261023/firmware-report.md` 和 `firmware-review.md`（相对IOSAgent根目录）。

## 可复现诊断入口（2026-10-23 补充）

新增串口/已配对 GATT 命令：

- `status`：完整状态 JSON（小 MTU 上请使用已有 status 属性长读，避免命令回复受通知长度限制）。
- `link`：各连接的实际 MTU、TX/RX PHY、连接间隔、latency、supervision timeout、加密和视频订阅状态；不包含地址或密钥。
- `phy 1|2`：只在恰好一个连接且已加密订阅视频时请求改变 PHY。回复 `requested` 不是协商成功，必须再读 `link`。设置不持久化，启动仍以 1M 为默认。

`--debug` 固件会记录各 HID report 的订阅，帮助区分“App 控制已连”和“系统鼠标已就绪”。新增这些能力不等于已经取得 1M/2M 实测结果。
