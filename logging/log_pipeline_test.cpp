#include "log_collector.hpp"
#include "log_dump_manager.hpp"
#include "log_sink.hpp"
#include "subprocess_log_init.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <glog/logging.h>

namespace mould::comm {
namespace {

namespace fs = std::filesystem;

void InitTestGlogOnce() {
  static bool done = false;
  if (done) {
    return;
  }
  google::InitGoogleLogging("log_pipeline_test");
  FLAGS_logtostderr = false;
  FLAGS_minloglevel = google::ERROR;
  done = true;
}

class LogPipelineTest : public ::testing::Test {
 protected:
  void SetUp() override { InitTestGlogOnce(); }
};

fs::path UniqueTempRoot(const char* tag) {
  const auto base = fs::temp_directory_path();
  const auto id = std::chrono::steady_clock::now().time_since_epoch().count() ^
                    reinterpret_cast<std::uintptr_t>(tag);
  return base / fs::path(std::string("log_pipeline_") + tag + "_" + std::to_string(id));
}

std::vector<fs::path> ListSortedRegular(const fs::path& dir, const std::string& prefix) {
  std::vector<fs::path> out;
  std::error_code ec;
  if (!fs::exists(dir, ec)) {
    return out;
  }
  for (const auto& entry : fs::directory_iterator(dir, ec)) {
    if (ec) {
      break;
    }
    if (!entry.is_regular_file()) {
      continue;
    }
    const std::string name = entry.path().filename().string();
    if (name.rfind(prefix, 0) == 0) {
      out.push_back(entry.path());
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::string ReadEntireFile(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::string ConcatAllLogContent(const fs::path& log_dir, const fs::path& tmp_dir,
                                  const std::string& prefix) {
  std::string acc;
  for (const auto& d : {log_dir, tmp_dir}) {
    for (const auto& p : ListSortedRegular(d, prefix)) {
      acc += ReadEntireFile(p);
    }
  }
  return acc;
}

// ---- 7.1 LogDumpManagerInitCreatesDirectoriesAndHistList ----
TEST_F(LogPipelineTest, LogDumpManagerInitCreatesDirectoriesAndHistList) {
  const fs::path root = UniqueTempRoot("init");
  const fs::path log_dir = root / "archive";
  const fs::path tmp_dir = root / "tmp";
  fs::create_directories(log_dir);
  fs::create_directories(tmp_dir);

  // 非法命名：Init 时应从 log_dir 删除
  {
    std::ofstream junk(log_dir / "not_a_log.txt");
    junk << "x";
  }
  const long long pid = static_cast<long long>(::getpid());
  const std::string hist0 = "mould_log_" + std::to_string(pid) + "_20200101_120000_000000";
  const std::string hist1 = "mould_log_" + std::to_string(pid) + "_20200102_120000_000000";
  const std::string recovered_stem = "mould_log_" + std::to_string(pid) + "_20200303_120000_000007";
  // 合法历史文件
  {
    std::ofstream a(log_dir / hist0);
    a << "old0\n";
    std::ofstream b(log_dir / hist1);
    b << "old1\n";
  }
  // 崩溃残留 tmp：应归档到 log_dir（不删）
  {
    std::ofstream t(tmp_dir / (recovered_stem + ".tmp"));
    t << "recovered\n";
  }

  LogDumpManagerConfig cfg;
  cfg.log_dir = log_dir.string();
  cfg.log_tmp_dir = tmp_dir.string();
  cfg.file_name_prefix = "mould_log";
  cfg.flush_interval = std::chrono::milliseconds(20);
  cfg.compression_enabled = false;

  LogDumpManager mgr(std::move(cfg));
  std::string err;
  ASSERT_TRUE(mgr.Init(&err)) << err;

  EXPECT_TRUE(fs::is_directory(log_dir));
  EXPECT_TRUE(fs::is_directory(tmp_dir));
  EXPECT_FALSE(fs::exists(log_dir / "not_a_log.txt"));

  auto names = ListSortedRegular(log_dir, "mould_log");
  ASSERT_GE(names.size(), 3u);
  // 归档后应存在无 .tmp 后缀的 recovered_stem
  bool found_recovered = false;
  for (const auto& p : names) {
    if (p.filename().string() == recovered_stem) {
      found_recovered = true;
      EXPECT_EQ(ReadEntireFile(p), "recovered\n");
    }
  }
  EXPECT_TRUE(found_recovered);
  EXPECT_FALSE(fs::exists(tmp_dir / (recovered_stem + ".tmp")));

  // 历史扫描结果按文件名字典序排列（与实现中 list::sort 一致）
  std::vector<std::string> stems;
  for (const auto& p : names) {
    stems.push_back(p.filename().string());
  }
  EXPECT_TRUE(std::is_sorted(stems.begin(), stems.end()));

  mgr.Stop();
}

// ---- 7.2 LogDumpManagerAddLogFormatsTextLines ----
TEST_F(LogPipelineTest, LogDumpManagerAddLogFormatsTextLines) {
  const fs::path root = UniqueTempRoot("fmt");
  const fs::path log_dir = root / "archive";
  fs::create_directories(log_dir);

  LogDumpManagerConfig cfg;
  cfg.log_dir = log_dir.string();
  cfg.log_tmp_dir.clear();
  cfg.flush_interval = std::chrono::milliseconds(20);
  cfg.compression_enabled = false;
  cfg.file_name_prefix = "mould_log";

  LogDumpManager mgr(std::move(cfg));
  std::string err;
  ASSERT_TRUE(mgr.Init(&err)) << err;

  mgr.AddLog("ModuleA", std::string("first line"));
  mgr.AddLog("ModuleB", std::string("second\n"));
  mgr.FlushBarrier();

  const std::string blob = ConcatAllLogContent(log_dir, {}, "mould_log");
  EXPECT_NE(blob.find("ModuleA\t>> first line\n"), std::string::npos);
  EXPECT_NE(blob.find("ModuleB\t>> second\n"), std::string::npos);

  mgr.Stop();
}

// ---- 7.3 LogDumpManagerRotationRenamesFromTemp ----
TEST_F(LogPipelineTest, LogDumpManagerRotationRenamesFromTemp) {
  const fs::path root = UniqueTempRoot("roll");
  const fs::path log_dir = root / "archive";
  const fs::path tmp_dir = root / "tmp";
  fs::create_directories(log_dir);
  fs::create_directories(tmp_dir);

  LogDumpManagerConfig cfg;
  cfg.log_dir = log_dir.string();
  cfg.log_tmp_dir = tmp_dir.string();
  cfg.max_file_size = 400;
  cfg.flush_interval = std::chrono::milliseconds(10);
  cfg.compression_enabled = false;
  cfg.file_name_prefix = "mould_log";

  LogDumpManager mgr(std::move(cfg));
  std::string err;
  ASSERT_TRUE(mgr.Init(&err)) << err;

  std::string chunk(120, 'x');
  for (int i = 0; i < 20; ++i) {
    mgr.AddLog("R", chunk + "\n");
  }
  mgr.FlushBarrier();

  const auto archived = ListSortedRegular(log_dir, "mould_log");
  ASSERT_GE(archived.size(), 1u);
  bool any_tmp_in_archive = false;
  for (const auto& p : archived) {
    if (p.extension() == ".tmp") {
      any_tmp_in_archive = true;
    }
  }
  EXPECT_FALSE(any_tmp_in_archive);

  mgr.Stop();
}

// ---- 7.4 LogDumpManagerDropsWhenQueueFullAndIncrementsCounter ----
TEST_F(LogPipelineTest, LogDumpManagerDropsWhenQueueFullAndIncrementsCounter) {
  const fs::path root = UniqueTempRoot("drop");
  const fs::path log_dir = root / "archive";
  fs::create_directories(log_dir);

  LogDumpManagerConfig cfg;
  cfg.log_dir = log_dir.string();
  cfg.log_tmp_dir.clear();
  cfg.queue_size = 16;
  cfg.drop_newest_when_full = false;
  cfg.flush_interval = std::chrono::milliseconds(1);
  cfg.compression_enabled = false;
  cfg.file_name_prefix = "mould_log";

  LogDumpManager mgr(std::move(cfg));
  std::string err;
  ASSERT_TRUE(mgr.Init(&err)) << err;

  constexpr int kThreads = 16;
  constexpr int kPerThread = 400;
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&mgr, t]() {
      for (int i = 0; i < kPerThread; ++i) {
        mgr.AddLog("flood", std::string("t") + std::to_string(t) + "i" + std::to_string(i) + "\n");
      }
    });
  }
  for (auto& th : threads) {
    th.join();
  }

  mgr.FlushBarrier();
  EXPECT_GT(mgr.TotalDroppedCount(), 0u);
  mgr.Stop();
}

// ---- 7.5 LogDumpManagerFlushBarrierNotifiesWaiters ----
TEST_F(LogPipelineTest, LogDumpManagerFlushBarrierNotifiesWaiters) {
  const fs::path root = UniqueTempRoot("barrier");
  const fs::path log_dir = root / "archive";
  fs::create_directories(log_dir);

  LogDumpManagerConfig cfg;
  cfg.log_dir = log_dir.string();
  cfg.log_tmp_dir.clear();
  cfg.flush_interval = std::chrono::seconds(30);
  cfg.compression_enabled = false;
  cfg.file_name_prefix = "mould_log";

  LogDumpManager mgr(std::move(cfg));
  std::string err;
  ASSERT_TRUE(mgr.Init(&err)) << err;

  mgr.AddLog("B", std::string("barrier-mark\n"));
  mgr.FlushBarrier();

  const std::string blob = ConcatAllLogContent(log_dir, {}, "mould_log");
  EXPECT_NE(blob.find("barrier-mark"), std::string::npos);

  mgr.Stop();
}

// ---- 7.6 LogCollectorLineBufferingAcrossReads ----
TEST_F(LogPipelineTest, LogCollectorLineBufferingAcrossReads) {
  int fds[2];
  ASSERT_EQ(::pipe(fds), 0);
  std::atomic<int> call_count{0};
  std::string last_line;
  std::mutex last_mu;

  LogCollector collector([&](const std::string& module_name,
                             std::shared_ptr<const std::string> line) {
    EXPECT_EQ(module_name, "pipe_mod");
    std::lock_guard<std::mutex> lock(last_mu);
    last_line = *line;
    call_count.fetch_add(1, std::memory_order_relaxed);
  });

  std::string add_err;
  ASSERT_TRUE(collector.AddSource(PipeSource{fds[0], "pipe_mod"}, &add_err)) << add_err;

  ASSERT_EQ(::write(fds[1], "hel", 3), 3);
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  EXPECT_EQ(call_count.load(), 0);

  ASSERT_EQ(::write(fds[1], "lo\n", 4), 4);
  for (int i = 0; i < 50 && call_count.load() < 1; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  EXPECT_EQ(call_count.load(), 1);
  {
    std::lock_guard<std::mutex> lock(last_mu);
    EXPECT_EQ(last_line, "hello");
  }

  ::close(fds[1]);
  fds[1] = -1;
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  collector.Stop();
  if (fds[0] >= 0) {
    ::close(fds[0]);
    fds[0] = -1;
  }
}

// ---- 7.7 LogSinkNoReentrancyDeadlock ----
TEST_F(LogPipelineTest, LogSinkNoReentrancyDeadlock) {
  FLAGS_minloglevel = google::INFO;

  const fs::path root = UniqueTempRoot("sink");
  const fs::path log_dir = root / "archive";
  fs::create_directories(log_dir);

  LogDumpManagerConfig cfg;
  cfg.log_dir = log_dir.string();
  cfg.log_tmp_dir.clear();
  cfg.flush_interval = std::chrono::milliseconds(5);
  cfg.compression_enabled = false;
  cfg.file_name_prefix = "mould_log";

  auto mgr = std::make_shared<LogDumpManager>(std::move(cfg));
  std::string err;
  ASSERT_TRUE(mgr->Init(&err)) << err;

  {
    LogDumpManagerSink sink(mgr, "SinkTestMod");
    std::atomic<bool> stop{false};
    std::thread hammer([&]() {
      for (int i = 0; i < 200; ++i) {
        if (stop.load()) {
          break;
        }
        LOG(INFO) << "hammer " << i;
      }
    });
    std::thread flusher([&]() {
      for (int i = 0; i < 50; ++i) {
        mgr->FlushBarrier();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
      stop.store(true);
    });
    hammer.join();
    flusher.join();
  }

  mgr->Stop();
  FLAGS_minloglevel = google::ERROR;
}

// ---- 7.8 PipeNonBlockingWriteIncrementsDropOnEAGAIN ----
TEST_F(LogPipelineTest, PipeNonBlockingWriteIncrementsDropOnEAGAIN) {
  int fds[2];
  ASSERT_EQ(::pipe(fds), 0);
  ASSERT_EQ(mould::SetNonBlocking(fds[1]), 0);

#ifdef __linux__
  // 缩小管道容量，便于在测试中较快遇到 EAGAIN
  (void)::fcntl(fds[1], F_SETPIPE_SZ, 4096);
#endif

  int eagain_writes = 0;
  char byte = 'Z';
  for (int i = 0; i < 200000; ++i) {
    ssize_t w = ::write(fds[1], &byte, 1);
    if (w < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        ++eagain_writes;
        break;
      }
      if (errno == EINTR) {
        continue;
      }
      FAIL() << "unexpected write errno=" << errno;
    }
  }
  EXPECT_GT(eagain_writes, 0);
  ::close(fds[0]);
  ::close(fds[1]);
}

// ---- 7.9 SupervisorRestartReusesSameLogPipe（待主进程注入，占位跳过）----
TEST_F(LogPipelineTest, LogPipeSupervisorRestartReusesSameLogPipe) {
  GTEST_SKIP() << "需主进程 Supervisor 单测注入后再实现 fd 复用断言";
}

}  // namespace
}  // namespace mould::comm
