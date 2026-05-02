#pragma once

#include <memory>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

class Logger
{
public:
    static Logger& getInstance()
    {
        static Logger instance;
        return instance;
    }

    std::shared_ptr<spdlog::logger> getLogger() const
    {
        return m_logger;
    }

private:
    Logger()
    {
        m_logger = spdlog::stdout_color_mt("testbridge_lab");
        m_logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
        spdlog::set_default_logger(m_logger);
    }

    std::shared_ptr<spdlog::logger> m_logger;
};

#define LOG_D(...) Logger::getInstance().getLogger()->debug(__VA_ARGS__)
#define LOG_I(...) Logger::getInstance().getLogger()->info(__VA_ARGS__)
#define LOG_W(...) Logger::getInstance().getLogger()->warn(__VA_ARGS__)
#define LOG_E(...) Logger::getInstance().getLogger()->error(__VA_ARGS__)
