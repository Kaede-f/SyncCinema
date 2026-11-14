#pragma once

#include <chrono>
#include <fstream>
#include <mutex>
#include <ostream>
#include <string>
#include <variant>
#include <vector>

using TelemetryValue = std::variant<std::string, long long, double, bool>;

struct TelemetryField
{
    std::string name;
    TelemetryValue value;
};

// 一条结构化事件只描述一个已经发生的事实。耗时等数值必须在业务层使用
// steady_clock 计算完成后再放入字段；timestamp 只用于跨日志检索和展示。
struct TelemetryEvent
{
    std::string name;
    std::string sessionId;
    int clientId = 0;
    long long controlEpoch = 0;
    std::vector<TelemetryField> fields;

    TelemetryEvent& addString(std::string name, std::string value);
    TelemetryEvent& addInteger(std::string name, long long value);
    TelemetryEvent& addDouble(std::string name, double value);
    TelemetryEvent& addBoolean(std::string name, bool value);
};

// 生成 UUID v4 形式的会话标识。它只用于日志关联，不承担身份认证职责。
std::string generateTelemetrySessionId();

std::string serializeTelemetryEvent(
    const TelemetryEvent& event,
    std::chrono::system_clock::time_point timestamp =
        std::chrono::system_clock::now()
);

// server 生命周期事件的统一出口：
//   1. 控制台输出带 [telemetry] 前缀，方便人工观察；
//   2. 设置 SYNCCINEMA_TELEMETRY_PATH 后，同时写入纯 JSONL 文件；
//   3. 一把锁覆盖整行序列化与写入，避免多线程把 JSON 拼在一起。
class TelemetryLogger
{
public:
    explicit TelemetryLogger(
        std::ostream* consoleOutput,
        const std::string& jsonlPath = {}
    );

    TelemetryLogger(const TelemetryLogger&) = delete;
    TelemetryLogger& operator=(const TelemetryLogger&) = delete;

    bool emit(const TelemetryEvent& event);
    bool fileEnabled() const;
    const std::string& initializationError() const;

    static std::string pathFromEnvironment();

private:
    std::ostream* consoleOutput_ = nullptr;
    std::ofstream jsonlFile_;
    std::string initializationError_;
    mutable std::mutex mutex_;
};
