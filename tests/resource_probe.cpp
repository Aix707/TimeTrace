#include <windows.h>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <psapi.h>
#include <string>
#include <thread>
static double elapsedClock() {
    ULONGLONG t{};
    QueryUnbiasedInterruptTime(&t);
    return t / 10000000.0;
}
static double cpu(HANDLE h) {
    FILETIME c{}, e{}, k{}, u{};
    GetProcessTimes(h, &c, &e, &k, &u);
    auto value = [](FILETIME f) {
        return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
    };
    return (value(k) + value(u)) / 10000000.0;
}
static bool uiVisible(DWORD pid) {
    struct Find {
        DWORD pid;
        bool visible;
    };
    Find f{pid, false};
    EnumWindows(
        [](HWND h, LPARAM l) -> BOOL {
            auto f = reinterpret_cast<Find *>(l);
            DWORD p{};
            GetWindowThreadProcessId(h, &p);
            wchar_t cls[80]{};
            GetClassNameW(h, cls, 80);
            if (p == f->pid && wcscmp(cls, L"TimeTrace.Window") == 0 && IsWindowVisible(h)) {
                f->visible = true;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&f));
    return f.visible;
}
int wmain(int argc, wchar_t **argv) {
    if (argc < 5) {
        std::cerr << "resource_probe PID seconds warmup output-prefix\n";
        return 2;
    }
    DWORD pid = wcstoul(argv[1], nullptr, 10);
    int seconds = _wtoi(argv[2]), warmup = _wtoi(argv[3]);
    std::wstring prefix = argv[4];
    HANDLE process =
        OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | SYNCHRONIZE, FALSE, pid);
    if (!process)
        return 3;
    std::ofstream csv(prefix + L".csv");
    csv << "elapsed_seconds,cpu_percent,private_working_set_bytes,working_set_bytes,private_commit_"
           "bytes,ui_visible\n";
    double initialCpu{}, initialTime{}, peak{}, peakCommit{}, lastPrivate{}, firstPrivate{},
        start = elapsedClock();
    unsigned samples{};
    bool exited = false, opened = false;
    for (;;) {
        double elapsed = elapsedClock() - start;
        if (elapsed >= warmup + seconds)
            break;
        if (WaitForSingleObject(process, 0) != WAIT_TIMEOUT) {
            exited = true;
            break;
        }
        PROCESS_MEMORY_COUNTERS_EX2 memory{};
        memory.cb = sizeof(memory);
        if (!GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&memory),
                                  sizeof(memory))) {
            CloseHandle(process);
            return 4;
        }
        if (elapsed >= warmup) {
            bool visible = uiVisible(pid);
            opened = opened || visible;
            if (!samples) {
                initialCpu = cpu(process);
                initialTime = elapsed;
                firstPrivate = static_cast<double>(memory.PrivateWorkingSetSize);
            }
            lastPrivate = static_cast<double>(memory.PrivateWorkingSetSize);
            peak = std::max(peak, lastPrivate);
            peakCommit = std::max(peakCommit, static_cast<double>(memory.PrivateUsage));
            double duration = elapsed - initialTime,
                   percent = duration > 0 ? (cpu(process) - initialCpu) / duration /
                                                GetActiveProcessorCount(ALL_PROCESSOR_GROUPS) * 100
                                          : 0;
            csv << std::fixed << std::setprecision(4) << duration << "," << percent << ","
                << memory.PrivateWorkingSetSize << "," << memory.WorkingSetSize << ","
                << memory.PrivateUsage << "," << visible << "\n";
            csv.flush();
            ++samples;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    double duration = elapsedClock() - start - initialTime;
    double average = duration > 0 ? (cpu(process) - initialCpu) / duration /
                                        GetActiveProcessorCount(ALL_PROCESSOR_GROUPS) * 100
                                  : 0;
    std::ofstream summary(prefix + L".json");
    summary << std::fixed << std::setprecision(6) << "{\n  \"pid\": " << pid
            << ",\n  \"measured_seconds\": " << duration << ",\n  \"warmup_seconds\": " << warmup
            << ",\n  \"samples\": " << samples << ",\n  \"average_cpu_percent\": " << average
            << ",\n  \"peak_private_working_set_bytes\": " << peak
            << ",\n  \"peak_private_commit_bytes\": " << peakCommit
            << ",\n  \"private_working_set_growth_bytes\": " << lastPrivate - firstPrivate
            << ",\n  \"process_exited_early\": " << (exited ? "true" : "false")
            << ",\n  \"ui_opened_during_measurement\": " << (opened ? "true" : "false")
            << ",\n  \"clock\": \"unbiased_interrupt_time\""
            << ",\n  \"passed_resource_budget\": "
            << (!exited && !opened && samples && average <= 0.5 && peak <= 50000000 ? "true"
                                                                                    : "false")
            << "\n}\n";
    CloseHandle(process);
    std::cout << "Samples: " << samples << ", CPU: " << average << "%, peak private WS: " << peak
              << " bytes\n";
    return exited ? 5 : 0;
}
