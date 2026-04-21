#include "module_factory_registry.hpp"

namespace mould::comm {

ModuleFactoryRegistry& ModuleFactoryRegistry::Instance() {
  static ModuleFactoryRegistry registry;
  return registry;
}

bool ModuleFactoryRegistry::Register(const std::string& module_name, FactoryFn factory, std::string* out_error) {
  if (module_name.empty()) {
    if (out_error != nullptr) {
      *out_error = "module registration failed: empty module name";
    }
    return false;
  }
  if (!factory) {
    if (out_error != nullptr) {
      *out_error = "module registration failed for '" + module_name + "': empty factory";
    }
    return false;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (factories_.find(module_name) != factories_.end()) {
    if (out_error != nullptr) {
      *out_error = "module registration failed: duplicate module name '" + module_name + "'";
    }
    return false;
  }
  factories_[module_name] = std::move(factory);
  return true;
}

std::unique_ptr<ModuleBase> ModuleFactoryRegistry::Create(
    const std::string& module_name,
    const ModuleFactoryConfig& config,
    std::string* out_error) const {
  FactoryFn factory;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = factories_.find(module_name);
    if (it == factories_.end()) {
      if (out_error != nullptr) {
        *out_error = "module factory not registered: '" + module_name + "'";
      }
      return nullptr;
    }
    factory = it->second;
  }
  std::unique_ptr<ModuleBase> instance = factory(config);
  if (!instance && out_error != nullptr) {
    *out_error = "module factory returned null instance: '" + module_name + "'";
  }
  return instance;
}

std::unordered_set<std::string> ModuleFactoryRegistry::RegisteredNames() const {
  std::unordered_set<std::string> names;
  std::lock_guard<std::mutex> lock(mutex_);
  names.reserve(factories_.size());
  for (const auto& [name, _] : factories_) {
    (void)_;
    names.insert(name);
  }
  return names;
}

}  // namespace mould::comm
