#include "Telemetry.h"

#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <random>
#include <sstream>
#include <system_error>
#include <type_traits>
#include <utility>

#include "MetricLogger.h"

namespace
{
    constexpr int kTelemetrySchemaVersion = 1;

    std::string escapeJson(const std::string& value)
    {
        std::ostringstream output;
        output << std::hex << std::setfill('0');

        for (unsigned char character : value)
        {
            switch (character)
            {
            case '"':
                output << "\\\"";
                break;
            case '\\':
                output << "\\\\";
                break;
            case '\b':
                output << "\\b";
                break;
            case '\f':
                output << "\\f";
                break;
            case '\n':
                output << "\\n";
                break;
            case '\r':
                output << "\\r";
                break;
            case '\t':
                output << "\\t";
                break;
            default:
                if (character < 0x20)
                {
                    output << "\\u" << std::setw(4)
                        << static_cast<int>(character);
                }
                else
                {
                    output << static_cast<char>(character);
                }
                break;
            }
        }
        return output.str();
    }

    std::string formatUtcTimestamp(
        std::chrono::system_clock::time_point timestamp)
    {
        using namespace std::chrono;

        const auto millisecondsSinceEpoch =
            duration_cast<milliseconds>(timestamp.time_since_epoch());
        const auto secondPart = duration_cast<seconds>(millisecondsSinceEpoch);
        const int millisecondPart = static_cast<int>(
            (millisecondsSinceEpoch - secondPart).count()
        );
        const std::time_t time = system_clock::to_time_t(
            system_clock::time_point(secondPart)
        );

        std::tm utc{};
#ifdef _WIN32
        gmtime_s(&utc, &time);
#else
        gmtime_r(&time, &utc);
#endif

        std::ostringstream output;
        output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S")
            << '.' << std::setfill('0') << std::setw(3)
            << millisecondPart << 'Z';
        return output.str();
    }

    void appendJsonValue(std::ostringstream& output, const TelemetryValue& value)
    {
        std::visit(
            [&output](const auto& concreteValue)
            {
                using ValueType = std::decay_t<decltype(concreteValue)>;
                if constexpr (std::is_same_v<ValueType, std::string>)
                {
                    output << '"' << escapeJson(concreteValue) << '"';
                }
                else if constexpr (std::is_same_v<ValueType, bool>)
                {
                    output << (concreteValue ? "true" : "false");
                }
                else if constexpr (std::is_same_v<ValueType, double>)
                {
                    if (std::isfinite(concreteValue))
                    {
                        output << std::setprecision(15) << concreteValue;
                    }
                    else
                    {
                        output << "null";
                    }
                }
                else
                {
                    output << concreteValue;
                }
            },
            value
        );
    }

    std::mt19937_64& sessionIdGenerator()
    {
        static std::mt19937_64 generator([]
        {
            std::random_device randomDevice;
            const auto wallClock = static_cast<unsigned int>(
                std::chrono::system_clock::now().time_since_epoch().count()
            );
            const auto steadyClock = static_cast<unsigned int>(
                std::chrono::steady_clock::now().time_since_epoch().count()
            );
            std::seed_seq seed{
                randomDevice(),
                randomDevice(),
                wallClock,
                steadyClock
            };
            return std::mt19937_64(seed);
        }());
        return generator;
    }

    std::mutex& sessionIdMutex()
    {
        static std::mutex mutex;
        return mutex;
    }
}

TelemetryEvent& TelemetryEvent::addString(std::string name, std::string value)
{
    fields.push_back(TelemetryField{ std::move(name), std::move(value) });
    return *this;
}

TelemetryEvent& TelemetryEvent::addInteger(std::string name, long long value)
{
    fields.push_back(TelemetryField{ std::move(name), value });
    return *this;
}

TelemetryEvent& TelemetryEvent::addDouble(std::string name, double value)
{
    fields.push_back(TelemetryField{ std::move(name), value });
    return *this;
}

TelemetryEvent& TelemetryEvent::addBoolean(std::string name, bool value)
{
    fields.push_back(TelemetryField{ std::move(name), value });
    return *this;
}

std::string generateTelemetrySessionId()
{
    std::uint64_t high = 0;
    std::uint64_t low = 0;
    {
        std::lock_guard<std::mutex> lock(sessionIdMutex());
        high = sessionIdGenerator()();
        low = sessionIdGenerator()();
    }

    // RFC 4122 variant + UUID version 4。日志 ID 不用于安全认证，但遵循通用
    // 格式后可以被日志平台、数据库和排障工具直接识别。
    high = (high & 0xffffffffffff0fffull) | 0x0000000000004000ull;
    low = (low & 0x3fffffffffffffffull) | 0x8000000000000000ull;

    std::ostringstream output;
    output << std::hex << std::setfill('0')
        << std::setw(8) << static_cast<std::uint32_t>(high >> 32) << '-'
        << std::setw(4) << static_cast<std::uint16_t>(high >> 16) << '-'
        << std::setw(4) << static_cast<std::uint16_t>(high) << '-'
        << std::setw(4) << static_cast<std::uint16_t>(low >> 48) << '-'
        << std::setw(12) << (low & 0x0000ffffffffffffull);
    return output.str();
}

std::string serializeTelemetryEvent(
    const TelemetryEvent& event,
    std::chrono::system_clock::time_point timestamp)
{
    std::ostringstream output;
    output << '{'
        << "\"schema_version\":" << kTelemetrySchemaVersion
        << ",\"timestamp\":\"" << formatUtcTimestamp(timestamp) << '"'
        << ",\"service\":\"sync_server\""
        << ",\"event\":\"" << escapeJson(event.name) << '"';

    if (!event.sessionId.empty())
    {
        output << ",\"session_id\":\""
            << escapeJson(event.sessionId) << '"';
    }
    if (event.clientId > 0)
    {
        output << ",\"client_id\":" << event.clientId;
    }
    if (event.controlEpoch > 0)
    {
        output << ",\"control_epoch\":" << event.controlEpoch;
    }

    output << ",\"attributes\":{";
    for (std::size_t index = 0; index < event.fields.size(); ++index)
    {
        if (index > 0)
        {
            output << ',';
        }
        output << '"' << escapeJson(event.fields[index].name) << "\":";
        appendJsonValue(output, event.fields[index].value);
    }
    output << "}}";
    return output.str();
}

TelemetryLogger::TelemetryLogger(
    std::ostream* consoleOutput,
    const std::string& jsonlPath)
    : consoleOutput_(consoleOutput)
{
    if (jsonlPath.empty())
    {
        return;
    }

    const std::filesystem::path path(jsonlPath);
    if (path.has_parent_path())
    {
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        if (error)
        {
            initializationError_ =
                "could not create telemetry directory: " + error.message();
            return;
        }
    }

    jsonlFile_.open(path, std::ios::out | std::ios::app);
    if (!jsonlFile_)
    {
        initializationError_ = "could not open telemetry file: " + jsonlPath;
    }
}

bool TelemetryLogger::emit(const TelemetryEvent& event)
{
    const std::string json = serializeTelemetryEvent(event);
    std::lock_guard<std::mutex> lock(mutex_);

    if (consoleOutput_ != nullptr)
    {
        // 与现有 server-side metrics 共用输出锁，保证两类结构化记录至少
        // 不会互相穿插；JSONL 文件则由本对象自己的锁保证严格单行。
        std::lock_guard<std::mutex> logLock(metricLogMutex());
        *consoleOutput_ << "[telemetry] " << json << '\n';
    }

    if (jsonlFile_.is_open())
    {
        jsonlFile_ << json << '\n';
        jsonlFile_.flush();
        return static_cast<bool>(jsonlFile_);
    }
    return initializationError_.empty();
}

bool TelemetryLogger::fileEnabled() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return jsonlFile_.is_open();
}

const std::string& TelemetryLogger::initializationError() const
{
    return initializationError_;
}

std::string TelemetryLogger::pathFromEnvironment()
{
#ifdef _WIN32
    char* path = nullptr;
    std::size_t pathLength = 0;
    if (_dupenv_s(&path, &pathLength, "SYNCCINEMA_TELEMETRY_PATH") != 0 ||
        path == nullptr)
    {
        return {};
    }

    std::string result(path);
    std::free(path);
    return result;
#else
    const char* path = std::getenv("SYNCCINEMA_TELEMETRY_PATH");
    return path == nullptr ? std::string{} : std::string(path);
#endif
}
