#include "launch_plan_config.hpp"
#include "ms_logging.hpp"
#include "test_helpers.hpp"

#include <gflags/gflags.h>

#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <unordered_set>

DECLARE_int32(test_startup_priority);
DECLARE_int32(test_batch_size);
DECLARE_string(test_model_path);
DECLARE_bool(test_enable_debug);
DECLARE_double(test_scale);
DECLARE_int32(test_shared_int);
DECLARE_int32(test_typecheck_int);
DECLARE_string(test_unknown_gflag_key);
DECLARE_int32(test_mod_a_exec_threads);
DECLARE_int32(test_mod_b_exec_threads);
DECLARE_string(test_mod_a_topic);
DECLARE_string(test_mod_b_topic);

namespace {

namespace fs = std::filesystem;

fs::path MakeUniqueRoot() {
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<std::uint64_t> dist;
  return fs::temp_directory_path() / ("mould_lp_ut_" + std::to_string(dist(gen)));
}

void WriteFile(const fs::path& path, const std::string& content) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path);
  out << content;
}

void ResetTestGflags() {
  google::SetCommandLineOption("test_startup_priority", "0");
  google::SetCommandLineOption("test_batch_size", "0");
  google::SetCommandLineOption("test_model_path", "");
  google::SetCommandLineOption("test_enable_debug", "false");
  google::SetCommandLineOption("test_scale", "0");
  google::SetCommandLineOption("test_shared_int", "0");
  google::SetCommandLineOption("test_typecheck_int", "0");
  google::SetCommandLineOption("test_unknown_gflag_key", "");
  google::SetCommandLineOption("test_mod_a_exec_threads", "0");
  google::SetCommandLineOption("test_mod_b_exec_threads", "0");
  google::SetCommandLineOption("test_mod_a_topic", "");
  google::SetCommandLineOption("test_mod_b_topic", "");
}

std::string MinimalIoJson() {
  return R"({
  "input_channel": {
    "infer.results": {"slot_payload_bytes": "64"}
  },
  "output_channel": {
    "broker.frames": {"slot_payload_bytes": "8"}
  }
})";
}

std::string IsolatedIoJson(const char* in_ch, const char* out_ch) {
  std::ostringstream oss;
  oss << R"({
  "input_channel": {")" << in_ch << R"(": {"slot_payload_bytes": "8"}},
  "output_channel": {")" << out_ch << R"(": {}}
})";
  return oss.str();
}

// 4.1 ParseSuccess_SingleModule_Minimal
bool TestParseSuccess_SingleModule_Minimal() {
  const fs::path root = MakeUniqueRoot();
  WriteFile(root / "io.json", MinimalIoJson());
  const std::string plan = std::string(R"({
  "modules": {
    "ModA": {
      "module_name": "ModA",
      "resource": { "test_startup_priority": 10 },
      "module_params": { "test_batch_size": 2 },
      "io_channels_config_path": "io.json"
    }
  }
})");
  WriteFile(root / "launch_plan.txt", plan);

  mould::config::ParsedLaunchPlan out;
  std::string err;
  const bool ok = mould::config::ParseLaunchPlanFile((root / "launch_plan.txt").string(), &out, &err);
  const bool cleanup = std::filesystem::remove_all(root);
  (void)cleanup;
  if (!Check(ok, "4.1 parse should succeed")) {
    return false;
  }
  if (!Check(out.modules.size() == 1, "4.1 one module")) {
    return false;
  }
  if (!Check(out.modules[0].module_name == "ModA", "4.1 module name")) {
    return false;
  }
  if (!Check(out.modules[0].channels.input_channels.size() == 1, "4.1 input channels")) {
    return false;
  }
  if (!Check(out.modules[0].channels.output_channels.size() == 1, "4.1 output channels")) {
    return false;
  }
  if (!Check(out.global_topology.count("broker.frames") == 1, "4.1 topology")) {
    return false;
  }
  return Check(err.empty(), "4.1 no error string");
}

// 4.2 ParseSuccess_MultiModule_AllIoReachable
bool TestParseSuccess_MultiModule_AllIoReachable() {
  const fs::path root = MakeUniqueRoot();
  WriteFile(root / "a.json", IsolatedIoJson("a.in", "a.out"));
  WriteFile(root / "b.json", IsolatedIoJson("b.in", "b.out"));
  const std::string plan = R"({
  "modules": {
    "Alpha": {
      "module_name": "Alpha",
      "resource": { "test_startup_priority": 1 },
      "module_params": {},
      "io_channels_config_path": "a.json"
    },
    "Beta": {
      "module_name": "Beta",
      "resource": { "test_startup_priority": 1 },
      "module_params": {},
      "io_channels_config_path": "b.json"
    }
  }
})";
  WriteFile(root / "launch_plan.txt", plan);

  mould::config::ParsedLaunchPlan out;
  std::string err;
  const bool ok = mould::config::ParseLaunchPlanFile((root / "launch_plan.txt").string(), &out, &err);
  std::filesystem::remove_all(root);
  if (!Check(ok, "4.2 parse")) {
    return false;
  }
  if (!Check(out.modules.size() == 2, "4.2 two modules")) {
    return false;
  }
  return Check(out.global_topology.size() >= 2, "4.2 aggregated topology");
}

// 4.2.1 ParseSuccess_TopLevelMinloglevel
bool TestParseSuccess_TopLevelMinloglevel() {
  const fs::path root = MakeUniqueRoot();
  WriteFile(root / "io.json", MinimalIoJson());
  const std::string plan = R"({
  "minloglevel": 2,
  "modules": {
    "ModA": {
      "module_name": "ModA",
      "resource": { "test_startup_priority": 10 },
      "module_params": {},
      "io_channels_config_path": "io.json"
    }
  }
})";
  WriteFile(root / "launch_plan.txt", plan);

  mould::config::ParsedLaunchPlan out;
  std::string err;
  const bool ok = mould::config::ParseLaunchPlanFile((root / "launch_plan.txt").string(), &out, &err);
  std::filesystem::remove_all(root);
  if (!Check(ok, "4.2.1 parse")) {
    return false;
  }
  if (!Check(out.minloglevel.has_value(), "4.2.1 minloglevel present")) {
    return false;
  }
  return Check(*(out.minloglevel) == 2, "4.2.1 minloglevel value");
}

bool TestParseSuccess_CommunicationSlotCount() {
  const fs::path root = MakeUniqueRoot();
  WriteFile(root / "io.json", MinimalIoJson());
  const std::string plan = R"({
  "communication": {
    "shm_slot_count": 1024
  },
  "modules": {
    "ModA": {
      "module_name": "ModA",
      "resource": { "test_startup_priority": 10 },
      "module_params": {},
      "io_channels_config_path": "io.json"
    }
  }
})";
  WriteFile(root / "launch_plan.txt", plan);

  mould::config::ParsedLaunchPlan out;
  std::string err;
  const bool ok = mould::config::ParseLaunchPlanFile((root / "launch_plan.txt").string(), &out, &err);
  std::filesystem::remove_all(root);
  if (!Check(ok, "4.2.2 parse communication shm_slot_count")) {
    return false;
  }
  if (!Check(out.communication_slot_count.has_value(), "4.2.2 shm_slot_count present")) {
    return false;
  }
  return Check(*(out.communication_slot_count) == 1024U, "4.2.2 shm_slot_count value");
}

bool TestParseSuccess_CommunicationSlotPayloadBytes() {
  const fs::path root = MakeUniqueRoot();
  WriteFile(root / "io.json", MinimalIoJson());
  const std::string plan = R"({
  "communication": {
    "slot_payload_bytes": 4096
  },
  "modules": {
    "ModA": {
      "module_name": "ModA",
      "resource": { "test_startup_priority": 10 },
      "module_params": {},
      "io_channels_config_path": "io.json"
    }
  }
})";
  WriteFile(root / "launch_plan.txt", plan);

  mould::config::ParsedLaunchPlan out;
  std::string err;
  const bool ok = mould::config::ParseLaunchPlanFile((root / "launch_plan.txt").string(), &out, &err);
  std::filesystem::remove_all(root);
  if (!Check(ok, "4.2.2b parse communication slot_payload_bytes")) {
    return false;
  }
  if (!Check(
          out.communication_slot_payload_bytes.has_value(),
          "4.2.2b slot_payload_bytes present")) {
    return false;
  }
  return Check(
      *(out.communication_slot_payload_bytes) == 4096U,
      "4.2.2b slot_payload_bytes value");
}

bool TestParseFails_InvalidCommunicationSlotCount() {
  const fs::path root = MakeUniqueRoot();
  WriteFile(root / "io.json", MinimalIoJson());
  const std::string plan = R"({
  "communication": {
    "shm_slot_count": 0
  },
  "modules": {
    "ModA": {
      "module_name": "ModA",
      "resource": { "test_startup_priority": 10 },
      "module_params": {},
      "io_channels_config_path": "io.json"
    }
  }
})";
  WriteFile(root / "launch_plan.txt", plan);

  mould::config::ParsedLaunchPlan out;
  std::string err;
  const bool ok = mould::config::ParseLaunchPlanFile((root / "launch_plan.txt").string(), &out, &err);
  std::filesystem::remove_all(root);
  if (!Check(!ok, "4.2.3 parse should fail invalid communication shm_slot_count")) {
    return false;
  }
  return Check(err.find("communication.shm_slot_count") != std::string::npos, "4.2.3 error field path");
}

// 4.3 ParseFails_LaunchPlanFileNotFound
bool TestParseFails_LaunchPlanFileNotFound() {
  mould::config::ParsedLaunchPlan out;
  std::string err;
  const bool ok = mould::config::ParseLaunchPlanFile("/tmp/nonexistent_launch_plan_99128374.txt", &out, &err);
  if (!Check(!ok, "4.3 should fail")) {
    return false;
  }
  if (!Check(!err.empty(), "4.3 error message")) {
    return false;
  }
  if (!Check(err.find("/tmp/nonexistent_launch_plan_99128374.txt") != std::string::npos, "4.3 path in error")) {
    return false;
  }
  return Check(out.modules.empty(), "4.3 no partial modules");
}

// 4.4 ParseFails_ModuleIoFileNotFound
bool TestParseFails_ModuleIoFileNotFound() {
  const fs::path root = MakeUniqueRoot();
  const std::string plan = R"({
  "modules": {
    "X": {
      "module_name": "X",
      "resource": {},
      "module_params": {},
      "io_channels_config_path": "missing.json"
    }
  }
})";
  WriteFile(root / "launch_plan.txt", plan);
  mould::config::ParsedLaunchPlan out;
  std::string err;
  const bool ok = mould::config::ParseLaunchPlanFile((root / "launch_plan.txt").string(), &out, &err);
  std::filesystem::remove_all(root);
  if (!Check(!ok, "4.4 fail")) {
    return false;
  }
  const std::string abs_missing = (root / "missing.json").lexically_normal().string();
  if (!Check(err.find("X") != std::string::npos || err.find("modules.X") != std::string::npos, "4.4 module id")) {
    return false;
  }
  return Check(err.find(abs_missing) != std::string::npos || err.find("missing.json") != std::string::npos,
      "4.4 path in error");
}

// 4.5 ParseSuccess_RelativeIoPath_ResolvedAgainstLaunchPlanDir
bool TestParseSuccess_RelativeIoPath_ResolvedAgainstLaunchPlanDir() {
  const fs::path root = MakeUniqueRoot();
  WriteFile(root / "deep" / "here" / "io.json", MinimalIoJson());
  const std::string plan = R"({
  "modules": {
    "R": {
      "module_name": "R",
      "resource": {},
      "module_params": {},
      "io_channels_config_path": "../deep/here/io.json"
    }
  }
})";
  WriteFile(root / "plan" / "launch_plan.txt", plan);
  std::string prev_cwd;
  {
    char cwd_buf[4096];
    if (getcwd(cwd_buf, sizeof(cwd_buf)) != nullptr) {
      prev_cwd = cwd_buf;
    }
  }
  if (::chdir("/") != 0) {
    if (!prev_cwd.empty() && ::chdir(prev_cwd.c_str()) != 0) {
      (void)std::filesystem::remove_all(root);
      return Check(false, "4.5 restore cwd after chdir / failed");
    }
    (void)std::filesystem::remove_all(root);
    return Check(false, "4.5 chdir / failed");
  }
  mould::config::ParsedLaunchPlan out;
  std::string err;
  const bool ok =
      mould::config::ParseLaunchPlanFile((root / "plan" / "launch_plan.txt").string(), &out, &err);
  if (!prev_cwd.empty()) {
    if (::chdir(prev_cwd.c_str()) != 0) {
      (void)std::filesystem::remove_all(root);
      return Check(false, "4.5 restore cwd failed");
    }
  }
  std::filesystem::remove_all(root);
  return Check(ok, "4.5 relative to launch dir");
}

// 4.6 ParseSuccess_AbsoluteIoPath
bool TestParseSuccess_AbsoluteIoPath() {
  const fs::path root = MakeUniqueRoot();
  const fs::path io_path = root / "abs_io.json";
  WriteFile(io_path, MinimalIoJson());
  const std::string plan = "{\n  \"modules\": {\n    \"Abs\": {\n      \"module_name\": \"Abs\",\n      \"resource\": {},\n      \"module_params\": {},\n      \"io_channels_config_path\": \"" +
      io_path.string() + "\"\n    }\n  }\n}";
  WriteFile(root / "launch_plan.txt", plan);
  mould::config::ParsedLaunchPlan out;
  std::string err;
  const bool ok = mould::config::ParseLaunchPlanFile((root / "launch_plan.txt").string(), &out, &err);
  std::filesystem::remove_all(root);
  return Check(ok, "4.6 absolute io");
}

// 4.7 ParseFails_MissingModuleName
bool TestParseFails_MissingModuleName() {
  const fs::path root = MakeUniqueRoot();
  WriteFile(root / "io.json", MinimalIoJson());
  const std::string plan = R"({
  "modules": {
    "K": {
      "resource": {},
      "module_params": {},
      "io_channels_config_path": "io.json"
    }
  }
})";
  WriteFile(root / "launch_plan.txt", plan);
  mould::config::ParsedLaunchPlan out;
  std::string err;
  const bool ok = mould::config::ParseLaunchPlanFile((root / "launch_plan.txt").string(), &out, &err);
  std::filesystem::remove_all(root);
  return Check(!ok && err.find("module_name") != std::string::npos, "4.7");
}

// 4.8 ParseFails_MissingResource
bool TestParseFails_MissingResource() {
  const fs::path root = MakeUniqueRoot();
  WriteFile(root / "io.json", MinimalIoJson());
  const std::string plan = R"({
  "modules": {
    "K": {
      "module_name": "K",
      "module_params": {},
      "io_channels_config_path": "io.json"
    }
  }
})";
  WriteFile(root / "launch_plan.txt", plan);
  mould::config::ParsedLaunchPlan out;
  std::string err;
  const bool ok = mould::config::ParseLaunchPlanFile((root / "launch_plan.txt").string(), &out, &err);
  std::filesystem::remove_all(root);
  return Check(!ok && err.find("resource") != std::string::npos, "4.8");
}

// 4.9 ParseFails_MissingModuleParams
bool TestParseFails_MissingModuleParams() {
  const fs::path root = MakeUniqueRoot();
  WriteFile(root / "io.json", MinimalIoJson());
  const std::string plan = R"({
  "modules": {
    "K": {
      "module_name": "K",
      "resource": {},
      "io_channels_config_path": "io.json"
    }
  }
})";
  WriteFile(root / "launch_plan.txt", plan);
  mould::config::ParsedLaunchPlan out;
  std::string err;
  const bool ok = mould::config::ParseLaunchPlanFile((root / "launch_plan.txt").string(), &out, &err);
  std::filesystem::remove_all(root);
  return Check(!ok && err.find("module_params") != std::string::npos, "4.9");
}

// 4.10 ParseFails_MissingIoPathField
bool TestParseFails_MissingIoPathField() {
  const fs::path root = MakeUniqueRoot();
  const std::string plan = R"({
  "modules": {
    "K": {
      "module_name": "K",
      "resource": {},
      "module_params": {}
    }
  }
})";
  WriteFile(root / "launch_plan.txt", plan);
  mould::config::ParsedLaunchPlan out;
  std::string err;
  const bool ok = mould::config::ParseLaunchPlanFile((root / "launch_plan.txt").string(), &out, &err);
  std::filesystem::remove_all(root);
  return Check(!ok && err.find("io_channels_config_path") != std::string::npos, "4.10");
}

// 4.11 ParseFails_IoFileMissingInputChannel
bool TestParseFails_IoFileMissingInputChannel() {
  const fs::path root = MakeUniqueRoot();
  const std::string bad_io = R"({
  "output_channel": { "c.out": {} }
})";
  WriteFile(root / "io.json", bad_io);
  const std::string plan = R"({
  "modules": {
    "K": {
      "module_name": "K",
      "resource": {},
      "module_params": {},
      "io_channels_config_path": "io.json"
    }
  }
})";
  WriteFile(root / "launch_plan.txt", plan);
  mould::config::ParsedLaunchPlan out;
  std::string err;
  const bool ok = mould::config::ParseLaunchPlanFile((root / "launch_plan.txt").string(), &out, &err);
  std::filesystem::remove_all(root);
  return Check(!ok && err.find("input_channel") != std::string::npos, "4.11");
}

// 4.12 ParseFails_IoFileMissingOutputChannel
bool TestParseFails_IoFileMissingOutputChannel() {
  const fs::path root = MakeUniqueRoot();
  const std::string bad_io = R"({
  "input_channel": { "c.in": {} }
})";
  WriteFile(root / "io.json", bad_io);
  const std::string plan = R"({
  "modules": {
    "K": {
      "module_name": "K",
      "resource": {},
      "module_params": {},
      "io_channels_config_path": "io.json"
    }
  }
})";
  WriteFile(root / "launch_plan.txt", plan);
  mould::config::ParsedLaunchPlan out;
  std::string err;
  const bool ok = mould::config::ParseLaunchPlanFile((root / "launch_plan.txt").string(), &out, &err);
  std::filesystem::remove_all(root);
  return Check(!ok && err.find("output_channel") != std::string::npos, "4.12");
}

// 4.13 ParseFails_UnknownKeyInLaunchPlanModule
bool TestParseFails_UnknownKeyInLaunchPlanModule() {
  const fs::path root = MakeUniqueRoot();
  WriteFile(root / "io.json", MinimalIoJson());
  const std::string plan = R"({
  "modules": {
    "K": {
      "module_name": "K",
      "resource": {},
      "module_params": {},
      "io_channels_config_path": "io.json",
      "typo_extra": 1
    }
  }
})";
  WriteFile(root / "launch_plan.txt", plan);
  mould::config::ParsedLaunchPlan out;
  std::string err;
  const bool ok = mould::config::ParseLaunchPlanFile((root / "launch_plan.txt").string(), &out, &err);
  std::filesystem::remove_all(root);
  return Check(!ok && err.find("unknown key") != std::string::npos, "4.13");
}

// 4.14 ParseFails_InvalidScalarTypeInResource
bool TestParseFails_InvalidScalarTypeInResource() {
  const fs::path root = MakeUniqueRoot();
  WriteFile(root / "io.json", MinimalIoJson());
  const std::string plan = R"({
  "modules": {
    "K": {
      "module_name": "K",
      "resource": { "test_typecheck_int": "not-a-number" },
      "module_params": {},
      "io_channels_config_path": "io.json"
    }
  }
})";
  WriteFile(root / "launch_plan.txt", plan);
  mould::config::ParsedLaunchPlan out;
  std::string err;
  const bool ok = mould::config::ParseLaunchPlanFile((root / "launch_plan.txt").string(), &out, &err);
  std::filesystem::remove_all(root);
  if (!Check(!ok, "4.14 fail")) {
    return false;
  }
  return Check(
      err.find("modules.K.resource") != std::string::npos || err.find("test_typecheck_int") != std::string::npos,
      "4.14 field path");
}

// 4.15 ParseSuccess_ScalarTypesRoundTrip
bool TestParseSuccess_ScalarTypesRoundTrip() {
  const fs::path root = MakeUniqueRoot();
  WriteFile(root / "io.json", MinimalIoJson());
  const std::string plan = R"({
  "modules": {
    "T": {
      "module_name": "T",
      "resource": {
        "test_model_path": "hello",
        "test_startup_priority": 42,
        "test_scale": 1.25,
        "test_enable_debug": true
      },
      "module_params": {},
      "io_channels_config_path": "io.json"
    }
  }
})";
  WriteFile(root / "launch_plan.txt", plan);
  mould::config::ParsedLaunchPlan out;
  std::string err;
  const bool ok = mould::config::ParseLaunchPlanFile((root / "launch_plan.txt").string(), &out, &err);
  std::filesystem::remove_all(root);
  if (!Check(ok, "4.15 parse")) {
    return false;
  }
  const auto& r = out.modules[0].resource;
  if (!Check(std::holds_alternative<std::string>(r.at("test_model_path")), "4.15 string")) {
    return false;
  }
  if (!Check(std::get<std::string>(r.at("test_model_path")) == "hello", "4.15 string val")) {
    return false;
  }
  if (!Check(std::holds_alternative<std::int64_t>(r.at("test_startup_priority")), "4.15 int")) {
    return false;
  }
  if (!Check(std::holds_alternative<double>(r.at("test_scale")), "4.15 float")) {
    return false;
  }
  return Check(std::holds_alternative<bool>(r.at("test_enable_debug")), "4.15 bool");
}

// 4.16 AggregateChannels_TwoModulesNoConflict
bool TestAggregateChannels_TwoModulesNoConflict() {
  const fs::path root = MakeUniqueRoot();
  const std::string io_m1 = R"({
  "input_channel": { "shared.ch": {"slot_payload_bytes": "8"} },
  "output_channel": { "m1.out": {} }
})";
  const std::string io_m2 = R"({
  "input_channel": { "m1.out": {"slot_payload_bytes": "8"} },
  "output_channel": { "m2.out": {} }
})";
  WriteFile(root / "m1.json", io_m1);
  WriteFile(root / "m2.json", io_m2);
  const std::string plan = R"({
  "modules": {
    "M1": {
      "module_name": "M1",
      "resource": {},
      "module_params": {},
      "io_channels_config_path": "m1.json"
    },
    "M2": {
      "module_name": "M2",
      "resource": {},
      "module_params": {},
      "io_channels_config_path": "m2.json"
    }
  }
})";
  WriteFile(root / "launch_plan.txt", plan);
  mould::config::ParsedLaunchPlan out;
  std::string err;
  const bool ok = mould::config::ParseLaunchPlanFile((root / "launch_plan.txt").string(), &out, &err);
  std::filesystem::remove_all(root);
  if (!Check(ok, "4.16")) {
    return false;
  }
  if (!Check(out.global_topology.count("m1.out") && out.global_topology.count("m2.out"), "4.16 channels")) {
    return false;
  }
  return true;
}

// 4.17 AggregateChannels_FailsOnCrossModuleSameNameInconsistent
bool TestAggregateChannels_FailsOnCrossModuleSameNameInconsistent() {
  const fs::path root = MakeUniqueRoot();
  const std::string io_a = R"({
  "input_channel": { "dup.ch": {"slot_payload_bytes": "1"} },
  "output_channel": { "a.out": {} }
})";
  const std::string io_b = R"({
  "input_channel": { "a.out": {"slot_payload_bytes": "1"} },
  "output_channel": { "dup.ch": {"slot_payload_bytes": "2"} }
})";
  WriteFile(root / "a.json", io_a);
  WriteFile(root / "b.json", io_b);
  const std::string plan = R"({
  "modules": {
    "A": {
      "module_name": "A",
      "resource": {},
      "module_params": {},
      "io_channels_config_path": "a.json"
    },
    "B": {
      "module_name": "B",
      "resource": {},
      "module_params": {},
      "io_channels_config_path": "b.json"
    }
  }
})";
  WriteFile(root / "launch_plan.txt", plan);
  mould::config::ParsedLaunchPlan out;
  std::string err;
  const bool ok = mould::config::ParseLaunchPlanFile((root / "launch_plan.txt").string(), &out, &err);
  std::filesystem::remove_all(root);
  return Check(!ok && err.find("dup.ch") != std::string::npos, "4.17 conflict");
}

// 4.18 ParseFails_MultiModuleGflagKeyConflict
bool TestParseFails_MultiModuleGflagKeyConflict() {
  const fs::path root = MakeUniqueRoot();
  WriteFile(root / "x.json", MinimalIoJson());
  WriteFile(root / "y.json", MinimalIoJson());
  const std::string plan = R"({
  "modules": {
    "X": {
      "module_name": "X",
      "resource": { "test_shared_int": 1 },
      "module_params": {},
      "io_channels_config_path": "x.json"
    },
    "Y": {
      "module_name": "Y",
      "resource": { "test_shared_int": 2 },
      "module_params": {},
      "io_channels_config_path": "y.json"
    }
  }
})";
  WriteFile(root / "launch_plan.txt", plan);
  mould::config::ParsedLaunchPlan out;
  std::string err;
  const bool ok = mould::config::ParseLaunchPlanFile((root / "launch_plan.txt").string(), &out, &err);
  std::filesystem::remove_all(root);
  return Check(!ok && err.find("test_shared_int") != std::string::npos, "4.18");
}

// 4.19 ParseFails_ModuleNameMismatchWithDictKey
bool TestParseFails_ModuleNameMismatchWithDictKey() {
  const fs::path root = MakeUniqueRoot();
  WriteFile(root / "io.json", MinimalIoJson());
  const std::string plan = R"({
  "modules": {
    "KeyA": {
      "module_name": "KeyB",
      "resource": {},
      "module_params": {},
      "io_channels_config_path": "io.json"
    }
  }
})";
  WriteFile(root / "launch_plan.txt", plan);
  mould::config::ParsedLaunchPlan out;
  std::string err;
  const bool ok = mould::config::ParseLaunchPlanFile((root / "launch_plan.txt").string(), &out, &err);
  std::filesystem::remove_all(root);
  return Check(!ok, "4.19");
}

// 4.20 ErrorMessage_ContainsModuleAndFieldPath
bool TestErrorMessage_ContainsModuleAndFieldPath() {
  const fs::path root = MakeUniqueRoot();
  WriteFile(root / "io.json", MinimalIoJson());
  const std::string plan = R"({
  "modules": {
    "BadMod": {
      "module_name": "BadMod",
      "resource": { "test_typecheck_int": "x" },
      "module_params": {},
      "io_channels_config_path": "io.json"
    }
  }
})";
  WriteFile(root / "launch_plan.txt", plan);
  std::string err;
  mould::config::ParsedLaunchPlan out;
  const bool ok = mould::config::ParseLaunchPlanFile((root / "launch_plan.txt").string(), &out, &err);
  std::filesystem::remove_all(root);
  if (!Check(!ok, "4.20")) {
    return false;
  }
  if (!Check(err.find("BadMod") != std::string::npos || err.find("modules.BadMod") != std::string::npos,
          "4.20 module")) {
    return false;
  }
  return Check(err.find("resource") != std::string::npos, "4.20 field path");
}

// ApplyLaunchPlan: unknown gflag key in parsed output
bool TestApplyFailsUnknownGflagKey() {
  const fs::path root = MakeUniqueRoot();
  WriteFile(root / "io.json", MinimalIoJson());
  const std::string plan = R"({
  "modules": {
    "Z": {
      "module_name": "Z",
      "resource": { "this_flag_does_not_exist_xyz": 1 },
      "module_params": {},
      "io_channels_config_path": "io.json"
    }
  }
})";
  WriteFile(root / "launch_plan.txt", plan);
  mould::config::ParsedLaunchPlan out;
  std::string err;
  const bool ok = mould::config::ParseLaunchPlanFile((root / "launch_plan.txt").string(), &out, &err);
  std::filesystem::remove_all(root);
  if (!Check(ok, "parse for apply test")) {
    return false;
  }
  const bool applied = mould::config::ApplyLaunchPlanScalarsToRegisteredGflags(out, &err);
  return Check(!applied && err.find("this_flag_does_not_exist_xyz") != std::string::npos, "apply unknown");
}

// ApplyLaunchPlanScalarsToMatchingRegisteredGflags: skip standard resource keys without gflags
bool TestApplyMatchingSkipsUnknownResourceKeys() {
  const fs::path root = MakeUniqueRoot();
  WriteFile(root / "io.json", MinimalIoJson());
  const std::string plan = R"({
  "modules": {
    "X": {
      "module_name": "X",
      "resource": {
        "startup_priority": 10,
        "cpu_set": "0",
        "restart_backoff_ms": 20,
        "restart_max_retries": 3,
        "restart_window_ms": 1000,
        "restart_fuse_ms": 3000,
        "ready_timeout_ms": 800,
        "test_startup_priority": 91
      },
      "module_params": {},
      "io_channels_config_path": "io.json"
    }
  }
})";
  WriteFile(root / "launch_plan.txt", plan);
  mould::config::ParsedLaunchPlan out;
  std::string err;
  if (!mould::config::ParseLaunchPlanFile((root / "launch_plan.txt").string(), &out, &err)) {
    std::filesystem::remove_all(root);
    return Check(false, ("matching skip parse " + err).c_str());
  }
  google::SetCommandLineOption("test_startup_priority", "0");
  const bool applied = mould::config::ApplyLaunchPlanScalarsToMatchingRegisteredGflags(
      out, mould::config::LaunchPlanGflagMatchPolicy::kSkipUnknownKeys, &err);
  std::filesystem::remove_all(root);
  if (!Check(applied, "matching skip apply")) {
    return false;
  }
  if (!Check(err.empty(), "matching skip err empty")) {
    return false;
  }
  return Check(FLAGS_test_startup_priority == 91, "matching skip sets known gflag");
}

bool TestApplyMatchingFailsOnUnknownStrict() {
  const fs::path root = MakeUniqueRoot();
  WriteFile(root / "io.json", MinimalIoJson());
  const std::string plan = R"({
  "modules": {
    "Y": {
      "module_name": "Y",
      "resource": {
        "startup_priority": 10,
        "cpu_set": "0",
        "restart_backoff_ms": 20,
        "restart_max_retries": 3,
        "restart_window_ms": 1000,
        "restart_fuse_ms": 3000,
        "ready_timeout_ms": 800,
        "test_startup_priority": 1
      },
      "module_params": { "test_unknown_gflag_key_zzz": 1 },
      "io_channels_config_path": "io.json"
    }
  }
})";
  WriteFile(root / "launch_plan.txt", plan);
  mould::config::ParsedLaunchPlan out;
  std::string err;
  if (!mould::config::ParseLaunchPlanFile((root / "launch_plan.txt").string(), &out, &err)) {
    std::filesystem::remove_all(root);
    return Check(false, ("matching strict parse " + err).c_str());
  }
  const bool applied = mould::config::ApplyLaunchPlanScalarsToMatchingRegisteredGflags(
      out, mould::config::LaunchPlanGflagMatchPolicy::kFailOnUnknownKeys, &err);
  std::filesystem::remove_all(root);
  return Check(!applied && err.find("test_unknown_gflag_key_zzz") != std::string::npos,
               "matching strict fails on unknown module_params");
}

// 4.21 Fork_MultiChild_EachReadsOwnModuleParamsGflags
bool TestFork_MultiChild_EachReadsOwnModuleParamsGflags() {
  const fs::path root = MakeUniqueRoot();
  WriteFile(root / "a.json", IsolatedIoJson("fa.in", "fa.out"));
  WriteFile(root / "b.json", IsolatedIoJson("fb.in", "fb.out"));
  const std::string plan = R"({
  "modules": {
    "ModA": {
      "module_name": "ModA",
      "resource": {},
      "module_params": {
        "test_mod_a_exec_threads": 11,
        "test_mod_a_topic": "topic-a"
      },
      "io_channels_config_path": "a.json"
    },
    "ModB": {
      "module_name": "ModB",
      "resource": {},
      "module_params": {
        "test_mod_b_exec_threads": 22,
        "test_mod_b_topic": "topic-b"
      },
      "io_channels_config_path": "b.json"
    }
  }
})";
  WriteFile(root / "launch_plan.txt", plan);

  mould::config::ParsedLaunchPlan out;
  std::string err;
  if (!mould::config::ParseLaunchPlanFile((root / "launch_plan.txt").string(), &out, &err)) {
    std::filesystem::remove_all(root);
    return Check(false, ("4.21 parse " + err).c_str());
  }
  if (!mould::config::ApplyLaunchPlanScalarsToRegisteredGflags(out, &err)) {
    std::filesystem::remove_all(root);
    return Check(false, ("4.21 apply " + err).c_str());
  }

  const pid_t p1 = fork();
  if (p1 < 0) {
    std::filesystem::remove_all(root);
    return Check(false, "4.21 fork1");
  }
  if (p1 == 0) {
    bool ok = FLAGS_test_mod_a_exec_threads == 11 && FLAGS_test_mod_a_topic == "topic-a";
    _exit(ok ? 0 : 6);
  }
  const pid_t p2 = fork();
  if (p2 < 0) {
    (void)waitpid(p1, nullptr, 0);
    std::filesystem::remove_all(root);
    return Check(false, "4.21 fork2");
  }
  if (p2 == 0) {
    bool ok = FLAGS_test_mod_b_exec_threads == 22 && FLAGS_test_mod_b_topic == "topic-b";
    _exit(ok ? 0 : 7);
  }

  int st1 = 0;
  int st2 = 0;
  (void)waitpid(p1, &st1, 0);
  (void)waitpid(p2, &st2, 0);
  std::filesystem::remove_all(root);
  if (!Check(WIFEXITED(st1) && WEXITSTATUS(st1) == 0, "4.21 child A")) {
    return false;
  }
  return Check(WIFEXITED(st2) && WEXITSTATUS(st2) == 0, "4.21 child B");
}

bool TestResourceSchemaValidator_RejectsMissingRequiredFields() {
  const fs::path root = MakeUniqueRoot();
  WriteFile(root / "io.json", MinimalIoJson());
  const std::string plan = R"({
  "modules": {
    "StrictA": {
      "module_name": "StrictA",
      "resource": {
        "startup_priority": 10,
        "cpu_set": "0,2",
        "restart_backoff_ms": 100,
        "restart_max_retries": 3,
        "restart_window_ms": 1000,
        "restart_fuse_ms": 5000
      },
      "module_params": {},
      "io_channels_config_path": "io.json"
    }
  }
})";
  WriteFile(root / "launch_plan.txt", plan);

  mould::config::ParsedLaunchPlan out;
  std::string err;
  mould::config::LaunchPlanValidationOptions options;
  options.enforce_strict_resource_schema = true;
  const bool ok = mould::config::ParseLaunchPlanFile((root / "launch_plan.txt").string(), &out, &err, options);
  std::filesystem::remove_all(root);
  if (!Check(!ok, "7.1 strict schema should reject missing required field")) {
    return false;
  }
  return Check(err.find("ready_timeout_ms") != std::string::npos, "7.1 error should mention missing field");
}

bool TestResourceSchemaValidator_RejectsInvalidCpuSetAndMapsLegacyCpuId() {
  {
    const fs::path root = MakeUniqueRoot();
    WriteFile(root / "io.json", MinimalIoJson());
    const std::string plan = R"({
    "modules": {
      "StrictEmptyCpuSet": {
        "module_name": "StrictEmptyCpuSet",
        "resource": {
          "startup_priority": 1,
          "cpu_set": "",
          "restart_backoff_ms": 100,
          "restart_max_retries": 3,
          "restart_window_ms": 1000,
          "restart_fuse_ms": 5000,
          "ready_timeout_ms": 3000
        },
        "module_params": {},
        "io_channels_config_path": "io.json"
      }
    }
  })";
    WriteFile(root / "launch_plan.txt", plan);
    mould::config::ParsedLaunchPlan out;
    std::string err;
    mould::config::LaunchPlanValidationOptions options;
    options.enforce_strict_resource_schema = true;
    const bool ok = mould::config::ParseLaunchPlanFile((root / "launch_plan.txt").string(), &out, &err, options);
    std::filesystem::remove_all(root);
    if (!Check(ok, "7.2 empty cpu_set should be treated as no binding")) {
      return false;
    }
    if (!Check(out.modules.size() == 1, "7.2 empty cpu_set one module")) {
      return false;
    }
    const auto it = out.modules[0].resource.find("cpu_set");
    if (!Check(it != out.modules[0].resource.end(), "7.2 empty cpu_set remains present")) {
      return false;
    }
    if (!Check(std::holds_alternative<std::string>(it->second), "7.2 empty cpu_set stored as string")) {
      return false;
    }
    if (!Check(std::get<std::string>(it->second).empty(), "7.2 empty cpu_set should normalize to empty string")) {
      return false;
    }
  }

  {
    const fs::path root = MakeUniqueRoot();
    WriteFile(root / "io.json", MinimalIoJson());
    const std::string plan = R"({
    "modules": {
      "StrictB": {
        "module_name": "StrictB",
        "resource": {
          "startup_priority": 1,
          "cpu_set": "0,a,4",
          "restart_backoff_ms": 100,
          "restart_max_retries": 3,
          "restart_window_ms": 1000,
          "restart_fuse_ms": 5000,
          "ready_timeout_ms": 3000
        },
        "module_params": {},
        "io_channels_config_path": "io.json"
      }
    }
  })";
    WriteFile(root / "launch_plan.txt", plan);
    mould::config::ParsedLaunchPlan out;
    std::string err;
    mould::config::LaunchPlanValidationOptions options;
    options.enforce_strict_resource_schema = true;
    const bool ok = mould::config::ParseLaunchPlanFile((root / "launch_plan.txt").string(), &out, &err, options);
    std::filesystem::remove_all(root);
    if (!Check(!ok, "7.2 invalid cpu_set should fail")) {
      return false;
    }
    if (!Check(err.find("cpu_set") != std::string::npos, "7.2 invalid cpu_set error path")) {
      return false;
    }
  }

  {
    const fs::path root = MakeUniqueRoot();
    WriteFile(root / "io.json", MinimalIoJson());
    const std::string plan = R"({
    "modules": {
      "StrictC": {
        "module_name": "StrictC",
        "resource": {
          "startup_priority": 1,
          "cpu_id": 3,
          "restart_backoff_ms": 100,
          "restart_max_retries": 3,
          "restart_window_ms": 1000,
          "restart_fuse_ms": 5000,
          "ready_timeout_ms": 3000
        },
        "module_params": {},
        "io_channels_config_path": "io.json"
      }
    }
  })";
    WriteFile(root / "launch_plan.txt", plan);
    mould::config::ParsedLaunchPlan out;
    std::string err;
    mould::config::LaunchPlanValidationOptions options;
    options.enforce_strict_resource_schema = true;
    const bool ok = mould::config::ParseLaunchPlanFile((root / "launch_plan.txt").string(), &out, &err, options);
    std::filesystem::remove_all(root);
    if (!Check(ok, "7.2 legacy cpu_id should map to cpu_set")) {
      return false;
    }
    if (!Check(out.modules.size() == 1, "7.2 mapped case one module")) {
      return false;
    }
    const auto it = out.modules[0].resource.find("cpu_set");
    if (!Check(it != out.modules[0].resource.end(), "7.2 mapped resource contains cpu_set")) {
      return false;
    }
    if (!Check(std::holds_alternative<std::string>(it->second), "7.2 cpu_set normalized string")) {
      return false;
    }
    return Check(std::get<std::string>(it->second) == "3", "7.2 cpu_id maps to cpu_set");
  }
}

bool TestLaunchPlan_RejectsUnregisteredModuleFactory() {
  const fs::path root = MakeUniqueRoot();
  WriteFile(root / "io.json", MinimalIoJson());
  const std::string plan = R"({
  "modules": {
    "UnknownMod": {
      "module_name": "UnknownMod",
      "resource": {
        "startup_priority": 1,
        "cpu_set": "0",
        "restart_backoff_ms": 100,
        "restart_max_retries": 3,
        "restart_window_ms": 1000,
        "restart_fuse_ms": 5000,
        "ready_timeout_ms": 3000
      },
      "module_params": {},
      "io_channels_config_path": "io.json"
    }
  }
})";
  WriteFile(root / "launch_plan.txt", plan);

  mould::config::ParsedLaunchPlan out;
  std::string err;
  const std::unordered_set<std::string> registered = {"KnownMod"};
  mould::config::LaunchPlanValidationOptions options;
  options.enforce_strict_resource_schema = true;
  options.registered_module_names = &registered;
  const bool ok = mould::config::ParseLaunchPlanFile((root / "launch_plan.txt").string(), &out, &err, options);
  std::filesystem::remove_all(root);
  if (!Check(!ok, "1.3 startup precheck should reject unregistered module")) {
    return false;
  }
  return Check(err.find("unregistered module factory") != std::string::npos, "1.3 error details");
}

}  // namespace

int main(int argc, char** argv) {
  (void)argc;
  mould::InitApplicationLogging(argv[0]);
  int dummy_argc = 1;
  google::ParseCommandLineFlags(&dummy_argc, &argv, true);

  ResetTestGflags();
  bool ok = true;
  ok = TestParseSuccess_SingleModule_Minimal() && ok;
  ResetTestGflags();
  ok = TestParseSuccess_MultiModule_AllIoReachable() && ok;
  ok = TestParseSuccess_TopLevelMinloglevel() && ok;
  ok = TestParseSuccess_CommunicationSlotCount() && ok;
  ok = TestParseSuccess_CommunicationSlotPayloadBytes() && ok;
  ok = TestParseFails_InvalidCommunicationSlotCount() && ok;
  ok = TestParseFails_LaunchPlanFileNotFound() && ok;
  ok = TestParseFails_ModuleIoFileNotFound() && ok;
  ok = TestParseSuccess_RelativeIoPath_ResolvedAgainstLaunchPlanDir() && ok;
  ok = TestParseSuccess_AbsoluteIoPath() && ok;
  ok = TestParseFails_MissingModuleName() && ok;
  ok = TestParseFails_MissingResource() && ok;
  ok = TestParseFails_MissingModuleParams() && ok;
  ok = TestParseFails_MissingIoPathField() && ok;
  ok = TestParseFails_IoFileMissingInputChannel() && ok;
  ok = TestParseFails_IoFileMissingOutputChannel() && ok;
  ok = TestParseFails_UnknownKeyInLaunchPlanModule() && ok;
  ok = TestParseFails_InvalidScalarTypeInResource() && ok;
  ok = TestParseSuccess_ScalarTypesRoundTrip() && ok;
  ok = TestAggregateChannels_TwoModulesNoConflict() && ok;
  ok = TestAggregateChannels_FailsOnCrossModuleSameNameInconsistent() && ok;
  ok = TestParseFails_MultiModuleGflagKeyConflict() && ok;
  ok = TestParseFails_ModuleNameMismatchWithDictKey() && ok;
  ok = TestErrorMessage_ContainsModuleAndFieldPath() && ok;
  ok = TestApplyFailsUnknownGflagKey() && ok;
  ok = TestApplyMatchingSkipsUnknownResourceKeys() && ok;
  ok = TestApplyMatchingFailsOnUnknownStrict() && ok;
  ok = TestFork_MultiChild_EachReadsOwnModuleParamsGflags() && ok;
  ok = TestResourceSchemaValidator_RejectsMissingRequiredFields() && ok;
  ok = TestResourceSchemaValidator_RejectsInvalidCpuSetAndMapsLegacyCpuId() && ok;
  ok = TestLaunchPlan_RejectsUnregisteredModuleFactory() && ok;

  if (!ok) {
    LOG(ERROR) << "launch_plan_config_test failed";
    mould::ShutdownApplicationLogging();
    return 1;
  }
  LOG(INFO) << "launch_plan_config_test passed";
  mould::ShutdownApplicationLogging();
  return 0;
}
