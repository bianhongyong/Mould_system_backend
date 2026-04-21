#include "module_factory_registry.hpp"

#include <gtest/gtest.h>

#include <string>

namespace mould::comm {
namespace {

class RegistryUnitTestModule final : public ModuleBase {
 public:
  explicit RegistryUnitTestModule(const ModuleFactoryConfig& config)
      : ModuleBase(config.module_name, config.runtime_context) {}

 private:
  bool DoInit() override { return true; }
  bool SetupSubscriptions() override { return true; }
};

TEST(ModuleFactoryRegistryTest, ModuleFactoryRegistry_CreateRegisteredModule_ReturnsInstance) {
  ModuleFactoryRegistry& registry = ModuleFactoryRegistry::Instance();
  const std::string module_name = "RegistryUnitTestModule_Create";
  std::string error;
  ASSERT_TRUE(registry.RegisterType<RegistryUnitTestModule>(module_name, &error)) << error;

  ModuleFactoryConfig config;
  config.module_name = module_name;
  auto instance = registry.Create(module_name, config, &error);
  ASSERT_NE(instance, nullptr) << error;
}

TEST(ModuleFactoryRegistryTest, ModuleFactoryRegistry_CreateUnknownModule_ReturnsNotFound) {
  ModuleFactoryRegistry& registry = ModuleFactoryRegistry::Instance();
  ModuleFactoryConfig config;
  config.module_name = "unknown";
  std::string error;
  auto instance = registry.Create("DefinitelyUnknownModule", config, &error);
  EXPECT_EQ(instance, nullptr);
  EXPECT_NE(error.find("not registered"), std::string::npos);
}

TEST(ModuleFactoryRegistryTest, ModuleFactoryRegistry_DuplicateRegistration_ReturnsAlreadyExists) {
  ModuleFactoryRegistry& registry = ModuleFactoryRegistry::Instance();
  const std::string module_name = "RegistryUnitTestModule_Duplicate";
  std::string error;
  ASSERT_TRUE(registry.RegisterType<RegistryUnitTestModule>(module_name, &error)) << error;
  EXPECT_FALSE(registry.RegisterType<RegistryUnitTestModule>(module_name, &error));
  EXPECT_NE(error.find("duplicate"), std::string::npos);
}

}  // namespace
}  // namespace mould::comm
