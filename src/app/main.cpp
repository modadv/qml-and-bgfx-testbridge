#include "core/logger/logger.h"
#include "engine/quick/render_viewport_item.h"
#include "quick/guidatabus.h"
#if TESTBRIDGE_APP_ENABLED
#include "testbridge/test_bridge.h"
#endif

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QDir>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QImage>
#include <QPainter>
#include <QProcess>
#include <QQuickItem>
#include <QQuickWindow>
#include <QThread>
#include <QTimer>
#include <QUrl>

#include <cmath>
#include <functional>
#include <map>

class LabController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int counter READ counter NOTIFY counterChanged)
public:
    explicit LabController(QObject* parent = nullptr)
        : QObject(parent)
    {
        setObjectName(QStringLiteral("controller_lab"));
    }

    int counter() const { return m_counter; }

    Q_INVOKABLE void increment()
    {
        ++m_counter;
        emit counterChanged();
        LOG_I("[LabController] counter incremented to {}", m_counter);
        GuiDataBus::instance()->post(GuiDataBus::SenderToken::fromObject(this),
                                     QStringLiteral("lab.counterChanged"),
                                     m_counter);
    }

    Q_INVOKABLE void reset()
    {
        m_counter = 0;
        emit counterChanged();
        LOG_I("[LabController] counter reset");
        GuiDataBus::instance()->post(GuiDataBus::SenderToken::fromObject(this),
                                     QStringLiteral("lab.counterChanged"),
                                     m_counter);
    }

signals:
    void counterChanged();

private:
    int m_counter = 0;
};

static QString ensureSampleAssets()
{
    const QString root = QDir::currentPath() + QStringLiteral("/assets");
    QDir().mkpath(root);

    const QString heightfieldPath = root + QStringLiteral("/sample_heightfield.png");
    const QString diffusePath = root + QStringLiteral("/sample_diffuse.png");

    if (!QFileInfo::exists(heightfieldPath))
    {
        QImage img(256, 256, QImage::Format_RGBA8888);
        for (int y = 0; y < img.height(); ++y)
        {
            for (int x = 0; x < img.width(); ++x)
            {
                const int dx = x - 128;
                const int dy = y - 128;
                const int wave = (std::sin(double(x) * 0.08) + std::cos(double(y) * 0.07)) * 32.0 + 128.0;
                const int mound = qMax(0, 180 - int(std::sqrt(double(dx * dx + dy * dy))));
                const int v = qBound(0, qMax(wave, mound), 255);
                img.setPixelColor(x, y, QColor(v, v, v, 255));
            }
        }
        img.save(heightfieldPath);
    }

    if (!QFileInfo::exists(diffusePath))
    {
        QImage img(256, 256, QImage::Format_RGBA8888);
        QPainter painter(&img);
        painter.fillRect(img.rect(), QColor(28, 36, 44));
        for (int y = 0; y < img.height(); y += 16)
        {
            for (int x = 0; x < img.width(); x += 16)
            {
                const bool alt = ((x / 16) + (y / 16)) % 2 == 0;
                painter.fillRect(QRect(x, y, 16, 16), alt ? QColor(70, 150, 110) : QColor(190, 175, 95));
            }
        }
        painter.end();
        img.save(diffusePath);
    }

    return root;
}

static const char* renderTierName(engine::RenderTier tier)
{
    switch (tier)
    {
        case engine::RenderTier::Full:
            return "full";
        case engine::RenderTier::NoCompute:
            return "nocompute";
    }
    return "unknown";
}

static nlohmann::json renderCapsJson()
{
    const auto& caps = RenderDevice::renderCaps();
    return {
        {"available", true},
        {"backend", "bgfx"},
        {"tier", renderTierName(caps.tier)},
        {"noCompute", caps.noCompute()},
        {"hasCompute", caps.hasCompute},
        {"hasIndirect", caps.hasIndirect},
        {"hasImageRW", caps.hasImageRW},
        {"isSoftwareBackend", caps.isSoftwareBackend},
        {"renderer", caps.rendererStr},
        {"glVersion", caps.glVersionStr},
        {"overrideSource", caps.overrideSource}
    };
}

static nlohmann::json bgfxStatsJson()
{
    const bgfx::Stats* stats = bgfx::getStats();
    if (!stats) {
        return {{"available", false}};
    }

    nlohmann::json views = nlohmann::json::array();
    for (uint16_t i = 0; i < stats->numViews; ++i) {
        const bgfx::ViewStats& view = stats->viewStats[i];
        views.push_back({
            {"view", view.view},
            {"name", view.name},
            {"cpuTimeBegin", view.cpuTimeBegin},
            {"cpuTimeEnd", view.cpuTimeEnd},
            {"gpuTimeBegin", view.gpuTimeBegin},
            {"gpuTimeEnd", view.gpuTimeEnd}
        });
    }

    return {
        {"available", true},
        {"cpuTimeFrame", stats->cpuTimeFrame},
        {"cpuTimeBegin", stats->cpuTimeBegin},
        {"cpuTimeEnd", stats->cpuTimeEnd},
        {"cpuTimerFreq", stats->cpuTimerFreq},
        {"gpuTimeBegin", stats->gpuTimeBegin},
        {"gpuTimeEnd", stats->gpuTimeEnd},
        {"gpuTimerFreq", stats->gpuTimerFreq},
        {"waitRender", stats->waitRender},
        {"waitSubmit", stats->waitSubmit},
        {"numDraw", stats->numDraw},
        {"numCompute", stats->numCompute},
        {"numBlit", stats->numBlit},
        {"maxGpuLatency", stats->maxGpuLatency},
        {"numDynamicIndexBuffers", stats->numDynamicIndexBuffers},
        {"numDynamicVertexBuffers", stats->numDynamicVertexBuffers},
        {"numFrameBuffers", stats->numFrameBuffers},
        {"numIndexBuffers", stats->numIndexBuffers},
        {"numPrograms", stats->numPrograms},
        {"numShaders", stats->numShaders},
        {"numTextures", stats->numTextures},
        {"numUniforms", stats->numUniforms},
        {"numVertexBuffers", stats->numVertexBuffers},
        {"numVertexLayouts", stats->numVertexLayouts},
        {"textureMemoryUsed", stats->textureMemoryUsed},
        {"rtMemoryUsed", stats->rtMemoryUsed},
        {"transientVbUsed", stats->transientVbUsed},
        {"transientIbUsed", stats->transientIbUsed},
        {"gpuMemoryMax", stats->gpuMemoryMax},
        {"gpuMemoryUsed", stats->gpuMemoryUsed},
        {"backbufferWidth", stats->width},
        {"backbufferHeight", stats->height},
        {"numViews", stats->numViews},
        {"views", views}
    };
}

static RenderViewportItem* findRenderViewportItem(QObject* root)
{
    if (!root)
        return nullptr;
    if (auto* item = qobject_cast<RenderViewportItem*>(root))
        return item;
    for (QObject* child : root->children()) {
        if (auto* found = findRenderViewportItem(child))
            return found;
    }
    if (auto* item = qobject_cast<QQuickItem*>(root)) {
        for (QQuickItem* child : item->childItems()) {
            if (auto* found = findRenderViewportItem(child))
                return found;
        }
    }
    return nullptr;
}

static nlohmann::json onGuiThread(const std::function<nlohmann::json()>& callback)
{
    nlohmann::json result;
    QObject* appObject = QCoreApplication::instance();
    if (!appObject || QThread::currentThread() == appObject->thread()) {
        result = callback();
        return result;
    }

    QMetaObject::invokeMethod(appObject, [&]() {
        result = callback();
    }, Qt::BlockingQueuedConnection);
    return result;
}

static RenderViewportItem* findLabRenderViewportItem(QQmlApplicationEngine& engine)
{
    for (QObject* root : engine.rootObjects()) {
        if (auto* found = findRenderViewportItem(root))
            return found;
    }
    return nullptr;
}

static void applyWindowEnvironmentOverrides(QQmlApplicationEngine& engine)
{
    if (engine.rootObjects().isEmpty())
        return;
    QObject* root = engine.rootObjects().front();
    auto applyDimension = [root](const char* envName, const char* propertyName) {
        const QString raw = qEnvironmentVariable(envName);
        if (raw.isEmpty())
            return;
        bool ok = false;
        const int value = raw.toInt(&ok);
        if (ok && value >= 320)
            root->setProperty(propertyName, value);
    };
    applyDimension("TESTBRIDGE_WINDOW_WIDTH", "width");
    applyDimension("TESTBRIDGE_WINDOW_HEIGHT", "height");
}

class LiveShaderService
{
public:
    explicit LiveShaderService(QQmlApplicationEngine& engine)
        : m_engine(engine)
    {
    }

    nlohmann::json compile(const nlohmann::json& params)
    {
        const std::string slot = params.value("slot", std::string("terrain_simple.fragment"));
        const std::string source = params.value("source", std::string{});
        QString shaderType;
        QString sourceBase;
        if (!slotSpec(slot, shaderType, sourceBase))
            throw std::invalid_argument("unsupported live shader slot: " + slot);
        if (source.empty())
            throw std::invalid_argument("params.source required");

        QString rendererDir;
        QString platform;
        QString profile;
        rendererTarget(shaderType, rendererDir, platform, profile);

        const QByteArray hashInput = QString::fromStdString(slot + "|" + source)
            .append("|")
            .append(rendererDir)
            .toUtf8();
        const QString hash = QString::fromLatin1(
            QCryptographicHash::hash(hashInput, QCryptographicHash::Sha256).toHex().left(24));

        QDir root(cacheRoot());
        root.mkpath(hash);
        const QString sourcePath = root.filePath(hash + "/" + sourceBase + ".sc");
        const QString binPath = root.filePath(hash + "/" + sourceBase + ".bin");
        const QString stdoutPath = root.filePath(hash + "/shaderc.stdout.txt");
        const QString stderrPath = root.filePath(hash + "/shaderc.stderr.txt");

        QFile sourceFile(sourcePath);
        if (!sourceFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
            throw std::runtime_error("failed to write live shader source");
        sourceFile.write(source.data(), qint64(source.size()));
        sourceFile.close();

        QFileInfo binInfo(binPath);
        bool cached = binInfo.exists() && binInfo.size() > 0;
        int exitCode = 0;
        QString stdOut;
        QString stdErr;
        if (!cached)
        {
            QProcess process;
            QStringList args;
            args << "-f" << QDir::toNativeSeparators(sourcePath)
                 << "-o" << QDir::toNativeSeparators(binPath)
                 << "--type" << shaderType
                 << "--platform" << platform
                 << "--profile" << profile
                 << "-i" << QDir::toNativeSeparators(sourceDir("external/bgfx.cmake/bgfx/src"))
                 << "-i" << QDir::toNativeSeparators(sourceDir("src/engine/shaders"))
                 << "-i" << QDir::toNativeSeparators(sourceDir("src/engine/common"))
                 << "--varyingdef" << QDir::toNativeSeparators(sourceDir("src/engine/shaders/varying.def.sc"))
                 << "-O" << "3";

            process.start(shadercPath(), args);
            if (!process.waitForFinished(30000))
            {
                process.kill();
                process.waitForFinished(3000);
                throw std::runtime_error("shaderc timed out");
            }
            exitCode = process.exitCode();
            stdOut = QString::fromLocal8Bit(process.readAllStandardOutput());
            stdErr = QString::fromLocal8Bit(process.readAllStandardError());
            QFile(stdoutPath).open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text);
            QFile outFile(stdoutPath);
            if (outFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
                outFile.write(stdOut.toUtf8());
            QFile errFile(stderrPath);
            if (errFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
                errFile.write(stdErr.toUtf8());
            cached = exitCode == 0 && QFileInfo(binPath).exists() && QFileInfo(binPath).size() > 0;
        }

        CompileRecord record;
        record.hash = hash;
        record.slot = QString::fromStdString(slot);
        record.renderer = rendererDir;
        record.sourcePath = sourcePath;
        record.binPath = binPath;
        record.stdoutPath = stdoutPath;
        record.stderrPath = stderrPath;
        record.ok = cached;
        record.lastExitCode = exitCode;
        record.lastStdout = stdOut;
        record.lastStderr = stdErr;
        m_records[hash] = record;

        return record.toJson(cached);
    }

    nlohmann::json apply(const nlohmann::json& params)
    {
        const QString hash = QString::fromStdString(params.value("hash", std::string{}));
        if (hash.isEmpty())
            throw std::invalid_argument("params.hash required");
        const auto it = m_records.find(hash);
        if (it == m_records.end() || !it->second.ok)
            throw std::runtime_error("compiled shader hash not found or failed");

        const CompileRecord record = it->second;
        const std::string requestedSlot = params.value("slot", record.slot.toStdString());
        if (requestedSlot != record.slot.toStdString())
            throw std::invalid_argument("params.slot does not match compiled shader slot");
        return onGuiThread([this, record]() {
            RenderViewportItem* item = findLabRenderViewportItem(m_engine);
            if (!item)
                return nlohmann::json{{"queued", false}, {"reason", "RenderViewportItem not found"}};
            item->requestLiveShader(record.slot, record.binPath, record.hash);
            m_lastAppliedHash = record.hash;
            return nlohmann::json{
                {"queued", true},
                {"slot", record.slot.toStdString()},
                {"hash", record.hash.toStdString()},
                {"binPath", record.binPath.toStdString()}
            };
        });
    }

    nlohmann::json revert(const nlohmann::json& params)
    {
        const std::string slot = params.value("slot", std::string("terrain_simple.fragment"));
        QString shaderType;
        QString sourceBase;
        if (!slotSpec(slot, shaderType, sourceBase))
            throw std::invalid_argument("unsupported live shader slot: " + slot);
        return onGuiThread([this, slot]() {
            RenderViewportItem* item = findLabRenderViewportItem(m_engine);
            if (!item)
                return nlohmann::json{{"queued", false}, {"reason", "RenderViewportItem not found"}};
            item->requestRevertLiveShader(QString::fromStdString(slot));
            m_lastAppliedHash.clear();
            return nlohmann::json{{"queued", true}, {"slot", slot}};
        });
    }

    nlohmann::json list() const
    {
        nlohmann::json records = nlohmann::json::array();
        for (const auto& pair : m_records)
            records.push_back(pair.second.toJson(true));
        return {
            {"available", true},
            {"supportedSlots", supportedSlotsJson()},
            {"lastAppliedHash", m_lastAppliedHash.toStdString()},
            {"cacheRoot", cacheRoot().toStdString()},
            {"shaderc", shadercPath().toStdString()},
            {"records", records}
        };
    }

private:
    struct CompileRecord
    {
        QString hash;
        QString slot;
        QString renderer;
        QString sourcePath;
        QString binPath;
        QString stdoutPath;
        QString stderrPath;
        bool ok = false;
        int lastExitCode = 0;
        QString lastStdout;
        QString lastStderr;

        nlohmann::json toJson(bool cached) const
        {
            return {
                {"ok", ok},
                {"cached", cached},
                {"hash", hash.toStdString()},
                {"slot", slot.toStdString()},
                {"renderer", renderer.toStdString()},
                {"sourcePath", sourcePath.toStdString()},
                {"binPath", binPath.toStdString()},
                {"stdoutPath", stdoutPath.toStdString()},
                {"stderrPath", stderrPath.toStdString()},
                {"exitCode", lastExitCode},
                {"stdout", lastStdout.toStdString()},
                {"stderr", lastStderr.toStdString()}
            };
        }
    };

    static QString sourceDir(const QString& relative)
    {
        return QDir(QStringLiteral(TESTBRIDGE_SOURCE_DIR)).filePath(relative);
    }

    static QString buildDir(const QString& relative)
    {
        return QDir(QStringLiteral(TESTBRIDGE_BUILD_DIR)).filePath(relative);
    }

    static QString cacheRoot()
    {
        const QString env = qEnvironmentVariable("TESTBRIDGE_SHADER_CACHE_DIR");
        if (!env.isEmpty())
            return env;
        return QDir(QCoreApplication::applicationDirPath()).filePath("live_shader_cache");
    }

    static QString shadercPath()
    {
        const QString env = qEnvironmentVariable("TESTBRIDGE_SHADERC");
        if (!env.isEmpty())
            return env;
#ifdef Q_OS_WIN
        const QString exe = QStringLiteral("shaderc.exe");
#else
        const QString exe = QStringLiteral("shaderc");
#endif
        const QStringList buildCandidates = {
            buildDir(QStringLiteral("external/bgfx.cmake/cmake/bgfx/") + exe),
            buildDir(QStringLiteral("external/bgfx.cmake/Release/") + exe),
            buildDir(QStringLiteral("external/bgfx.cmake/Debug/") + exe),
        };
        for (const QString& candidate : buildCandidates)
        {
            if (QFileInfo::exists(candidate))
                return candidate;
        }
        return QDir(QCoreApplication::applicationDirPath()).filePath("../../external/bgfx.cmake/Release/" + exe);
    }

    static nlohmann::json supportedSlotsJson()
    {
        return {"terrain_simple.vertex", "terrain_simple.fragment", "overlay_max_elevation.compute"};
    }

    static bool slotSpec(const std::string& slot, QString& shaderType, QString& sourceBase)
    {
        if (slot == "terrain_simple.vertex")
        {
            shaderType = QStringLiteral("vertex");
            sourceBase = QStringLiteral("vs_live_terrain_simple");
            return true;
        }
        if (slot == "terrain_simple.fragment")
        {
            shaderType = QStringLiteral("fragment");
            sourceBase = QStringLiteral("fs_live_terrain_simple");
            return true;
        }
        if (slot == "overlay_max_elevation.compute")
        {
            shaderType = QStringLiteral("compute");
            sourceBase = QStringLiteral("cs_live_overlay_max_elevation");
            return true;
        }
        return false;
    }

    static QString direct3DProfile(const QString& shaderType)
    {
        // shaderc derives the stage letter from --type and prepends it itself, so
        // the profile must be passed *without* one: "s_5_0" becomes vs_5_0/ps_5_0/
        // cs_5_0. Passing an already-prefixed "vs_5_0" yielded "vvs_5_0" and
        // D3DCompile error X3506 (unrecognized compiler target). This matches the
        // offline build, which passes s_5_0 for dx11 (src/engine/CMakeLists.txt).
        Q_UNUSED(shaderType);
        return QStringLiteral("s_5_0");
    }

    static void rendererTarget(const QString& shaderType,
                               QString& rendererDir,
                               QString& platform,
                               QString& profile)
    {
        switch (bgfx::getRendererType())
        {
            case bgfx::RendererType::Direct3D11:
            case bgfx::RendererType::Direct3D12:
                rendererDir = "dx11";
                platform = "windows";
                profile = direct3DProfile(shaderType);
                return;
            case bgfx::RendererType::OpenGL:
                rendererDir = "glsl";
#ifdef Q_OS_MACOS
                platform = "osx";
                profile = "410";
#elif defined(Q_OS_WIN)
                platform = "windows";
                profile = "440";
#else
                platform = "linux";
                profile = "440";
#endif
                return;
            case bgfx::RendererType::Vulkan:
                rendererDir = "spirv";
#ifdef Q_OS_WIN
                platform = "windows";
#elif defined(Q_OS_MACOS)
                platform = "osx";
#else
                platform = "linux";
#endif
                profile = "spirv";
                return;
            case bgfx::RendererType::Metal:
                rendererDir = "metal";
                platform = "osx";
                profile = "metal";
                return;
            default:
                rendererDir = "dx11";
                platform = "windows";
                profile = direct3DProfile(shaderType);
                return;
        }
    }

    QQmlApplicationEngine& m_engine;
    std::map<QString, CompileRecord> m_records;
    QString m_lastAppliedHash;
};

int main(int argc, char* argv[])
{
    qputenv("QT_OPENGL", QByteArrayLiteral("desktop"));
    qputenv("QSG_RENDER_LOOP", QByteArrayLiteral("basic"));
    QCoreApplication::setAttribute(Qt::AA_UseDesktopOpenGL);
    QCoreApplication::setApplicationName(QStringLiteral("testbridge-lab"));
    QCoreApplication::setOrganizationName(QStringLiteral("TestBridge"));

    QGuiApplication app(argc, argv);
    Logger::getInstance();

    qmlRegisterType<RenderViewportItem>("TestBridgeLab.Engine", 1, 0, "RenderViewportItem");

    const QString assetRoot = ensureSampleAssets();

    LabController controller;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("labController"), &controller);
    engine.rootContext()->setContextProperty(
        QStringLiteral("sampleHeightfieldUrl"),
        QUrl::fromLocalFile(assetRoot + QStringLiteral("/sample_heightfield.png")));
    engine.rootContext()->setContextProperty(
        QStringLiteral("sampleDiffuseUrl"),
        QUrl::fromLocalFile(assetRoot + QStringLiteral("/sample_diffuse.png")));

    const QUrl mainUrl = QUrl::fromLocalFile(QDir::currentPath() + QStringLiteral("/qml/Main.qml"));
    engine.load(mainUrl);
    if (engine.rootObjects().isEmpty())
        return 1;
    applyWindowEnvironmentOverrides(engine);

    quint16 port = 47600;
    const QString envPort = qEnvironmentVariable("TESTBRIDGE_PORT");
    if (!envPort.isEmpty())
    {
        bool ok = false;
        const uint parsed = envPort.toUInt(&ok);
        if (ok && parsed > 0 && parsed <= 65535)
            port = static_cast<quint16>(parsed);
    }

#if TESTBRIDGE_APP_ENABLED
    auto& bridge = testbridge::TestBridge::instance();
    bridge.setRenderCapsProvider([]() {
        nlohmann::json out = renderCapsJson();
        auto& system = RenderDevice::instance();
        out["initialized"] = system.isInitialized();
        out["generation"] = system.generation();
        out["lastFrameId"] = system.lastFrameId();
        out["readbackCount"] = system.readbackCount();
        return out;
    });
    bridge.setRenderStatsProvider([&engine]() {
        return onGuiThread([&engine]() {
            auto* item = findLabRenderViewportItem(engine);
            if (!item) {
                return nlohmann::json{
                    {"available", false},
                    {"reason", "RenderViewportItem not found"}
                };
            }
            QMutexLocker lock(&item->m_lock);
            nlohmann::json out = item->m_scene.performanceSnapshot();
            auto& system = RenderDevice::instance();
            out["bgfx"] = {
                {"initialized", system.isInitialized()},
                {"generation", system.generation()},
                {"lastFrameId", system.lastFrameId()},
                {"stats", bgfxStatsJson()}
            };
            return out;
        });
    });
    bridge.setRenderResourcesProvider([&engine]() {
        return onGuiThread([&engine]() {
            auto* item = findLabRenderViewportItem(engine);
            if (!item) {
                return nlohmann::json{
                    {"available", false},
                    {"reason", "RenderViewportItem not found"}
                };
            }
            QMutexLocker lock(&item->m_lock);
            nlohmann::json out = item->m_scene.resourcesSnapshot();
            out["qmlObjectName"] = item->objectName().toStdString();
            out["heightfieldSource"] = item->heightfieldSource().toString().toStdString();
            out["diffuseSource"] = item->diffuseSource().toString().toStdString();
            out["quickItem"] = {
                {"width", item->width()},
                {"height", item->height()},
                {"visible", item->isVisible()},
                {"windowVisible", item->window() ? item->window()->isVisible() : false}
            };
            return out;
        });
    });

#if TESTBRIDGE_LIVE_SHADER_ENABLED
    LiveShaderService liveShaders(engine);
    bridge.setShaderCompileHandler([&liveShaders](const nlohmann::json& params) {
        return liveShaders.compile(params);
    });
    bridge.setShaderApplyHandler([&liveShaders](const nlohmann::json& params) {
        return liveShaders.apply(params);
    });
    bridge.setShaderRevertHandler([&liveShaders](const nlohmann::json& params) {
        return liveShaders.revert(params);
    });
    bridge.setShaderListProvider([&liveShaders]() {
        return liveShaders.list();
    });
#endif

    if (!bridge.start(&app, port))
    {
        LOG_E("[testbridge-lab] TestBridge failed to listen on 127.0.0.1:{}", port);
        return 2;
    }

    QObject::connect(&app, &QCoreApplication::aboutToQuit, [&bridge] {
        bridge.stop();
    });

    LOG_I("[testbridge-lab] TestBridge listening on 127.0.0.1:{}", port);
#else
    LOG_I("[testbridge-lab] TestBridge disabled; running app without Agent automation endpoint");
#endif
    return app.exec();
}

#include "main.moc"
