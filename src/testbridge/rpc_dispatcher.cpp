#include "rpc_dispatcher.h"
#include <QMetaObject>
#include <QVariant>

namespace testbridge {

void RpcDispatcher::registerMethod(const std::string& method, RpcHandler handler)
{
    m_handlers[method] = std::move(handler);
}

nlohmann::json RpcDispatcher::makeError(const nlohmann::json& id, int code,
                                         const std::string& message,
                                         const nlohmann::json& data)
{
    nlohmann::json resp;
    resp["jsonrpc"] = "2.0";
    resp["id"]      = id;
    nlohmann::json err;
    err["code"]    = code;
    err["message"] = message;
    if (!data.is_null()) err["data"] = data;
    resp["error"] = err;
    return resp;
}

nlohmann::json RpcDispatcher::makeResult(const nlohmann::json& id,
                                          const nlohmann::json& result)
{
    nlohmann::json resp;
    resp["jsonrpc"] = "2.0";
    resp["id"]      = id;
    resp["result"]  = result;
    return resp;
}

std::string RpcDispatcher::dispatch(const std::string& raw, QObject* /*guiTarget*/)
{
    nlohmann::json req;
    try {
        req = nlohmann::json::parse(raw);
    } catch (...) {
        return makeError(nullptr, RpcError::ParseError, "Parse error").dump();
    }

    // Validate jsonrpc version
    if (!req.contains("jsonrpc") || req["jsonrpc"] != "2.0") {
        return makeError(nullptr, RpcError::InvalidRequest, "Invalid Request").dump();
    }

    bool isNotification = !req.contains("id");
    nlohmann::json id = isNotification ? nlohmann::json(nullptr) : req["id"];

    if (!req.contains("method") || !req["method"].is_string()) {
        if (isNotification) return {};
        return makeError(id, RpcError::InvalidRequest, "method required").dump();
    }

    std::string method = req["method"].get<std::string>();
    nlohmann::json params = req.value("params", nlohmann::json::object());

    auto it = m_handlers.find(method);
    if (it == m_handlers.end()) {
        if (isNotification) return {};
        return makeError(id, RpcError::MethodNotFound,
                         "Method not found: " + method).dump();
    }

    try {
        nlohmann::json result = it->second(params);
        if (isNotification) return {};
        return makeResult(id, result).dump();
    } catch (const std::exception& e) {
        if (isNotification) return {};
        return makeError(id, RpcError::InternalError, e.what()).dump();
    } catch (...) {
        if (isNotification) return {};
        return makeError(id, RpcError::InternalError, "Unknown error").dump();
    }
}

} // namespace testbridge
