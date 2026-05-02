#include "qml_probe.h"

#include <QGuiApplication>
#include <QCoreApplication>
#include <QWindow>
#include <QQuickWindow>
#include <QQuickItem>
#include <QMetaObject>
#include <QMetaMethod>
#include <QMetaProperty>
#include <QVariant>
#include <QMetaObject>
#include <QSet>
#include <QTest>

namespace testbridge {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static nlohmann::json qvariantToJson(const QVariant& v)
{
    if (!v.isValid() || v.isNull()) return nullptr;
    switch (static_cast<int>(v.type())) {
        case QMetaType::Bool:     return v.toBool();
        case QMetaType::Int:
        case QMetaType::Long:
        case QMetaType::LongLong: return v.toLongLong();
        case QMetaType::Double:
        case QMetaType::Float:    return v.toDouble();
        case QMetaType::QString:  return v.toString().toStdString();
        default:                  return v.toString().toStdString();
    }
}

static QVariant jsonToQVariant(const nlohmann::json& j)
{
    if (j.is_null())   return {};
    if (j.is_boolean())   return j.get<bool>();
    if (j.is_number_integer()) return QVariant(static_cast<qlonglong>(j.get<long long>()));
    if (j.is_number_float())   return j.get<double>();
    if (j.is_string()) return QString::fromStdString(j.get<std::string>());
    return QString::fromStdString(j.dump());
}

// Walk object tree recursively, collect all QObject* with a non-empty objectName.
// IMPORTANT: QQuickItem children live under childItems(), not QObject::children().
// We must walk both edges or QML-rendered controls become invisible to find/list.
// Use a visited-set to break cycles between QObject parent/children and
// QQuickItem parentItem/childItems. seen is per-call so handles can't leak across RPCs.
// We also walk QQuickWindow::contentItem() and Loader::item() (the latter is
// not always parented to its Loader as a QQuickItem child — async load, swap).
static void collectObjects(QObject* root, QList<QObject*>& out,
                           QSet<QObject*>& seen, int depth = 0)
{
    if (!root || depth > 100) return;
    if (seen.contains(root)) return;
    seen.insert(root);
    if (!root->objectName().isEmpty()) out.append(root);
    for (auto* child : root->children())
        collectObjects(child, out, seen, depth + 1);
    if (auto* item = qobject_cast<QQuickItem*>(root)) {
        for (auto* qchild : item->childItems())
            collectObjects(qchild, out, seen, depth + 1);
    }
    if (auto* w = qobject_cast<QQuickWindow*>(root)) {
        if (auto* ci = w->contentItem())
            collectObjects(ci, out, seen, depth + 1);
    }
    // Loader: its `item` property holds the loaded QQuickItem, which may not
    // appear under childItems() while async-loading or after swap.
    if (root->inherits("QQuickLoader")) {
        QVariant v = root->property("item");
        if (auto* loaded = qvariant_cast<QObject*>(v))
            collectObjects(loaded, out, seen, depth + 1);
    }
}

// Aggregate all probe roots: top-level windows + QCoreApplication
// (covers QObjects parented to QQmlEngine / QGuiApplication itself).
static void collectAllNamed(QList<QObject*>& objs)
{
    QSet<QObject*> seen;
    for (auto* win : QGuiApplication::allWindows())
        collectObjects(win, objs, seen);
    if (auto* app = QCoreApplication::instance())
        collectObjects(app, objs, seen);
}

// ---------------------------------------------------------------------------
// QmlProbe
// ---------------------------------------------------------------------------

QmlProbe::QmlProbe(QObject* parent)
    : QObject(parent)
{}

quint64 QmlProbe::registerObject(QObject* obj)
{
    if (!obj) return 0;
    auto handle = reinterpret_cast<quint64>(obj);
    QMutexLocker lk(&m_regMutex);
    m_registry[handle] = QPointer<QObject>(obj);
    return handle;
}

QObject* QmlProbe::resolveHandle(quint64 handle)
{
    QMutexLocker lk(&m_regMutex);
    auto it = m_registry.find(handle);
    if (it == m_registry.end()) return nullptr;
    return it.value().data(); // null if object was destroyed
}

nlohmann::json QmlProbe::list(const nlohmann::json& params)
{
    Q_UNUSED(params)
    nlohmann::json result = nlohmann::json::array();

    QMetaObject::invokeMethod(this, [&]() {
        QList<QObject*> objs;
        collectAllNamed(objs);

        for (auto* obj : objs) {
            nlohmann::json item;
            item["objectName"] = obj->objectName().toStdString();
            item["class"]      = obj->metaObject()->className();
            item["handle"]     = registerObject(obj);
            result.push_back(item);
        }
    }, Qt::DirectConnection);

    return result;
}

nlohmann::json QmlProbe::find(const nlohmann::json& params)
{
    if (!params.contains("objectName"))
        throw std::invalid_argument("params.objectName required");
    QString name = QString::fromStdString(params["objectName"].get<std::string>());

    nlohmann::json result = nullptr;

    QMetaObject::invokeMethod(this, [&]() {
        QList<QObject*> objs;
        collectAllNamed(objs);

        for (auto* obj : objs) {
            if (obj->objectName() == name) {
                result = registerObject(obj);
                break;
            }
        }
    }, Qt::DirectConnection);

    return result;
}

nlohmann::json QmlProbe::get(const nlohmann::json& params)
{
    if (!params.contains("handle") || !params.contains("property"))
        throw std::invalid_argument("params.handle and params.property required");

    quint64 handle = params["handle"].get<quint64>();
    QString prop   = QString::fromStdString(params["property"].get<std::string>());

    nlohmann::json result = nullptr;

    QMetaObject::invokeMethod(this, [&]() {
        QObject* obj = resolveHandle(handle);
        if (!obj) throw std::runtime_error("Invalid or stale handle");
        QVariant val = obj->property(prop.toUtf8().constData());
        result = qvariantToJson(val);
    }, Qt::DirectConnection);

    return result;
}

nlohmann::json QmlProbe::set(const nlohmann::json& params)
{
    if (!params.contains("handle") || !params.contains("property") || !params.contains("value"))
        throw std::invalid_argument("params.handle, params.property, params.value required");

    quint64 handle = params["handle"].get<quint64>();
    QString prop   = QString::fromStdString(params["property"].get<std::string>());
    QVariant value = jsonToQVariant(params["value"]);

    bool ok = false;
    QMetaObject::invokeMethod(this, [&]() {
        QObject* obj = resolveHandle(handle);
        if (!obj) throw std::runtime_error("Invalid or stale handle");
        ok = obj->setProperty(prop.toUtf8().constData(), value);
    }, Qt::DirectConnection);

    return ok;
}

nlohmann::json QmlProbe::invoke(const nlohmann::json& params)
{
    if (!params.contains("handle") || !params.contains("method"))
        throw std::invalid_argument("params.handle and params.method required");

    quint64 handle = params["handle"].get<quint64>();
    QString method = QString::fromStdString(params["method"].get<std::string>());
    nlohmann::json args = params.value("args", nlohmann::json::array());

    nlohmann::json result = nullptr;

    QMetaObject::invokeMethod(this, [&]() {
        QObject* obj = resolveHandle(handle);
        if (!obj) throw std::runtime_error("Invalid or stale handle");

        QVariant ret;
        bool ok = false;

        // Build up to 10 args. We dispatch on the JSON type so that integer/bool/double/string
        // values match the actual method signature instead of always being passed as QVariant —
        // many Q_INVOKABLE methods declare concrete types (e.g. `bool foo(int id)`).
        static thread_local QVariant     v_storage[10];
        static thread_local int          i_storage[10];
        static thread_local bool         b_storage[10];
        static thread_local double       d_storage[10];
        static thread_local QString      s_storage[10];

        auto toArg = [&](int i) -> QGenericArgument {
            if (i >= (int)args.size()) return QGenericArgument();
            const auto& j = args[i];
            if (j.is_boolean())          { b_storage[i] = j.get<bool>();           return Q_ARG(bool, b_storage[i]); }
            if (j.is_number_integer())   { i_storage[i] = (int)j.get<long long>(); return Q_ARG(int, i_storage[i]); }
            if (j.is_number_float())     { d_storage[i] = j.get<double>();         return Q_ARG(double, d_storage[i]); }
            if (j.is_string())           { s_storage[i] = QString::fromStdString(j.get<std::string>()); return Q_ARG(QString, s_storage[i]); }
            v_storage[i] = jsonToQVariant(j);
            return Q_ARG(QVariant, v_storage[i]);
        };

        ok = QMetaObject::invokeMethod(
            obj, method.toUtf8().constData(),
            Qt::DirectConnection,
            Q_RETURN_ARG(QVariant, ret),
            toArg(0), toArg(1), toArg(2), toArg(3), toArg(4),
            toArg(5), toArg(6), toArg(7), toArg(8), toArg(9));

        if (!ok) {
            // Try without return value
            ok = QMetaObject::invokeMethod(obj, method.toUtf8().constData(),
                Qt::DirectConnection,
                toArg(0), toArg(1), toArg(2), toArg(3), toArg(4),
                toArg(5), toArg(6), toArg(7), toArg(8), toArg(9));
        }

        if (!ok) throw std::runtime_error("invokeMethod failed for: " + method.toStdString());
        result = qvariantToJson(ret);
    }, Qt::DirectConnection);

    return result;
}

nlohmann::json QmlProbe::click(const nlohmann::json& params)
{
    if (!params.contains("handle"))
        throw std::invalid_argument("params.handle required");

    quint64 handle = params["handle"].get<quint64>();

    QMetaObject::invokeMethod(this, [&]() {
        QObject* obj = resolveHandle(handle);
        if (!obj) throw std::runtime_error("Invalid or stale handle");

        auto* item = qobject_cast<QQuickItem*>(obj);
        if (!item) throw std::runtime_error("Object is not a QQuickItem");

        QQuickWindow* win = item->window();
        if (!win) throw std::runtime_error("QQuickItem has no window");

        // Compute center in window coordinates
        QPointF scenePos = item->mapToScene(
            QPointF(item->width() / 2.0, item->height() / 2.0));
        QPoint center = scenePos.toPoint();

        QTest::mouseClick(win, Qt::LeftButton, Qt::NoModifier, center);
    }, Qt::DirectConnection);

    return true;
}

nlohmann::json QmlProbe::type(const nlohmann::json& params)
{
    if (!params.contains("handle") || !params.contains("text"))
        throw std::invalid_argument("params.handle and params.text required");

    quint64 handle = params["handle"].get<quint64>();
    QString text   = QString::fromStdString(params["text"].get<std::string>());

    QMetaObject::invokeMethod(this, [&]() {
        QObject* obj = resolveHandle(handle);
        if (!obj) throw std::runtime_error("Invalid or stale handle");

        auto* item = qobject_cast<QQuickItem*>(obj);
        if (!item) throw std::runtime_error("Object is not a QQuickItem");

        QQuickWindow* win = item->window();
        if (!win) throw std::runtime_error("QQuickItem has no window");

        item->forceActiveFocus();
        for (const QChar& ch : text)
            QTest::keyClick(win, ch.toLatin1());
    }, Qt::DirectConnection);

    return true;
}

// ---------------------------------------------------------------------------
// Helpers for mouse/key parsing
// ---------------------------------------------------------------------------

static Qt::MouseButton parseMouseButton(const std::string& s)
{
    if (s == "left"   || s.empty()) return Qt::LeftButton;
    if (s == "right")               return Qt::RightButton;
    if (s == "middle")              return Qt::MiddleButton;
    throw std::invalid_argument("unknown mouse button: " + s);
}

static Qt::KeyboardModifiers parseModifiers(const nlohmann::json& mods)
{
    Qt::KeyboardModifiers out = Qt::NoModifier;
    if (mods.is_null()) return out;
    if (!mods.is_array()) throw std::invalid_argument("modifiers must be an array of strings");
    for (const auto& m : mods) {
        if (!m.is_string()) throw std::invalid_argument("modifier entry must be a string");
        const std::string s = m.get<std::string>();
        if      (s == "ctrl"  || s == "control") out |= Qt::ControlModifier;
        else if (s == "shift")                   out |= Qt::ShiftModifier;
        else if (s == "alt")                     out |= Qt::AltModifier;
        else if (s == "meta"  || s == "super")   out |= Qt::MetaModifier;
        else if (s == "keypad")                  out |= Qt::KeypadModifier;
        else throw std::invalid_argument("unknown modifier: " + s);
    }
    return out;
}

// Map a key name to Qt::Key. Single-char strings ("a", "5", " ") fall back to
// the upper-cased latin1 code, which matches QTest::keyClick's expectation.
static int parseKey(const std::string& name)
{
    static const QHash<QString, int> kMap = {
        {"enter",     Qt::Key_Return},  {"return",   Qt::Key_Return},
        {"escape",    Qt::Key_Escape},  {"esc",      Qt::Key_Escape},
        {"tab",       Qt::Key_Tab},     {"backtab",  Qt::Key_Backtab},
        {"space",     Qt::Key_Space},
        {"backspace", Qt::Key_Backspace},{"delete",  Qt::Key_Delete},
        {"home",      Qt::Key_Home},    {"end",      Qt::Key_End},
        {"pageup",    Qt::Key_PageUp},  {"pagedown", Qt::Key_PageDown},
        {"up",        Qt::Key_Up},      {"down",     Qt::Key_Down},
        {"left",      Qt::Key_Left},    {"right",    Qt::Key_Right},
        {"insert",    Qt::Key_Insert},
        {"f1", Qt::Key_F1}, {"f2", Qt::Key_F2}, {"f3", Qt::Key_F3},
        {"f4", Qt::Key_F4}, {"f5", Qt::Key_F5}, {"f6", Qt::Key_F6},
        {"f7", Qt::Key_F7}, {"f8", Qt::Key_F8}, {"f9", Qt::Key_F9},
        {"f10", Qt::Key_F10}, {"f11", Qt::Key_F11}, {"f12", Qt::Key_F12},
    };
    QString q = QString::fromStdString(name).toLower();
    auto it = kMap.find(q);
    if (it != kMap.end()) return it.value();
    if (name.size() == 1) {
        // Latin1 single character: QTest accepts the upper-case code.
        return QChar(name[0]).toUpper().unicode();
    }
    throw std::invalid_argument("unknown key name: " + name);
}

nlohmann::json QmlProbe::mouse(const nlohmann::json& params)
{
    if (!params.contains("handle"))
        throw std::invalid_argument("params.handle required");

    quint64 handle      = params["handle"].get<quint64>();
    std::string action  = params.value("action", std::string("click"));
    std::string btnName = params.value("button", std::string("left"));
    Qt::MouseButton btn = parseMouseButton(btnName);
    Qt::KeyboardModifiers mods = parseModifiers(params.value("modifiers", nlohmann::json()));
    int delay_ms        = params.value("delay_ms", -1);

    QMetaObject::invokeMethod(this, [&]() {
        QObject* obj = resolveHandle(handle);
        if (!obj) throw std::runtime_error("Invalid or stale handle");

        auto* item = qobject_cast<QQuickItem*>(obj);
        if (!item) throw std::runtime_error("Object is not a QQuickItem");

        QQuickWindow* win = item->window();
        if (!win) throw std::runtime_error("QQuickItem has no window");

        QPointF scene = item->mapToScene(
            QPointF(item->width() / 2.0, item->height() / 2.0));
        QPoint center = scene.toPoint();

        if (action == "click") {
            QTest::mouseClick(win, btn, mods, center, delay_ms);
        } else if (action == "dblclick" || action == "doubleclick") {
            QTest::mouseDClick(win, btn, mods, center, delay_ms);
        } else if (action == "press") {
            QTest::mousePress(win, btn, mods, center, delay_ms);
        } else if (action == "release") {
            QTest::mouseRelease(win, btn, mods, center, delay_ms);
        } else if (action == "move") {
            QTest::mouseMove(win, center, delay_ms);
        } else {
            throw std::invalid_argument("unknown mouse action: " + action);
        }
    }, Qt::DirectConnection);

    return true;
}

nlohmann::json QmlProbe::key(const nlohmann::json& params)
{
    if (!params.contains("handle") || !params.contains("key"))
        throw std::invalid_argument("params.handle and params.key required");

    quint64 handle      = params["handle"].get<quint64>();
    std::string keyName = params["key"].get<std::string>();
    int keyCode         = parseKey(keyName);
    std::string action  = params.value("action", std::string("click"));
    Qt::KeyboardModifiers mods = parseModifiers(params.value("modifiers", nlohmann::json()));
    int delay_ms        = params.value("delay_ms", -1);

    QMetaObject::invokeMethod(this, [&]() {
        QObject* obj = resolveHandle(handle);
        if (!obj) throw std::runtime_error("Invalid or stale handle");

        auto* item = qobject_cast<QQuickItem*>(obj);
        if (!item) throw std::runtime_error("Object is not a QQuickItem");

        QQuickWindow* win = item->window();
        if (!win) throw std::runtime_error("QQuickItem has no window");

        item->forceActiveFocus();

        if (action == "click") {
            QTest::keyClick(win, static_cast<Qt::Key>(keyCode), mods, delay_ms);
        } else if (action == "press") {
            QTest::keyPress(win, static_cast<Qt::Key>(keyCode), mods, delay_ms);
        } else if (action == "release") {
            QTest::keyRelease(win, static_cast<Qt::Key>(keyCode), mods, delay_ms);
        } else {
            throw std::invalid_argument("unknown key action: " + action);
        }
    }, Qt::DirectConnection);

    return true;
}

nlohmann::json QmlProbe::geometry(const nlohmann::json& params)
{
    if (!params.contains("handle"))
        throw std::invalid_argument("params.handle required");

    quint64 handle = params["handle"].get<quint64>();
    nlohmann::json result = nlohmann::json::object();

    QMetaObject::invokeMethod(this, [&]() {
        QObject* obj = resolveHandle(handle);
        if (!obj) throw std::runtime_error("Invalid or stale handle");

        auto* item = qobject_cast<QQuickItem*>(obj);
        if (!item) {
            // Non-QQuickItem (e.g. ContextProperty VM) — return whatever we can.
            result["isQuickItem"] = false;
            return;
        }
        result["isQuickItem"] = true;

        const QPointF tl = item->mapToScene(QPointF(0, 0));
        const QPointF br = item->mapToScene(QPointF(item->width(), item->height()));

        nlohmann::json local;
        local["x"] = item->x();
        local["y"] = item->y();
        local["w"] = item->width();
        local["h"] = item->height();
        result["local"] = local;

        nlohmann::json scene;
        scene["x"] = tl.x();
        scene["y"] = tl.y();
        scene["w"] = br.x() - tl.x();
        scene["h"] = br.y() - tl.y();
        result["scene"] = scene;

        result["visible"]            = item->isVisible();
        result["enabled"]            = item->isEnabled();
        result["opacity"]            = item->opacity();
        result["z"]                  = item->z();
        result["rotation"]           = item->rotation();
        result["scale"]              = item->scale();
        result["clip"]               = item->clip();
        result["activeFocus"]        = item->hasActiveFocus();
        // effectivelyVisible is the heuristic an end-user would care about: visible chain + opacity > 0.
        result["effectivelyVisible"] = item->isVisible() && item->opacity() > 0.0;
    }, Qt::DirectConnection);

    return result;
}

static const char* methodTypeName(QMetaMethod::MethodType type)
{
    switch (type) {
        case QMetaMethod::Method: return "method";
        case QMetaMethod::Signal: return "signal";
        case QMetaMethod::Slot: return "slot";
        case QMetaMethod::Constructor: return "constructor";
    }
    return "unknown";
}

static const char* accessName(QMetaMethod::Access access)
{
    switch (access) {
        case QMetaMethod::Private: return "private";
        case QMetaMethod::Protected: return "protected";
        case QMetaMethod::Public: return "public";
    }
    return "unknown";
}

nlohmann::json QmlProbe::meta(const nlohmann::json& params)
{
    if (!params.contains("handle"))
        throw std::invalid_argument("params.handle required");

    const quint64 handle = params["handle"].get<quint64>();
    const bool includeValues = params.value("include_values", true);
    nlohmann::json result = nlohmann::json::object();

    QMetaObject::invokeMethod(this, [&]() {
        QObject* obj = resolveHandle(handle);
        if (!obj) throw std::runtime_error("Invalid or stale handle");

        const QMetaObject* meta = obj->metaObject();
        result["handle"] = handle;
        result["class"] = meta->className();
        result["objectName"] = obj->objectName().toStdString();
        result["inheritsQuickItem"] = qobject_cast<QQuickItem*>(obj) != nullptr;

        nlohmann::json properties = nlohmann::json::array();
        for (int i = 0; i < meta->propertyCount(); ++i) {
            const QMetaProperty prop = meta->property(i);
            nlohmann::json item;
            item["name"] = prop.name();
            item["typeName"] = prop.typeName();
            item["type"] = prop.type();
            item["readable"] = prop.isReadable();
            item["writable"] = prop.isWritable();
            item["resettable"] = prop.isResettable();
            item["constant"] = prop.isConstant();
            item["final"] = prop.isFinal();
            item["designable"] = prop.isDesignable(obj);
            item["stored"] = prop.isStored(obj);
            item["user"] = prop.isUser(obj);
            item["enum"] = prop.isEnumType();
            item["notify"] = prop.hasNotifySignal()
                ? prop.notifySignal().methodSignature().constData()
                : "";
            if (includeValues && prop.isReadable()) {
                item["value"] = qvariantToJson(prop.read(obj));
            }
            properties.push_back(item);
        }
        result["properties"] = properties;

        nlohmann::json methods = nlohmann::json::array();
        nlohmann::json signalItems = nlohmann::json::array();
        for (int i = 0; i < meta->methodCount(); ++i) {
            const QMetaMethod method = meta->method(i);
            nlohmann::json item;
            item["name"] = method.name().constData();
            item["signature"] = method.methodSignature().constData();
            item["type"] = methodTypeName(method.methodType());
            item["access"] = accessName(method.access());
            item["returnType"] = method.typeName();

            nlohmann::json paramTypes = nlohmann::json::array();
            for (const QByteArray& type : method.parameterTypes())
                paramTypes.push_back(type.constData());
            item["parameterTypes"] = paramTypes;

            nlohmann::json paramNames = nlohmann::json::array();
            for (const QByteArray& name : method.parameterNames())
                paramNames.push_back(name.constData());
            item["parameterNames"] = paramNames;

            if (method.methodType() == QMetaMethod::Signal)
                signalItems.push_back(item);
            else
                methods.push_back(item);
        }
        result["methods"] = methods;
        result["signals"] = signalItems;

        if (auto* item = qobject_cast<QQuickItem*>(obj)) {
            nlohmann::json quick;
            quick["x"] = item->x();
            quick["y"] = item->y();
            quick["width"] = item->width();
            quick["height"] = item->height();
            quick["visible"] = item->isVisible();
            quick["enabled"] = item->isEnabled();
            quick["opacity"] = item->opacity();
            quick["activeFocus"] = item->hasActiveFocus();
            result["quickItem"] = quick;
        }
    }, Qt::DirectConnection);

    return result;
}

// ---------------------------------------------------------------------------
// tree / hit
// ---------------------------------------------------------------------------

static void dumpItemTree(QQuickItem* item, nlohmann::json& out, int depth, int maxDepth,
                         bool onlyVisible, int maxNodes, bool& truncated)
{
    if (!item || depth > maxDepth) return;
    if (onlyVisible && !item->isVisible()) return;
    if ((int)out.size() >= maxNodes) { truncated = true; return; }

    nlohmann::json node;
    node["class"]      = item->metaObject()->className();
    node["objectName"] = item->objectName().toStdString();
    node["depth"]      = depth;
    const QPointF tl = item->mapToScene(QPointF(0, 0));
    nlohmann::json scene;
    scene["x"] = tl.x();
    scene["y"] = tl.y();
    scene["w"] = item->width();
    scene["h"] = item->height();
    node["scene"]   = scene;
    node["visible"] = item->isVisible();
    node["opacity"] = item->opacity();
    node["clip"]    = item->clip();
    out.push_back(node);

    for (auto* child : item->childItems()) {
        if ((int)out.size() >= maxNodes) { truncated = true; break; }
        dumpItemTree(child, out, depth + 1, maxDepth, onlyVisible, maxNodes, truncated);
    }
}

nlohmann::json QmlProbe::tree(const nlohmann::json& params)
{
    int  maxDepth    = params.value("max_depth", 30);
    bool onlyVisible = params.value("only_visible", true);
    int  maxNodes    = params.value("max_nodes", 5000);

    nlohmann::json items = nlohmann::json::array();
    bool truncated = false;

    QMetaObject::invokeMethod(this, [&]() {
        for (auto* w : QGuiApplication::allWindows()) {
            auto* qw = qobject_cast<QQuickWindow*>(w);
            if (!qw) continue;
            QQuickItem* root = qw->contentItem();
            if (!root) continue;
            dumpItemTree(root, items, 0, maxDepth, onlyVisible, maxNodes, truncated);
            if ((int)items.size() >= maxNodes) break;
        }
    }, Qt::DirectConnection);

    nlohmann::json result = nlohmann::json::object();
    result["items"]     = items;
    result["count"]     = items.size();
    result["truncated"] = truncated;
    result["limit"]     = maxNodes;
    return result;
}

// Recursive hit-test: deepest visible QQuickItem whose scene rect contains (x,y).
// Honors clip:true on parents — a child outside the parent's clip rect is invisible.
static QQuickItem* hitTest(QQuickItem* item, const QPointF& scenePt)
{
    if (!item || !item->isVisible() || item->opacity() <= 0.0) return nullptr;
    const QPointF tl = item->mapToScene(QPointF(0, 0));
    QRectF rect(tl.x(), tl.y(), item->width(), item->height());
    if (!rect.contains(scenePt)) return nullptr;

    QQuickItem* best = item;
    for (auto* child : item->childItems()) {
        // If parent clips, drop hits outside its own rect — childItems can stick out
        // past their parent visually, but won't render.
        if (item->clip() && !rect.contains(scenePt)) continue;
        QQuickItem* h = hitTest(child, scenePt);
        if (h) best = h;
    }
    return best;
}

nlohmann::json QmlProbe::hit(const nlohmann::json& params)
{
    if (!params.contains("x") || !params.contains("y"))
        throw std::invalid_argument("params.x and params.y required");

    double x = params["x"].get<double>();
    double y = params["y"].get<double>();

    nlohmann::json result = nlohmann::json::array();

    QMetaObject::invokeMethod(this, [&]() {
        QPointF pt(x, y);
        for (auto* w : QGuiApplication::allWindows()) {
            auto* qw = qobject_cast<QQuickWindow*>(w);
            if (!qw) continue;
            QQuickItem* root = qw->contentItem();
            if (!root) continue;
            QQuickItem* hit = hitTest(root, pt);
            if (!hit) continue;
            const std::string winName = qw->objectName().isEmpty()
                ? std::string(qw->metaObject()->className())
                : qw->objectName().toStdString();
            // Walk from hit up to root (deepest first), tagging which window
            // each node belongs to so multi-window callers can disambiguate.
            for (QQuickItem* cur = hit; cur; cur = cur->parentItem()) {
                nlohmann::json node;
                node["class"]      = cur->metaObject()->className();
                node["objectName"] = cur->objectName().toStdString();
                node["handle"]     = registerObject(cur);
                node["window"]     = winName;
                const QPointF tl = cur->mapToScene(QPointF(0, 0));
                nlohmann::json scene;
                scene["x"] = tl.x();
                scene["y"] = tl.y();
                scene["w"] = cur->width();
                scene["h"] = cur->height();
                node["scene"]   = scene;
                node["visible"] = cur->isVisible();
                node["clip"]    = cur->clip();
                result.push_back(node);
            }
        }
    }, Qt::DirectConnection);

    return result;
}

} // namespace testbridge
