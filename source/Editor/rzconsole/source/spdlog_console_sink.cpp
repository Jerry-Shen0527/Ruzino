#include <rzconsole/spdlog_console_sink.h>
#include <rzconsole/imgui_console.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>

RUZINO_NAMESPACE_OPEN_SCOPE

template<typename Mutex>
void console_sink<Mutex>::set_console(ImGui_Console* console) {
    std::lock_guard<Mutex> lock(this->mutex_);
    console_ = console;
}

template<typename Mutex>
void console_sink<Mutex>::sink_it_(const spdlog::details::log_msg& msg) {
    if (console_) {
        spdlog::memory_buf_t formatted;
        this->formatter_->format(msg, formatted);
        std::string formatted_str = fmt::to_string(formatted);
        
        // Remove trailing newline if present
        if (!formatted_str.empty() && formatted_str.back() == '\n') {
            formatted_str.pop_back();
        }
        
        // Forward to console with severity - create LogItem directly
        LogSeverity severity = LogSeverity::Info;
        switch (msg.level) {
            case spdlog::level::warn: severity = LogSeverity::Warning; break;
            case spdlog::level::err: 
            case spdlog::level::critical: severity = LogSeverity::Error; break;
            default: severity = LogSeverity::Info; break;
        }
        
        console_->PrintWithSeverity(formatted_str.c_str(), severity);
    }
}

// Explicit template instantiations
template class console_sink<std::mutex>;
template class console_sink<spdlog::details::null_mutex>;

std::shared_ptr<console_sink_mt>& get_global_console_sink() {
    static std::shared_ptr<console_sink_mt> instance = std::make_shared<console_sink_mt>();
    return instance;
}

void setup_console_logging(ImGui_Console* console) {
    auto sink = get_global_console_sink();
    sink->set_console(console);

    if (auto logger = spdlog::default_logger()) {
        auto& sinks = logger->sinks();
        auto existing = std::find(sinks.begin(), sinks.end(), sink);
        if (existing == sinks.end()) {
            sinks.push_back(sink);
        }
        logger->set_pattern(
            "[%Y-%m-%d %H:%M:%S.%e] [%P:%t] [%l] [%n] %v");
        return;
    }

    // Fallback when no framework logger exists yet.
    auto logger = std::make_shared<spdlog::logger>("console", sink);
    logger->set_level(spdlog::level::trace);
    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%P:%t] [%l] [%n] %v");
    logger->flush_on(spdlog::level::err);

    spdlog::set_default_logger(logger);
    spdlog::flush_every(std::chrono::seconds(1));
}

std::shared_ptr<spdlog::logger> create_console_logger(
    const std::string& logger_name,
    ImGui_Console* console)
{
    auto sink = get_global_console_sink();
    sink->set_console(console);
    
    auto logger = std::make_shared<spdlog::logger>(logger_name, sink);
    logger->set_level(spdlog::level::trace);
    
    // Try to register logger if the function exists
    try {
        spdlog::register_logger(logger);
    } catch (...) {
        // Fallback: just return the logger
    }
    
    return logger;
}

RUZINO_NAMESPACE_CLOSE_SCOPE
