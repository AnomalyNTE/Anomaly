#include "anomaly/repository_network.hpp"

#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>

int main() {
    const auto root = std::filesystem::temp_directory_path() /
        (L"anomaly-network-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(root);
    const auto source = root / L"source.bin";
    std::ofstream(source, std::ios::binary) << "network-worker-fixture";
    const std::string uri = "file:///" + source.generic_string();
    anomaly::RepositoryNetworkService service;
    auto future = service.FetchToFile({uri, root / L"download.bin", 1024});
    const auto downloaded = future.get();
    bool result = downloaded.Ok() && downloaded.bytes == 22 &&
        downloaded.worker_thread_id != GetCurrentThreadId() &&
        std::filesystem::file_size(root / L"download.bin") == 22;
    auto rejected_future = service.FetchToFile({
        "http://repo.example/insecure", root / L"rejected.bin", 1024});
    const auto rejected = rejected_future.get();
    result = result &&
        rejected.error == anomaly::RepositoryNetworkError::InsecureTransport;
    const auto snapshot = service.Snapshot();
    result = result && snapshot.worker_thread_id != 0 &&
        snapshot.completed == 1 && snapshot.failed == 1;
    service.Stop();
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    if (!result) std::cerr << "network worker isolation or transfer contract failed\n";
    return result ? 0 : 2;
}
