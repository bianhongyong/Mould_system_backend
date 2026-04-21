#pragma once

#include "module_base.hpp"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

namespace mould::comm {

struct ModuleFactoryConfig {
  std::string module_name;
  ModuleRuntimeContext runtime_context;
};

class ModuleFactoryRegistry {
 public:
  using FactoryFn = std::function<std::unique_ptr<ModuleBase>(const ModuleFactoryConfig&)>;

  static ModuleFactoryRegistry& Instance();

  bool Register(const std::string& module_name, FactoryFn factory, std::string* out_error);

  template <typename ModuleClass>
  bool RegisterType(const std::string& module_name, std::string* out_error) {
    static_assert(std::is_base_of_v<ModuleBase, ModuleClass>, "Registered type must derive from ModuleBase");
    static_assert(
        std::is_constructible_v<ModuleClass, const ModuleFactoryConfig&>,
        "Registered type must be constructible from const ModuleFactoryConfig&");
    return Register(module_name, [](const ModuleFactoryConfig& config) {
      return std::make_unique<ModuleClass>(config);
    }, out_error);
  }

  std::unique_ptr<ModuleBase> Create(const std::string& module_name, const ModuleFactoryConfig& config, std::string* out_error) const;

  std::unordered_set<std::string> RegisteredNames() const;

 private:
  mutable std::mutex mutex_;
  std::unordered_map<std::string, FactoryFn> factories_;
};

namespace detail {
template <typename ModuleClass>
class ModuleAutoRegistrar {
 public:
  explicit ModuleAutoRegistrar(const char* module_name) {
    std::string error;
    if (!ModuleFactoryRegistry::Instance().RegisterType<ModuleClass>(module_name, &error)) {
      // Keep silent here; duplicate registrations can happen in tests and are observable by registry APIs.
    }
  }
};
}  // namespace detail

}  // namespace mould::comm

#define REGISTER_MOULD_MODULE(ModuleClass) \
  namespace { \
  ::mould::comm::detail::ModuleAutoRegistrar<ModuleClass> g_module_auto_registrar_##ModuleClass(#ModuleClass); \
  }

#define REGISTER_MOULD_MODULE_AS(alias, ModuleClass) \
  namespace { \
  ::mould::comm::detail::ModuleAutoRegistrar<ModuleClass> g_module_auto_registrar_alias_##ModuleClass(alias); \
  }
