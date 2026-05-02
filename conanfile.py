from conan import ConanFile
from conan.tools.cmake import CMakeDeps, CMakeToolchain, cmake_layout


class TestBridgeLabConan(ConanFile):
    settings = "os", "compiler", "build_type", "arch"

    options = {
        "use_system_qt": [True, False],
    }

    default_options = {
        "use_system_qt": False,
        "qt/*:shared": True,
        "qt/*:qtsvg": True,
        "qt/*:qtquickcontrols": True,
        "qt/*:qtwebsockets": True,
        "qt/*:openssl": False,
    }

    def requirements(self):
        if not self.options.use_system_qt:
            self.requires("qt/5.15.11", options={"with_sqlite3": False})
        self.requires("spdlog/1.14.1")
        self.requires("nlohmann_json/3.11.2")

    def generate(self):
        tc = CMakeToolchain(self)
        prefix_paths = []
        if self.options.use_system_qt:
            qt_prefix = self.conf.get("user.build:system_qt_prefix")
            if qt_prefix:
                prefix_paths.append(qt_prefix)
        if prefix_paths:
            tc.cache_variables["CMAKE_PREFIX_PATH"] = ";".join(prefix_paths)
        tc.generate()
        CMakeDeps(self).generate()

    def layout(self):
        cmake_layout(self)
