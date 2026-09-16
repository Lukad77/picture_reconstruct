#include "PipelineConfig.h"
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>

namespace pipelineconfig {
namespace {
namespace fs = std::filesystem;
using json = nlohmann::json;

[[noreturn]] void bad(const fs::path& file, const std::string& pointer, const std::string& message) {
    throw std::runtime_error(file.string() + ": " + pointer + ": " + message);
}

void objectKeys(const json& value, const fs::path& file, const std::string& pointer,
                std::initializer_list<const char*> allowed) {
    if (!value.is_object()) bad(file, pointer, "expected object");
    std::set<std::string> keys; for (const char* key : allowed) keys.insert(key);
    for (auto it = value.begin(); it != value.end(); ++it)
        if (!keys.count(it.key())) bad(file, pointer + "/" + it.key(), "unknown field");
}

const json& required(const json& object, const char* key, const fs::path& file, const std::string& pointer) {
    auto it = object.find(key); if (it == object.end()) bad(file, pointer + "/" + key, "missing required field"); return *it;
}

uint64_t positiveU64(const json& value, const fs::path& file, const std::string& pointer) {
    if (!value.is_number_unsigned() && !value.is_number_integer()) bad(file, pointer, "expected positive integer");
    int64_t signedValue = 0;
    if (value.is_number_integer()) { signedValue = value.get<int64_t>(); if (signedValue <= 0) bad(file, pointer, "must be greater than zero"); return static_cast<uint64_t>(signedValue); }
    uint64_t result = value.get<uint64_t>(); if (!result) bad(file, pointer, "must be greater than zero"); return result;
}

int positiveInt(const json& value, const fs::path& file, const std::string& pointer) {
    uint64_t result = positiveU64(value, file, pointer); if (result > static_cast<uint64_t>(std::numeric_limits<int>::max())) bad(file, pointer, "value is too large"); return static_cast<int>(result);
}

uint64_t scaled(const json& value, uint64_t unit, const fs::path& file, const std::string& pointer) {
    uint64_t n = positiveU64(value, file, pointer); if (n > std::numeric_limits<uint64_t>::max() / unit) bad(file, pointer, "byte conversion overflows"); return n * unit;
}

std::string text(const json& value, const fs::path& file, const std::string& pointer) {
    if (!value.is_string() || value.get<std::string>().empty()) bad(file, pointer, "expected non-empty string"); return value.get<std::string>();
}

fs::path resolvedPath(const json& value, const fs::path& file, const std::string& pointer) {
    fs::path result(text(value, file, pointer)); if (result.is_relative()) result = file.parent_path() / result; return result.lexically_normal();
}

json readJson(const fs::path& file) {
    std::ifstream input(file); if (!input) bad(file, "", "cannot open configuration file");
    try { json root; input >> root; return root; }
    catch (const json::exception& e) { bad(file, "", std::string("invalid JSON: ") + e.what()); }
}

void schema(const json& root, const fs::path& file) {
    const auto& value = required(root, "schema_version", file, "");
    if (!value.is_number_integer() || value.get<int>() != 1) bad(file, "/schema_version", "unsupported schema version");
}
} // namespace

fs::path discoverConfigPath(int argc, char** argv, const char* environmentName, const fs::path& defaultPath) {
    if (argc == 3 && std::string(argv[1]) == "--config") return fs::path(argv[2]);
    if (argc != 1) throw std::runtime_error("configuration mode accepts only --config <path>");
#ifdef _WIN32
    char* value = nullptr; size_t length = 0;
    if (_dupenv_s(&value, &length, environmentName) == 0 && value && *value) {
        fs::path result(value); std::free(value); return result;
    }
    std::free(value);
#else
    if (const char* value = std::getenv(environmentName); value && *value) return fs::path(value);
#endif
    return defaultPath;
}

SenderConfig loadSenderConfig(const fs::path& inputPath) {
    fs::path file = fs::absolute(inputPath).lexically_normal(); json root = readJson(file);
    objectKeys(root, file, "", {"schema_version", "task_id", "network", "capture", "spool", "pipeline"}); schema(root, file);
    SenderConfig out; out.taskId = positiveU64(required(root, "task_id", file, ""), file, "/task_id");
    const auto& network = required(root, "network", file, ""); objectKeys(network, file, "/network", {"host", "port", "reconnect_delay_ms"});
    out.host = text(required(network, "host", file, "/network"), file, "/network/host");
    uint64_t port = positiveU64(required(network, "port", file, "/network"), file, "/network/port"); if (port > 65535) bad(file, "/network/port", "must be at most 65535"); out.port = static_cast<uint16_t>(port);
    out.transfer.reconnectDelay = std::chrono::milliseconds(positiveU64(required(network, "reconnect_delay_ms", file, "/network"), file, "/network/reconnect_delay_ms"));

    const auto& capture = required(root, "capture", file, ""); objectKeys(capture, file, "/capture", {"mode", "camera_index", "width", "height", "fps", "warmup_frames", "scan_csv"});
    out.captureMode = text(required(capture, "mode", file, "/capture"), file, "/capture/mode"); if (out.captureMode != "synthetic" && out.captureMode != "v4l2") bad(file, "/capture/mode", "must be synthetic or v4l2");
    const auto& camera = required(capture, "camera_index", file, "/capture"); if (!camera.is_number_integer()) bad(file, "/capture/camera_index", "expected integer"); out.cameraIndex = camera.get<int>();
    if (out.captureMode == "v4l2" && out.cameraIndex < 0) bad(file, "/capture/camera_index", "must be non-negative in v4l2 mode");
    out.width = positiveInt(required(capture, "width", file, "/capture"), file, "/capture/width");
    out.height = positiveInt(required(capture, "height", file, "/capture"), file, "/capture/height");
    out.fps = positiveInt(required(capture, "fps", file, "/capture"), file, "/capture/fps");
    const auto& warmup = required(capture, "warmup_frames", file, "/capture"); if (!warmup.is_number_integer() || warmup.get<int>() < 0) bad(file, "/capture/warmup_frames", "must be a non-negative integer"); out.warmupFrames = warmup.get<int>();
    out.scanCsv = resolvedPath(required(capture, "scan_csv", file, "/capture"), file, "/capture/scan_csv"); if (!fs::is_regular_file(out.scanCsv)) bad(file, "/capture/scan_csv", "file does not exist");

    const auto& spool = required(root, "spool", file, ""); objectKeys(spool, file, "/spool", {"directory", "max_size_gib"});
    out.spoolDirectory = resolvedPath(required(spool, "directory", file, "/spool"), file, "/spool/directory");
    out.transfer.maxSpoolBytes = scaled(required(spool, "max_size_gib", file, "/spool"), v2transfer::kGiB, file, "/spool/max_size_gib");
    const auto& pipeline = required(root, "pipeline", file, ""); objectKeys(pipeline, file, "/pipeline", {"max_in_flight_frames", "max_queue_mib"});
    out.transfer.maxInFlightFrames = positiveInt(required(pipeline, "max_in_flight_frames", file, "/pipeline"), file, "/pipeline/max_in_flight_frames");
    out.transfer.maxQueuedBytes = scaled(required(pipeline, "max_queue_mib", file, "/pipeline"), v2transfer::kMiB, file, "/pipeline/max_queue_mib");
    return out;
}

ReceiverConfig loadReceiverConfig(const fs::path& inputPath) {
    fs::path file = fs::absolute(inputPath).lexically_normal(); json root = readJson(file);
    objectKeys(root, file, "", {"schema_version", "task_id", "network", "spool", "pipeline", "reconstruction"}); schema(root, file);
    ReceiverConfig out; out.taskId = positiveU64(required(root, "task_id", file, ""), file, "/task_id"); out.transfer.expectedTaskId = out.taskId;
    const auto& network = required(root, "network", file, ""); objectKeys(network, file, "/network", {"port"});
    uint64_t port = positiveU64(required(network, "port", file, "/network"), file, "/network/port"); if (port > 65535) bad(file, "/network/port", "must be at most 65535"); out.port = static_cast<uint16_t>(port);
    const auto& spool = required(root, "spool", file, ""); objectKeys(spool, file, "/spool", {"directory", "max_size_gib", "cleanup_after_success"});
    out.spoolDirectory = resolvedPath(required(spool, "directory", file, "/spool"), file, "/spool/directory");
    out.transfer.maxSpoolBytes = scaled(required(spool, "max_size_gib", file, "/spool"), v2transfer::kGiB, file, "/spool/max_size_gib");
    const auto& cleanup = required(spool, "cleanup_after_success", file, "/spool"); if (!cleanup.is_boolean()) bad(file, "/spool/cleanup_after_success", "expected boolean"); out.transfer.cleanupCompletedTask = cleanup.get<bool>();
    const auto& pipeline = required(root, "pipeline", file, ""); objectKeys(pipeline, file, "/pipeline", {"max_queue_mib"});
    out.transfer.maxQueuedBytes = scaled(required(pipeline, "max_queue_mib", file, "/pipeline"), v2transfer::kMiB, file, "/pipeline/max_queue_mib");
    const auto& reconstruction = required(root, "reconstruction", file, ""); objectKeys(reconstruction, file, "/reconstruction", {"spots_csv", "output_directory", "output_width", "output_height"});
    out.spotsCsv = resolvedPath(required(reconstruction, "spots_csv", file, "/reconstruction"), file, "/reconstruction/spots_csv"); if (!fs::is_regular_file(out.spotsCsv)) bad(file, "/reconstruction/spots_csv", "file does not exist");
    out.outputDirectory = resolvedPath(required(reconstruction, "output_directory", file, "/reconstruction"), file, "/reconstruction/output_directory");
    out.outputWidth = positiveInt(required(reconstruction, "output_width", file, "/reconstruction"), file, "/reconstruction/output_width");
    out.outputHeight = positiveInt(required(reconstruction, "output_height", file, "/reconstruction"), file, "/reconstruction/output_height");
    return out;
}
} // namespace pipelineconfig
