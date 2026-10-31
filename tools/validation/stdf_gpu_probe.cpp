// Isolated developer probe; no production adapter, app integration or packaging.
// Uses the existing pinned ORT/CUDA DLLs without linking to application code.
#define ORT_API_MANUAL_INIT
#if defined(__MINGW32__) && !defined(_stdcall)
#define _stdcall __stdcall
#endif
#include <onnxruntime_cxx_api.h>
#include <nlohmann/json.hpp>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>

using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;

template<class T> T proc(HMODULE module, const char* name) {
    auto value = GetProcAddress(module, name);
    if (!value) throw std::runtime_error(std::string("Missing DLL symbol: ") + name);
    return std::bit_cast<T>(value);
}

struct Runtime {
    HMODULE ort{};
    std::vector<HMODULE> dependencies;
    DLL_DIRECTORY_COOKIE cookie{};
    const OrtApi* api{};
    using MemoryInfo = int (*)(std::size_t*, std::size_t*);
    MemoryInfo memoryInfo{};
    explicit Runtime(const std::filesystem::path& path) {
        cookie = AddDllDirectory(std::filesystem::canonical(path).c_str());
        if (!cookie) throw std::runtime_error("AddDllDirectory failed");
        constexpr const wchar_t* names[]{L"cudart64_12.dll", L"cublasLt64_12.dll", L"cublas64_12.dll", L"cufft64_11.dll",
            L"cudnn_graph64_9.dll", L"cudnn_ops64_9.dll", L"cudnn_cnn64_9.dll", L"cudnn_adv64_9.dll",
            L"cudnn_engines_precompiled64_9.dll", L"cudnn_engines_runtime_compiled64_9.dll", L"cudnn_heuristic64_9.dll",
            L"cudnn64_9.dll", L"onnxruntime_providers_shared.dll"};
        for (const auto* name : names) {
            auto loaded = LoadLibraryExW((path / name).c_str(), nullptr,
                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (!loaded) throw std::runtime_error("Could not load pinned CUDA dependency; Windows " + std::to_string(GetLastError()));
            dependencies.push_back(loaded);
        }
        memoryInfo = proc<MemoryInfo>(dependencies.front(), "cudaMemGetInfo");
        ort = LoadLibraryExW((path / L"onnxruntime.dll").c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!ort) throw std::runtime_error("Could not load pinned ONNX Runtime DLL");
        using GetApi = const OrtApiBase* (ORT_API_CALL*)() noexcept;
        const auto* base = proc<GetApi>(ort, "OrtGetApiBase")();
        if (std::string(base->GetVersionString()) != "1.22.0") throw std::runtime_error("Expected ORT 1.22.0");
        api = base->GetApi(ORT_API_VERSION);
        if (!api) throw std::runtime_error("ORT API incompatible");
        Ort::InitApi(api);
    }
    ~Runtime() {
        if (ort) FreeLibrary(ort);
        for (auto it = dependencies.rbegin(); it != dependencies.rend(); ++it) FreeLibrary(*it);
        if (cookie) RemoveDllDirectory(cookie);
    }
    Json memory() const {
        std::size_t free = 0, total = 0;
        const int result = memoryInfo(&free, &total);
        return {{"cuda_error", result}, {"free_bytes", free}, {"total_bytes", total}, {"used_bytes", total - free},
                {"scope", "device-wide snapshot; not per-process peak"}};
    }
};

std::vector<float> readInput(const std::filesystem::path& path, std::size_t count) {
    if (std::filesystem::file_size(path) != count * sizeof(float)) throw std::runtime_error("Input byte count mismatches geometry");
    std::vector<float> data(count);
    std::ifstream input(path, std::ios::binary);
    if (!input.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(count * sizeof(float))))
        throw std::runtime_error("Could not read tensor fixture");
    return data;
}

int main(int argc, char** argv) {
    Json report{{"schemaVersion", 1}, {"status", "not_started"}};
    std::filesystem::path reportPath;
    bool mayWriteReport = false;
    try {
        if (argc != 11) throw std::runtime_error("usage: probe runtime graph input h w output report mode repeats arenaBytes");
        const std::filesystem::path runtimePath = std::filesystem::absolute(argv[1]);
        const std::filesystem::path graph = std::filesystem::absolute(argv[2]);
        const std::filesystem::path outputPath = std::filesystem::absolute(argv[6]);
        reportPath = std::filesystem::absolute(argv[7]);
        const std::string mode = argv[8];
        const int height = std::stoi(argv[4]), width = std::stoi(argv[5]), repeats = std::stoi(argv[9]);
        if (height <= 0 || width <= 0 || height > 2048 || width > 2048 || repeats < 1 || repeats > 20)
            throw std::runtime_error("Out-of-range developer-probe geometry/repeats");
        if (std::filesystem::exists(outputPath) || std::filesystem::exists(reportPath))
            throw std::runtime_error("Refusing to overwrite existing tensor/evidence");
        mayWriteReport = true;
        Runtime runtime(runtimePath);
        report["mode"] = mode;
        report["runtime"] = runtimePath.string();
        report["runtime_version"] = "1.22.0";
        report["input_shape"] = {1, 7, height, width};
        report["memory_before_session"] = runtime.memory();
        Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "rifelove-stdf-gpu-probe");
        Ort::SessionOptions options;
        options.SetIntraOpNumThreads(2).SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        const auto profilePrefix = reportPath.parent_path() / (reportPath.stem().wstring() + L"-profile");
        options.EnableProfiling(profilePrefix.c_str());
        if (mode == "strict-cuda") options.AddConfigEntry("session.disable_cpu_ep_fallback", "1");
        if (mode != "cpu") {
            OrtCUDAProviderOptionsV2* cuda = nullptr;
            Ort::ThrowOnError(runtime.api->CreateCUDAProviderOptions(&cuda));
            try {
                const char* keys[]{"device_id", "gpu_mem_limit", "arena_extend_strategy", "cudnn_conv_algo_search",
                    "cudnn_conv_use_max_workspace", "use_tf32", "do_copy_in_default_stream"};
                const char* values[]{mode == "invalid-device" ? "99" : "0", argv[10], "kSameAsRequested", "DEFAULT", "0", "0", "1"};
                Ort::ThrowOnError(runtime.api->UpdateCUDAProviderOptions(cuda, keys, values, std::size(keys)));
                options.AppendExecutionProvider_CUDA_V2(*cuda);
                runtime.api->ReleaseCUDAProviderOptions(cuda);
            } catch (...) {
                runtime.api->ReleaseCUDAProviderOptions(cuda);
                throw;
            }
        }
        const auto sessionBegin = Clock::now();
        Ort::Session session(env, graph.c_str(), options);
        report["session_setup_seconds"] = std::chrono::duration<double>(Clock::now() - sessionBegin).count();
        report["memory_after_session"] = runtime.memory();
        auto data = readInput(argv[3], static_cast<std::size_t>(7) * height * width);
        const std::array<std::int64_t, 4> shape{1, 7, height, width};
        auto cpu = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        auto tensor = Ort::Value::CreateTensor<float>(cpu, data.data(), data.size(), shape.data(), shape.size());
        const char* inputNames[]{"luma_frames"}; const char* outputNames[]{"restored_luma"};
        Ort::RunOptions run;
        std::vector<Ort::Value> result;
        report["run_seconds"] = Json::array();
        for (int repeat = 0; repeat < repeats; ++repeat) {
            const auto begin = Clock::now();
            result = session.Run(run, inputNames, &tensor, 1, outputNames, 1);
            report["run_seconds"].push_back(std::chrono::duration<double>(Clock::now() - begin).count());
        }
        report["memory_after_runs"] = runtime.memory();
        if (mode == "cancel") {
            Json cancellation;
            run.SetTerminate();
            try {
                auto ignored = session.Run(run, inputNames, &tensor, 1, outputNames, 1);
                cancellation["pre_cancel_rejected"] = false;
            } catch (const Ort::Exception& error) {
                cancellation["pre_cancel_rejected"] = true;
                cancellation["pre_cancel_error"] = error.what();
            }
            run.UnsetTerminate();
            std::exception_ptr cancellationError;
            std::atomic_bool requested = false;
            const auto begin = Clock::now();
            std::jthread cancel([&] {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                requested.store(true);
                try { run.SetTerminate(); } catch (...) { cancellationError = std::current_exception(); }
            });
            try {
                auto ignored = session.Run(run, inputNames, &tensor, 1, outputNames, 1);
                cancellation["in_flight_cancel_rejected"] = false;
                cancellation["request_observed_before_return"] = requested.load();
                // ORT may finish kernels already enqueued. Drop this result;
                // only the separate recovery run below may create an output.
                cancellation["in_flight_result_discarded"] = true;
            } catch (const Ort::Exception& error) {
                cancellation["in_flight_cancel_rejected"] = true;
                cancellation["request_observed_before_return"] = requested.load();
                cancellation["in_flight_result_discarded"] = true;
                cancellation["in_flight_error"] = error.what();
            }
            cancel.join();
            if (cancellationError) std::rethrow_exception(cancellationError);
            cancellation["in_flight_elapsed_seconds"] = std::chrono::duration<double>(Clock::now() - begin).count();
            cancellation["cancelled_result_published"] = std::filesystem::exists(outputPath);
            cancellation["request_delay_ms"] = 2;
            run.UnsetTerminate();
            result = session.Run(run, inputNames, &tensor, 1, outputNames, 1);
            cancellation["subsequent_valid_run_succeeded"] = true;
            cancellation["written_output_is_separate_recovery_run"] = true;
            report["cancellation"] = std::move(cancellation);
        }
        const auto info = result.at(0).GetTensorTypeAndShapeInfo();
        report["output_shape"] = info.GetShape();
        const auto* values = result.at(0).GetTensorData<float>();
        const auto count = info.GetElementCount();
        if (info.GetShape() != std::vector<std::int64_t>{1, 1, height, width} ||
            !std::all_of(values, values + count, [](float value){ return std::isfinite(value); }))
            throw std::runtime_error("Output shape or finiteness failed");
        Ort::AllocatorWithDefaultOptions allocator;
        auto profile = session.EndProfilingAllocated(allocator);
        report["profile_file"] = profile.get();
        std::ofstream output(outputPath, std::ios::binary);
        output.write(reinterpret_cast<const char*>(values), static_cast<std::streamsize>(count * sizeof(float)));
        if (!output) throw std::runtime_error("Could not write tensor result");
        report["memory_after_output"] = runtime.memory();
        report["status"] = "completed_probe_not_product_qualification";
    } catch (const Ort::Exception& error) {
        report["status"] = "ort_error"; report["error"] = error.what(); report["ort_error_code"] = error.GetOrtErrorCode();
    } catch (const std::exception& error) {
        report["status"] = "probe_error"; report["error"] = error.what();
    }
    if (!reportPath.empty() && mayWriteReport) {
        std::ofstream output(reportPath);
        output << report.dump(2) << '\n';
    }
    std::cout << report.dump(2) << '\n';
    return report["status"] == "completed_probe_not_product_qualification" ? 0 : 1;
}
