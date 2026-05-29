#ifndef DBMS_TELEMETRY_HPP
#define DBMS_TELEMETRY_HPP

#include "json.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <iostream>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace dbms {

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;
using Ms = std::chrono::milliseconds;

// Клиент шлёт служебные команды с ';' как SQL: "TELEMETRY; "
inline std::string admin_command_name(const std::string& query) {
    std::string cmd = query;
    while (!cmd.empty() && (cmd.back() == ' ' || cmd.back() == '\t' || cmd.back() == ';')) {
        cmd.pop_back();
    }
    const auto start = cmd.find_first_not_of(" \t");
    if (start == std::string::npos) {
        return "";
    }
    const auto end = cmd.find_last_not_of(" \t");
    return cmd.substr(start, end - start + 1);
}

inline bool is_admin_command(const std::string& query) {
    const std::string cmd = admin_command_name(query);
    return cmd == "TELEMETRY" || cmd == "TELEMETRY CLUSTER" ||
           cmd == "STATS" || cmd == "ROTATE";
}

// Одно завершённое обращение к серверу.
struct RequestSample {
    Clock::time_point timestamp{}; // дата окончания запроса
    long long duration_ms = 0;
    bool is_error = false;
};

// Снимок метрик в момент запроса TELEMETRY / вывода на экран.
struct TelemetrySnapshot {
    std::string node_id;

    double current_rps = 0.0;
    double avg_rps_10m = 0.0;
    double max_rps_10m = 0.0;

    double avg_processing_ms_10s = 0.0;
    long long errors_last_minute = 0;
    double error_rate_per_sec_1m = 0.0;

    long long total_requests = 0;
    std::string collected_at;
};

// дек завершённых запросов (локально на узле).
class TelemetryCollector {
public:
    static constexpr int kRpsWindowSec = 1;
    static constexpr int kLatencyWindowSec = 10;
    static constexpr int kErrorWindowSec = 60;
    static constexpr int kRpsHistorySec = 600; // 10 минут

    explicit TelemetryCollector(std::string node_id = "local")
        : node_id_(std::move(node_id)) {}

    const std::string& node_id() const { return node_id_; }

    // собирает данные, добавляет их в дек и чистит данные > 10 минут
    void record(long long duration_ms, bool is_error) {
        RequestSample sample;
        sample.timestamp = Clock::now();
        sample.duration_ms = duration_ms;
        sample.is_error = is_error;

        std::lock_guard<std::mutex> lock(mutex_); // делаем безопасным
        samples_.push_back(sample); // добавляем метку
        total_requests_++;
        prune_locked(Clock::now());
    }

    TelemetrySnapshot snapshot() {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto now = Clock::now();
        prune_locked(now);

        TelemetrySnapshot snap;
        snap.node_id = node_id_;
        snap.total_requests = total_requests_;
        snap.collected_at = format_wall_clock();

        // считает количество запросов за какое то время (окно времени)
        const auto count_since = [&](Ms window) {
            const auto cutoff = now - window;
            int n = 0;
            for (const auto& s : samples_) {
                if (s.timestamp >= cutoff) {
                    ++n;
                }
            }
            return n;
        };

        const int current_count = count_since(Ms{kRpsWindowSec * 1000}); // за 1 сек
        snap.current_rps = static_cast<double>(current_count) / kRpsWindowSec;

        const int count_10m = count_since(Ms{kRpsHistorySec * 1000}); // за 10 м
        snap.avg_rps_10m = static_cast<double>(count_10m) / kRpsHistorySec;
        snap.max_rps_10m = max_rps_in_window_locked(now, Ms{kRpsHistorySec * 1000});

        long long latency_sum = 0;
        int latency_count = 0;
        const auto latency_cutoff = now - Ms{kLatencyWindowSec * 1000};
        for (const auto& s : samples_) {
            if (s.timestamp >= latency_cutoff) {
                latency_sum += s.duration_ms;
                ++latency_count;
            }
        }
        // среднее время за 10 секунд
        snap.avg_processing_ms_10s =
            latency_count > 0 ? static_cast<double>(latency_sum) / latency_count : 0.0;

        const auto error_cutoff = now - Ms{kErrorWindowSec * 1000};
        for (const auto& s : samples_) {
            if (s.timestamp >= error_cutoff && s.is_error) {
                ++snap.errors_last_minute;
            }
        }
        // количество ошибок за секунду за 1 минуту
        snap.error_rate_per_sec_1m =
            static_cast<double>(snap.errors_last_minute) / kErrorWindowSec;

        return snap;
    }

    // превращаем снапшот в json
    std::string to_json() {
        return snapshot_to_json(snapshot()).dump();
    }

    // из структуры в json
    static json snapshot_to_json(const TelemetrySnapshot& snap) {
        json j;
        j["node_id"] = snap.node_id;
        j["current_rps"] = snap.current_rps;
        j["avg_rps_10m"] = snap.avg_rps_10m;
        j["max_rps_10m"] = snap.max_rps_10m;
        j["avg_processing_ms_10s"] = snap.avg_processing_ms_10s;
        j["errors_last_minute"] = snap.errors_last_minute;
        j["error_rate_per_sec_1m"] = snap.error_rate_per_sec_1m;
        j["total_requests"] = snap.total_requests;
        j["collected_at"] = snap.collected_at;
        return j;
    }

    // создание строки для консоли
    std::string format_live_line() {
        const auto snap = snapshot();
        std::ostringstream os;
        os << std::fixed << std::setprecision(2);
        os << "[telemetry:" << snap.node_id << "] "
           << "RPS=" << snap.current_rps
           << " avg10m=" << snap.avg_rps_10m
           << " max10m=" << snap.max_rps_10m
           << " latency10s=" << snap.avg_processing_ms_10s << "ms"
           << " errors1m=" << snap.errors_last_minute
           << " err/s=" << snap.error_rate_per_sec_1m;
        return os.str();
    }

private:
    std::string node_id_;
    mutable std::mutex mutex_;
    std::deque<RequestSample> samples_;
    long long total_requests_ = 0;

    // очистка элементов старше 10 минут
    void prune_locked(Clock::time_point now) {
        const auto cutoff = now - Ms{kRpsHistorySec * 1000};
        while (!samples_.empty() && samples_.front().timestamp < cutoff) {
            samples_.pop_front();
        }
    }

    // ищем пик нагрузки по секундам
    double max_rps_in_window_locked(Clock::time_point now, Ms window) const {
        const auto cutoff = now - window;
        std::unordered_map<long long, int> per_second;

        for (const auto& s : samples_) {
            if (s.timestamp < cutoff) {
                continue;
            }
            const auto sec = std::chrono::duration_cast<std::chrono::seconds>(
                                 s.timestamp.time_since_epoch()).count();
            ++per_second[sec];
        }

        int peak = 0;
        for (const auto& [_, count] : per_second) {
            peak = std::max(peak, count);
        }
        return static_cast<double>(peak);
    }

    static std::string format_wall_clock() {
        const auto now = std::chrono::system_clock::now();
        const auto t = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};

#if defined(_WIN32)
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        std::ostringstream os;
        os << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
        return os.str();
    }
};

// RAII: фиксирует длительность и код при завершении запроса.
class RequestTelemetryScope {
public:
    explicit RequestTelemetryScope(TelemetryCollector* collector = nullptr,
                                   bool* recorded_flag = nullptr)
        : collector_(collector), recorded_flag_(recorded_flag), start_(Clock::now()) {}

    ~RequestTelemetryScope() {
        if (!finished_) {
            // код 500 - ошибка сервера
            finish(500);
        }
    }

    // отправляет данные в коллектор
    void finish(int status_code) {
        if (finished_) {
            return;
        }
        finished_ = true;
        const auto end = Clock::now();
        const auto duration_ms = std::chrono::duration_cast<Ms>(end - start_).count();
        const bool is_error = status_code >= 400;
        if (collector_) {
            collector_->record(duration_ms, is_error);
        }
        if (recorded_flag_) {
            *recorded_flag_ = true;
        }
    }

private:
    TelemetryCollector* collector_;
    bool* recorded_flag_;
    Clock::time_point start_;
    bool finished_ = false;
};

// Децентрализованный реестр: каждый узел регистрирует свой коллектор,
class TelemetryRegistry {
public:
    static TelemetryRegistry& instance() {
        static TelemetryRegistry registry;
        return registry;
    }

    // регистрирует коллектор ноды[id] = коллектор
    void register_node(TelemetryCollector* collector) {
        if (!collector) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        nodes_[collector->node_id()] = collector;
    }

    // удаляет регистрацию ноды
    void unregister_node(const std::string& node_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        nodes_.erase(node_id);
    }

    // создаём json всего кластера
    json cluster_snapshot() {
        std::lock_guard<std::mutex> lock(mutex_);
        json nodes = json::array();
        // снапшотим каждую ноду и складываем в json::array
        for (const auto& [id, collector] : nodes_) {
            if (collector) {
                nodes.push_back(TelemetryCollector::snapshot_to_json(collector->snapshot()));
            }
        }

        json root;
        root["nodes"] = nodes;
        root["node_count"] = nodes.size();

        if (!nodes.empty()) {
            double rps_sum = 0;
            double avg_rps_sum = 0;
            double max_rps_peak = 0;
            long long errors_sum = 0;
            for (const auto& n : nodes) {
                rps_sum += n.value("current_rps", 0.0);
                avg_rps_sum += n.value("avg_rps_10m", 0.0);
                max_rps_peak = std::max(max_rps_peak, n.value("max_rps_10m", 0.0));
                errors_sum += n.value("errors_last_minute", 0LL);
            }
            // заносим в кластер все метрики
            json cluster;
            cluster["current_rps_sum"] = rps_sum;
            cluster["avg_rps_10m_sum"] = avg_rps_sum;
            cluster["max_rps_10m_peak"] = max_rps_peak;
            cluster["errors_last_minute_sum"] = errors_sum;
            root["cluster"] = cluster;
        }

        return root;
    }

private:
    TelemetryRegistry() = default;

    mutable std::mutex mutex_;
    // сортированный словарь нод
    std::map<std::string, TelemetryCollector*> nodes_;
};

// периодический вывод метрик в консоль сервера (in real time).
class TelemetryDisplay {
public:
    TelemetryDisplay(TelemetryCollector& collector, std::chrono::seconds interval = std::chrono::seconds(5))
        : collector_(&collector), interval_(interval) {}

    // запускает дисплей поток 
    void start(std::atomic<bool>& running_flag) {
        // устанавливает true и возвращает старое значение
        if (running_.exchange(true)) {
            return;
        }
        // запускается поток
        thread_ = std::thread([this, &running_flag]() {
            // пока сервер жив и работает дисплей
            while (running_flag.load() && running_.load()) {
                if (collector_) {
                    std::cout << collector_->format_live_line() << std::endl;
                }
                // сон на какое то время (по умолчанию 2 с)
                std::this_thread::sleep_for(interval_);
            }
        });
    }

    // останавливает дисплей поток
    void stop() {
        running_.store(false);
        // ждёт завершение потока
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    ~TelemetryDisplay() { stop(); }

private:
    TelemetryCollector* collector_;
    std::chrono::seconds interval_;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

} // namespace dbms

#endif
