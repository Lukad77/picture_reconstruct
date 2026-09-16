#include "PipelineConfig.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstdlib>

int main() {
    namespace fs = std::filesystem;
    const fs::path source = fs::path(__FILE__).parent_path().parent_path();
    bool ok = true;
    try {
        auto sender = pipelineconfig::loadSenderConfig(source / "config" / "sender.json");
        auto receiver = pipelineconfig::loadReceiverConfig(source / "config" / "receiver.json");
        ok = sender.host == "127.0.0.1" && sender.port == 19090 && sender.taskId == 1 &&
             sender.transfer.maxInFlightFrames == 4 && sender.scanCsv.is_absolute() &&
             receiver.port == 19090 && receiver.transfer.expectedTaskId == 1 &&
             receiver.transfer.cleanupCompletedTask && receiver.spotsCsv.is_absolute();

        const fs::path invalid = fs::temp_directory_path() / "picture_reconstruct_invalid_config.json";
        { std::ofstream out(invalid); out << "{\"schema_version\":1,\"unknown\":true}"; }
        bool rejected = false;
        try { (void)pipelineconfig::loadSenderConfig(invalid); }
        catch (const std::exception& e) { rejected = std::string(e.what()).find("/unknown") != std::string::npos; }
        fs::remove(invalid); ok = ok && rejected;

        char program[] = "test";
        char configFlag[] = "--config";
        char explicitPath[] = "explicit.json";
        char* defaultArgs[] = {program};
        char* explicitArgs[] = {program, configFlag, explicitPath};
#ifdef _WIN32
        _putenv_s("PICTURE_RECONSTRUCT_TEST_CONFIG", "environment.json");
#else
        setenv("PICTURE_RECONSTRUCT_TEST_CONFIG", "environment.json", 1);
#endif
        ok = ok && pipelineconfig::discoverConfigPath(1, defaultArgs, "PICTURE_RECONSTRUCT_TEST_CONFIG", "default.json") == "environment.json";
        ok = ok && pipelineconfig::discoverConfigPath(3, explicitArgs, "PICTURE_RECONSTRUCT_TEST_CONFIG", "default.json") == "explicit.json";
#ifdef _WIN32
        _putenv_s("PICTURE_RECONSTRUCT_TEST_CONFIG", "");
#else
        unsetenv("PICTURE_RECONSTRUCT_TEST_CONFIG");
#endif
        ok = ok && pipelineconfig::discoverConfigPath(1, defaultArgs, "PICTURE_RECONSTRUCT_TEST_CONFIG", "default.json") == "default.json";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n'; return 1;
    }
    if (!ok) { std::cerr << "pipeline config test failed\n"; return 1; }
    std::cout << "pipeline config loading and strict validation OK\n";
    return 0;
}
