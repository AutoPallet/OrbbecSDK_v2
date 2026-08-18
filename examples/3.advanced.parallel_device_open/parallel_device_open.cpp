// Copyright (c) Orbbec Inc. All Rights Reserved.
// Licensed under the MIT License.

#include <libobsensor/ObSensor.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Options {
    enum class Mode {
        All,
        Index,
        Address,
        DefaultPipeline,
        AccessMismatch,
    };

    Mode        mode = Mode::All;
    uint32_t    index = 0;
    std::string address;
    uint16_t    port                      = 8090;
    size_t      copies                    = 1;
    bool        disableNetworkEnumeration = false;
};

class StartGate {
public:
    void arriveAndWait() {
        std::unique_lock<std::mutex> lock(mutex_);
        ++ready_;
        readyCv_.notify_one();
        startCv_.wait(lock, [this]() { return started_; });
    }

    void startWhenReady(size_t workerCount) {
        std::unique_lock<std::mutex> lock(mutex_);
        readyCv_.wait(lock, [this, workerCount]() { return ready_ == workerCount; });
        started_ = true;
        startCv_.notify_all();
    }

    void cancel() {
        std::lock_guard<std::mutex> lock(mutex_);
        started_ = true;
        startCv_.notify_all();
    }

private:
    std::mutex              mutex_;
    std::condition_variable readyCv_;
    std::condition_variable startCv_;
    size_t                  ready_   = 0;
    bool                    started_ = false;
};

struct DeviceOpenResult {
    ob_device                    *device = nullptr;
    std::string                   error;
    std::chrono::steady_clock::duration elapsed{};
};

struct PipelineOpenResult {
    ob_pipeline                  *pipeline = nullptr;
    std::string                   error;
    std::chrono::steady_clock::duration elapsed{};
};

std::string takeError(ob_error *&error) {
    if(!error) {
        return {};
    }
    const auto message = std::string(ob_error_get_message(error));
    ob_delete_error(error);
    error = nullptr;
    return message;
}

void reportCleanupError(const char *operation, ob_error *&error) {
    if(error) {
        std::cerr << operation << " failed during cleanup: " << ob_error_get_message(error) << std::endl;
        ob_delete_error(error);
        error = nullptr;
    }
}

void deleteDevice(ob_device *device) {
    if(!device) {
        return;
    }
    ob_error *error = nullptr;
    ob_delete_device(device, &error);
    reportCleanupError("ob_delete_device", error);
}

void deletePipeline(ob_pipeline *pipeline) {
    if(!pipeline) {
        return;
    }
    ob_error *error = nullptr;
    ob_delete_pipeline(pipeline, &error);
    reportCleanupError("ob_delete_pipeline", error);
}

void deleteDeviceList(ob_device_list *list) {
    if(!list) {
        return;
    }
    ob_error *error = nullptr;
    ob_delete_device_list(list, &error);
    reportCleanupError("ob_delete_device_list", error);
}

void deleteContext(ob_context *context) {
    if(!context) {
        return;
    }
    ob_error *error = nullptr;
    ob_delete_context(context, &error);
    reportCleanupError("ob_delete_context", error);
}

size_t parseSize(const char *flag, const char *value) {
    char         *end    = nullptr;
    const auto    parsed = std::strtoull(value, &end, 10);
    const bool    valid  = end != value && *end == '\0' && parsed > 0;
    if(!valid) {
        throw std::invalid_argument(std::string(flag) + " requires a positive integer");
    }
    return static_cast<size_t>(parsed);
}

uint32_t parseIndexAllowZero(const char *flag, const char *value) {
    char       *end    = nullptr;
    const auto  parsed = std::strtoull(value, &end, 10);
    const bool  valid  = end != value && *end == '\0' && parsed <= UINT32_MAX;
    if(!valid) {
        throw std::invalid_argument(std::string(flag) + " requires a uint32 integer");
    }
    return static_cast<uint32_t>(parsed);
}

Options parseOptions(int argc, char **argv) {
    Options options;
    bool    modeSet = false;

    for(int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if(arg == "--all") {
            options.mode = Options::Mode::All;
            modeSet      = true;
        }
        else if(arg == "--index" && i + 1 < argc) {
            options.mode  = Options::Mode::Index;
            options.index = parseIndexAllowZero("--index", argv[++i]);
            modeSet       = true;
        }
        else if(arg == "--address" && i + 1 < argc) {
            options.mode    = Options::Mode::Address;
            options.address = argv[++i];
            modeSet         = true;
        }
        else if(arg == "--port" && i + 1 < argc) {
            const auto port = parseSize("--port", argv[++i]);
            if(port > UINT16_MAX) {
                throw std::invalid_argument("--port exceeds the uint16 range");
            }
            options.port = static_cast<uint16_t>(port);
        }
        else if(arg == "--copies" && i + 1 < argc) {
            options.copies = parseSize("--copies", argv[++i]);
        }
        else if(arg == "--disable-network-enumeration") {
            options.disableNetworkEnumeration = true;
        }
        else if(arg == "--default-pipeline") {
            options.mode = Options::Mode::DefaultPipeline;
            modeSet      = true;
        }
        else if(arg == "--access-mismatch" && i + 1 < argc) {
            options.mode  = Options::Mode::AccessMismatch;
            options.index = parseIndexAllowZero("--access-mismatch", argv[++i]);
            modeSet       = true;
        }
        else {
            throw std::invalid_argument("unknown or incomplete argument: " + arg);
        }
    }

    if(!modeSet) {
        throw std::invalid_argument("select --all, --index, --address, --default-pipeline, or --access-mismatch");
    }
    if(options.mode == Options::Mode::Address && options.address.empty()) {
        throw std::invalid_argument("--address requires an IP address");
    }
    if(options.disableNetworkEnumeration && options.mode != Options::Mode::Address) {
        throw std::invalid_argument("--disable-network-enumeration requires --address");
    }
    return options;
}

template <typename Function> void runWorkers(size_t count, Function function) {
    StartGate                gate;
    std::vector<std::thread> workers;
    workers.reserve(count);
    try {
        for(size_t worker = 0; worker < count; ++worker) {
            workers.emplace_back([worker, &gate, &function]() {
                gate.arriveAndWait();
                function(worker);
            });
        }
        gate.startWhenReady(count);
    }
    catch(...) {
        gate.cancel();
        for(auto &worker: workers) {
            worker.join();
        }
        throw;
    }
    for(auto &worker: workers) {
        worker.join();
    }
}

std::string deviceIdentity(ob_device *device) {
    ob_error       *error = nullptr;
    ob_device_info *info  = ob_device_get_device_info(device, &error);
    auto            failure = takeError(error);
    if(!failure.empty()) {
        return "device-info error: " + failure;
    }

    const char *uid = ob_device_info_get_uid(info, &error);
    failure         = takeError(error);
    if(!failure.empty()) {
        ob_delete_device_info(info, &error);
        reportCleanupError("ob_delete_device_info", error);
        return "UID error: " + failure;
    }
    const std::string uidCopy(uid);

    const char *serial = ob_device_info_get_serial_number(info, &error);
    failure            = takeError(error);
    const std::string serialCopy = failure.empty() ? std::string(serial) : "serial error: " + failure;

    ob_delete_device_info(info, &error);
    reportCleanupError("ob_delete_device_info", error);
    return "UID=" + uidCopy + " serial=" + serialCopy;
}

long long elapsedMilliseconds(std::chrono::steady_clock::duration elapsed) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
}

int reportDeviceResults(std::vector<DeviceOpenResult> &results, std::chrono::steady_clock::duration wallTime) {
    bool failed = false;
    for(size_t i = 0; i < results.size(); ++i) {
        auto &result = results[i];
        if(!result.error.empty()) {
            std::cerr << "worker " << i << " failed after " << elapsedMilliseconds(result.elapsed) << " ms: " << result.error << std::endl;
            failed = true;
        }
        else {
            std::cout << "worker " << i << " opened " << deviceIdentity(result.device) << " in " << elapsedMilliseconds(result.elapsed) << " ms"
                      << std::endl;
        }
    }
    std::cout << "total wall time: " << elapsedMilliseconds(wallTime) << " ms" << std::endl;
    for(auto &result: results) {
        deleteDevice(result.device);
    }
    return failed ? 1 : 0;
}

int runDeviceOpens(size_t count, const std::function<ob_device *(size_t, ob_error **)> &open) {
    std::vector<DeviceOpenResult> results(count);
    const auto                    wallStarted = std::chrono::steady_clock::now();
    runWorkers(count, [&](size_t worker) {
        const auto started = std::chrono::steady_clock::now();
        try {
            ob_error *error       = nullptr;
            results[worker].device = open(worker, &error);
            results[worker].error  = takeError(error);
        }
        catch(const std::exception &error) {
            results[worker].error = error.what();
        }
        catch(...) {
            results[worker].error = "unknown C++ exception";
        }
        results[worker].elapsed = std::chrono::steady_clock::now() - started;
    });
    return reportDeviceResults(results, std::chrono::steady_clock::now() - wallStarted);
}

int runPipelines(size_t copies) {
    std::vector<PipelineOpenResult> results(copies);
    const auto                      wallStarted = std::chrono::steady_clock::now();
    runWorkers(copies, [&](size_t worker) {
        const auto started = std::chrono::steady_clock::now();
        try {
            ob_error *error         = nullptr;
            results[worker].pipeline = ob_create_pipeline(&error);
            results[worker].error    = takeError(error);
        }
        catch(const std::exception &error) {
            results[worker].error = error.what();
        }
        catch(...) {
            results[worker].error = "unknown C++ exception";
        }
        results[worker].elapsed = std::chrono::steady_clock::now() - started;
    });

    bool failed = false;
    for(size_t i = 0; i < results.size(); ++i) {
        auto &result = results[i];
        if(!result.error.empty()) {
            std::cerr << "worker " << i << " failed after " << elapsedMilliseconds(result.elapsed) << " ms: " << result.error << std::endl;
            failed = true;
            continue;
        }
        ob_error  *error  = nullptr;
        ob_device *device = ob_pipeline_get_device(result.pipeline, &error);
        auto       failure = takeError(error);
        if(!failure.empty()) {
            std::cerr << "worker " << i << " pipeline device lookup failed: " << failure << std::endl;
            failed = true;
        }
        else {
            std::cout << "worker " << i << " created pipeline for " << deviceIdentity(device) << " in " << elapsedMilliseconds(result.elapsed) << " ms"
                      << std::endl;
            deleteDevice(device);
        }
    }
    std::cout << "total wall time: " << elapsedMilliseconds(std::chrono::steady_clock::now() - wallStarted) << " ms" << std::endl;
    for(auto &result: results) {
        deletePipeline(result.pipeline);
    }
    return failed ? 1 : 0;
}

int runAccessMismatch(ob_device_list *list, uint32_t index) {
    ob_error  *error  = nullptr;
    ob_device *device = ob_device_list_get_device_ex(list, index, OB_DEVICE_CONTROL_ACCESS, &error);
    auto       failure = takeError(error);
    if(!failure.empty()) {
        std::cerr << "control-access open failed: " << failure << std::endl;
        return 1;
    }

    ob_device *mismatch = ob_device_list_get_device_ex(list, index, OB_DEVICE_MONITOR_ACCESS, &error);
    if(!error) {
        std::cout << "SKIP: selected device accepted mismatched access modes; it does not enforce access control" << std::endl;
        deleteDevice(mismatch);
        deleteDevice(device);
        return 0;
    }

    const auto exceptionType = ob_error_get_exception_type(error);
    failure                  = takeError(error);
    deleteDevice(mismatch);
    deleteDevice(device);
    if(exceptionType != OB_EXCEPTION_TYPE_ACCESS_DENIED) {
        std::cerr << "mismatched access mode returned the wrong error: " << failure << std::endl;
        return 1;
    }
    std::cout << "mismatched access mode returned access denied: " << failure << std::endl;
    return 0;
}

void printUsage(const char *program) {
    std::cerr << "usage:\n"
              << "  " << program << " --all\n"
              << "  " << program << " --index INDEX --copies N\n"
              << "  " << program << " --address IP [--port PORT] --copies N [--disable-network-enumeration]\n"
              << "  " << program << " --default-pipeline --copies N\n"
              << "  " << program << " --access-mismatch INDEX" << std::endl;
}

}  // namespace

int main(int argc, char **argv) {
    Options options;
    try {
        options = parseOptions(argc, argv);
    }
    catch(const std::exception &error) {
        std::cerr << error.what() << std::endl;
        printUsage(argv[0]);
        return 2;
    }

    ob_error   *error   = nullptr;
    ob_context *context = ob_create_context(&error);
    auto        failure = takeError(error);
    if(!failure.empty()) {
        std::cerr << "context creation failed: " << failure << std::endl;
        return 1;
    }

    if(options.disableNetworkEnumeration) {
        ob_enable_net_device_enumeration(context, false, &error);
        failure = takeError(error);
        if(!failure.empty()) {
            std::cerr << "disabling network enumeration failed: " << failure << std::endl;
            deleteContext(context);
            return 1;
        }
    }

    int result = 1;
    if(options.mode == Options::Mode::Address) {
        result = runDeviceOpens(options.copies, [&](size_t, ob_error **workerError) {
            return ob_create_net_device(context, options.address.c_str(), options.port, workerError);
        });
    }
    else if(options.mode == Options::Mode::DefaultPipeline) {
        result = runPipelines(options.copies);
    }
    else {
        ob_device_list *list = ob_query_device_list(context, &error);
        failure              = takeError(error);
        if(!failure.empty()) {
            std::cerr << "device enumeration failed: " << failure << std::endl;
            deleteContext(context);
            return 1;
        }

        uint32_t count = ob_device_list_get_count(list, &error);
        failure        = takeError(error);
        if(!failure.empty()) {
            std::cerr << "device count failed: " << failure << std::endl;
            deleteDeviceList(list);
            deleteContext(context);
            return 1;
        }

        if(options.mode == Options::Mode::All) {
            if(count == 0) {
                std::cerr << "no devices found" << std::endl;
                result = 1;
            }
            else {
                result = runDeviceOpens(count, [&](size_t worker, ob_error **workerError) {
                    return ob_device_list_get_device(list, static_cast<uint32_t>(worker), workerError);
                });
            }
        }
        else if(options.index >= count) {
            std::cerr << "device index " << options.index << " is outside the enumerated count " << count << std::endl;
            result = 2;
        }
        else if(options.mode == Options::Mode::Index) {
            result = runDeviceOpens(options.copies, [&](size_t, ob_error **workerError) {
                return ob_device_list_get_device(list, options.index, workerError);
            });
        }
        else {
            result = runAccessMismatch(list, options.index);
        }
        deleteDeviceList(list);
    }

    deleteContext(context);
    return result;
}
