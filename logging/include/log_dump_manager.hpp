#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace mould::comm {

using LogEntry = std::shared_ptr<const std::string>;

struct LogDumpManagerConfig {
  std::string log_dir;
  /// 与 log_dir 不同时，写入中的日志先落盘到此目录，滚动时再 rename 到 log_dir。
  std::string log_tmp_dir;
  std::size_t queue_size = 4096;
  bool drop_newest_when_full = false;
  std::size_t max_file_size = 10 * 1024 * 1024;
  std::size_t max_file_count = 10;
  std::string file_name_prefix = "mould_log";
  bool compression_enabled = false;
  unsigned int compression_threads = 1;
  std::chrono::milliseconds flush_interval{1000};
};

class LogDumpManager {
 public:
  explicit LogDumpManager(LogDumpManagerConfig config);
  ~LogDumpManager();

  LogDumpManager(const LogDumpManager&) = delete;
  LogDumpManager& operator=(const LogDumpManager&) = delete;

  bool Init(std::string* out_error);

  void AddLog(const std::string& module_name, LogEntry line);
  void AddLog(const std::string& module_name, std::string line);

  void Stop();
  void FlushBarrier();

  std::uint64_t DroppedCount(const std::string& module_name) const;
  std::uint64_t TotalDroppedCount() const;
  std::uint64_t QueueSize() const;

 private:
  struct LogItem {
    std::string module_name;
    LogEntry content;
  };

  void WriteThreadLoop();

  bool ShouldRoll() const;
  void OpenNewFile();
  void DoRoll();
  void RenameToFinal(const std::string& tmp_path);

  void CleanInvalidDumpFilesByFormat();
  void ScanHistFiles();
  /// 将崩溃等遗留的 .tmp 直接 rename 到归档目录（不删、不按大小阈值）。
  void ArchiveTmpLeftovers();

  std::string GenTempFilePath();
  std::string GenFinalFilePath();
  std::string GenTempFileName();
  std::string AllocUniqueStem() const;
  bool StemIsAvailable(const std::string& stem) const;

  void EnqueueCompress(const std::string& file_path);
  void CompressThreadLoop();

  void DropOne(bool newest);
  void IncrementDropCount(const std::string& module_name);

  LogDumpManagerConfig config_;

  std::atomic<bool> stopped_{false};
  std::thread write_thread_;

  mutable std::mutex queue_mutex_;
  std::condition_variable queue_cv_;
  std::deque<LogItem> queue_;

  std::atomic<bool> compress_stopped_{false};
  std::vector<std::thread> compress_threads_;
  std::mutex compress_mutex_;
  std::condition_variable compress_cv_;
  std::deque<std::string> compress_queue_;

  int fd_ = -1;
  std::string current_file_path_;
  std::size_t current_file_size_ = 0;
  std::list<std::string> hist_files_;
  std::atomic<std::uint64_t> written_bytes_{0};

  mutable std::mutex drop_mutex_;
  std::unordered_map<std::string, std::uint64_t> drop_counts_;
  std::uint64_t total_dropped_ = 0;
};

}  // namespace mould::comm
