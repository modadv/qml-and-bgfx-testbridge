#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <memory>

#include <nlohmann/json.hpp>
#include <QObject>

namespace testbridge {

using RpcHandler = std::function<nlohmann::json(const nlohmann::json& params)>;

// JSON-RPC 2.0 dispatcher.
// Register method handlers, then call dispatch() with raw message text.
// Returns a complete JSON-RPC response string (may be empty for notifications).
class RpcDispatcher
{
public:
    void registerMethod(const std::string& method, RpcHandler handler);

    // Dispatch a raw JSON-RPC request string.
    // `target` is the QObject on the GUI thread for Qt::BlockingQueuedConnection.
    std::string dispatch(const std::string& raw, QObject* guiTarget);

    // Build standard error response
    static nlohmann::json makeError(const nlohmann::json& id, int code,
                                    const std::string& message,
                                    const nlohmann::json& data = nullptr);

    static nlohmann::json makeResult(const nlohmann::json& id,
                                     const nlohmann::json& result);

private:
    std::unordered_map<std::string, RpcHandler> m_handlers;
};

// Standard JSON-RPC error codes
namespace RpcError {
    constexpr int ParseError     = -32700;
    constexpr int InvalidRequest = -32600;
    constexpr int MethodNotFound = -32601;
    constexpr int InvalidParams  = -32602;
    constexpr int InternalError  = -32603;
}

} // namespace testbridge
