#pragma once

#include <string>
#include <unordered_map>
#include <any>
#include <memory>
#include <stdexcept>

namespace nc {

// ============================================================
// Система модулей — плагины загружаются как shared library
// Вдохновлено PocketMine/Minestom: каждая подсистема — отдельный модуль
// ============================================================

class Server;

// Интерфейс модуля. Каждая подсистема наследует этот класс.
class Module {
public:
    virtual ~Module() = default;

    // Жизненный цикл
    virtual void onEnable(Server& server) = 0;
    virtual void onDisable() {}

    // Имя модуля для логирования и поиска
    virtual std::string_view getName() const = 0;
};

// Реестр модулей. Server хранит один экземпляр.
class ModuleRegistry {
public:
    template<typename T, typename... Args>
    T& registerModule(Args&&... args) {
        auto mod = std::make_unique<T>(std::forward<Args>(args)...);
        T& ref = *mod;
        modules_[std::string(mod->getName())] = std::move(mod);
        return ref;
    }

    template<typename T>
    T* getModule() {
        for (auto& [name, mod] : modules_) {
            auto* ptr = dynamic_cast<T*>(mod.get());
            if (ptr) return ptr;
        }
        return nullptr;
    }

    void enableAll(Server& server) {
        for (auto& [name, mod] : modules_) {
            mod->onEnable(server);
        }
    }

    void disableAll() {
        for (auto& [name, mod] : modules_) {
            mod->onDisable();
        }
    }

private:
    std::unordered_map<std::string, std::unique_ptr<Module>> modules_;
};

} // namespace nc
