// bridge.h — 数据面契约（步骤 1 实现）
// 一个"通道" = 一个 IBridge。串口通道、SSH 出站通道都实现它。
// 关键约定：ring 归 Bridge 独占持有；所有读者通过 read_since 拿增量，
// 各自维护 epoch/seen 游标，因此终端窗格、入站会话、网页 WS 读的是同一份数据。

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace sb3 {

class IBridge {
public:
    virtual ~IBridge() = default;

    // 生命周期
    virtual bool start() = 0;
    virtual void stop()  = 0;
    virtual bool running() const = 0;
    virtual void reconnect() = 0;

    // 身份
    virtual const std::string& name() const = 0;

    // 下行：UI / 服务把字节写进通道（内部入写队列，不阻塞调用方）
    virtual std::size_t write(const void* data, std::size_t n) = 0;

    // 最近一次“发送→首个回显”的时延（微秒）；无数据时返回 0
    virtual std::uint64_t echo_latency_us() const { return 0; }
    virtual std::uint64_t echo_count() const { return 0; }
    virtual std::uint64_t tx_bytes() const { return 0; }
    virtual std::uint64_t rx_bytes() const { return 0; }

    // 上行：读 ring 增量。首次读把 epoch/seen 置为当前游标。
    // epoch 变化表示 ring 被清空或回绕，调用方应丢弃旧画面重画。
    virtual std::string read_since(std::uint64_t* epoch, std::uint64_t* seen) = 0;

    // 全量读：拿 ring 里现存的全部上行字节（用于"加载历史会话"重放）。
    // 只读、不改任何读者游标；内容可能从行中间开始。
    virtual std::string read_all(std::uint64_t* epoch, std::uint64_t* seen) = 0;

    // 通知计数：单调递增，读者先用它判断"有没有新数据"，避免无谓拷贝
    virtual std::uint64_t notify_seq() const = 0;

    // 日志控制
    virtual void clear() = 0;
    virtual void set_disk_log(bool enabled) = 0;

    // 状态快照（JSON 文本），供状态栏 / 网页 / Agent 共用
    virtual std::string status_json() const = 0;
};

}  // namespace sb3
