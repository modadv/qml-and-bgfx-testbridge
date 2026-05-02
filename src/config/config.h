#pragma once

#include <nlohmann/json.hpp>

#include <mutex>
#include <string>
#include <unordered_map>

namespace cfg {

class Config
{
public:
    static void set(const std::string& key, const nlohmann::json& value)
    {
        std::lock_guard<std::mutex> lock(mutex());
        values()[key] = value;
    }

    static bool save()
    {
        return true;
    }

    static bool getJson(const std::string& key, nlohmann::json& out)
    {
        std::lock_guard<std::mutex> lock(mutex());
        auto it = values().find(key);
        if (it == values().end())
            return false;
        out = it->second;
        return true;
    }

    static bool getDefaultJson(const std::string& key, nlohmann::json& out)
    {
        if (key != "usersettings/window_style/ng3d/camera")
            return false;

        out = {
            {"distance", 3.0},
            {"fovY", 60.0},
            {"pitch", 30.0},
            {"target", {{"x", 0.0}, {"y", 0.0}, {"z", 0.0}}},
            {"yaw", 180.0}
        };
        return true;
    }

    template <typename T>
    static bool get(const std::string& key, T& out)
    {
        std::lock_guard<std::mutex> lock(mutex());
        auto it = values().find(key);
        if (it == values().end())
            return false;
        out = it->second.get<T>();
        return true;
    }

private:
    static std::unordered_map<std::string, nlohmann::json>& values()
    {
        static std::unordered_map<std::string, nlohmann::json> store;
        return store;
    }

    static std::mutex& mutex()
    {
        static std::mutex m;
        return m;
    }
};

} // namespace cfg
