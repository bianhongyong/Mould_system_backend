#include <gflags/gflags.h>

// Test-only gflags; JSON keys in fixtures MUST match these names (task 2.1 / 2.3).
DEFINE_int32(mould_test_startup_priority, 0, "launch plan parser test");
DEFINE_int32(mould_test_batch_size, 0, "launch plan parser test");
DEFINE_string(mould_test_model_path, "", "launch plan parser test");
DEFINE_bool(mould_test_enable_debug, false, "launch plan parser test");
DEFINE_double(mould_test_scale, 0.0, "launch plan parser test");
DEFINE_int32(mould_test_shared_int, 0, "launch plan parser test");
DEFINE_int32(mould_test_typecheck_int, 0, "launch plan parser test; JSON must be integral");
DEFINE_string(mould_test_unknown_gflag_key, "", "unused; for negative tests");

DEFINE_int32(mould_test_mod_a_exec_threads, 0, "fork test module A");
DEFINE_int32(mould_test_mod_b_exec_threads, 0, "fork test module B");
DEFINE_string(mould_test_mod_a_topic, "", "fork test module A");
DEFINE_string(mould_test_mod_b_topic, "", "fork test module B");
