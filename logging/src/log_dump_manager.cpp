#include "log_dump_manager.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <ctime>
#include <cstring>
#include <filesystem>
#include <glog/logging.h>
#include <regex>
#include <system_error>
#include <utility>

#ifdef MOULD_HAVE_ZLIB
#include <zlib.h>
#endif

namespace mould::comm {
namespace {

namespace fs = std::filesystem;

bool EnsureDir(const std::string& path, std::string* out_error) {
  std::error_code ec;
  if (fs::create_directories(path, ec); ec) {
    if (out_error != nullptr) {
      *out_error = "failed to create directory " + path + ": " + ec.message();
    }
    return false;
  }
  return true;
}

std::string RegexEscapePrefix(const std::string& prefix) {
  return std::regex_replace(prefix, std::regex(R"([.+*?^${}()|\[\]\\])"), R"(\$&)");
}

// 匹配: prefix_<pid>_<YYYYMMDD>_<HHMMSS>_<ffffff>(.gz)?
bool IsValidHistFile(const std::string& prefix, const std::string& name) {
  const std::string escaped = RegexEscapePrefix(prefix);
  const std::string pattern =
      "^" + escaped + R"(_\d+_\d{8}_\d{6}_\d{6}(\.gz)?$)";
  return std::regex_match(name, std::regex(pattern));
}

bool IsValidTmpFile(const std::string& prefix, const std::string& name) {
  const std::string escaped = RegexEscapePrefix(prefix);
  const std::string pattern = "^" + escaped + R"(_\d+_\d{8}_\d{6}_\d{6}\.tmp$)";
  return std::regex_match(name, std::regex(pattern));
}

std::string FormatLogStem(const std::string& prefix, pid_t pid,
                          std::chrono::system_clock::time_point now) {
  const std::time_t tt = std::chrono::system_clock::to_time_t(now);
  struct tm tm_buf;
  localtime_r(&tt, &tm_buf);
  char dt[32];
  std::strftime(dt, sizeof(dt), "%Y%m%d_%H%M%S", &tm_buf);
  const auto since_epoch = now.time_since_epoch();
  const auto sec_floor = std::chrono::duration_cast<std::chrono::seconds>(since_epoch);
  const long long micros = std::chrono::duration_cast<std::chrono::microseconds>(
                               since_epoch - sec_floor)
                               .count();
  char tail[96];
  std::snprintf(tail, sizeof(tail), "%lld_%s_%06lld", static_cast<long long>(pid), dt,
                static_cast<long long>(micros));
  return prefix + "_" + std::string(tail);
}

}  // namespace

LogDumpManager::LogDumpManager(LogDumpManagerConfig config)
    : config_(std::move(config)) {}

LogDumpManager::~LogDumpManager() {
  Stop();
}

bool LogDumpManager::Init(std::string* out_error) {
  if (!EnsureDir(config_.log_dir, out_error)) {
    return false;
  }
  if (!config_.log_tmp_dir.empty() && config_.log_tmp_dir != config_.log_dir) {
    if (!EnsureDir(config_.log_tmp_dir, out_error)) {
      return false;
    }
  }

  // Clean invalid files and scan history.
  CleanInvalidDumpFilesByFormat();
  ScanHistFiles();

  // 将上次异常退出留在 tmp 目录中的文件直接归档到 log_dir（不删除、不依赖大小阈值）。
  ArchiveTmpLeftovers();
  ScanHistFiles();

  // Open first file.
  OpenNewFile();

  // Start write thread.
  write_thread_ = std::thread(&LogDumpManager::WriteThreadLoop, this);

  // Start compression threads if enabled.
  if (config_.compression_enabled) {
    for (unsigned int i = 0; i < config_.compression_threads; ++i) {
      compress_threads_.emplace_back(&LogDumpManager::CompressThreadLoop, this);
    }
  }

  return true;
}

void LogDumpManager::AddLog(const std::string& module_name, LogEntry line) {
  if (stopped_.load(std::memory_order_acquire)) {
    return;
  }
  LogItem item;
  item.module_name = module_name;
  item.content = std::move(line);

  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (queue_.size() >= config_.queue_size) {
      DropOne(config_.drop_newest_when_full);
    }
    queue_.push_back(std::move(item));
  }
  queue_cv_.notify_one();
}

void LogDumpManager::AddLog(const std::string& module_name, std::string line) {
  AddLog(module_name, std::make_shared<const std::string>(std::move(line)));
}

void LogDumpManager::Stop() {
  stopped_.store(true, std::memory_order_release);
  queue_cv_.notify_all();
  if (write_thread_.joinable()) {
    write_thread_.join();
  }
  compress_stopped_.store(true, std::memory_order_release);
  compress_cv_.notify_all();
  for (auto& t : compress_threads_) {
    if (t.joinable()) {
      t.join();
    }
  }
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

void LogDumpManager::FlushBarrier() {
  std::unique_lock<std::mutex> lock(queue_mutex_);
  auto barrier_before = queue_.size();
  lock.unlock();
  queue_cv_.notify_all();

  // Spin until queue is fully processed.
  // A proper implementation would use a waitable barrier counter;
  // for correctness we spin-sleep.
  constexpr auto kSpinInterval = std::chrono::milliseconds(5);
  while (!stopped_.load(std::memory_order_acquire)) {
    lock.lock();
    bool done = queue_.empty() && (barrier_before == 0 || queue_.size() < barrier_before);
    lock.unlock();
    if (done) break;
    std::this_thread::sleep_for(kSpinInterval);
  }

  // fsync the current file.
  if (fd_ >= 0) {
    ::fsync(fd_);
  }
}

void LogDumpManager::WriteThreadLoop() {
  std::vector<LogItem> batch;
  batch.reserve(256);

  while (!stopped_.load(std::memory_order_acquire)) {
    {
      std::unique_lock<std::mutex> lock(queue_mutex_);
      queue_cv_.wait_for(lock, config_.flush_interval, [this]() {
        return stopped_.load(std::memory_order_acquire) || !queue_.empty();
      });
      if (queue_.empty()) {
        continue;
      }
      batch.assign(std::make_move_iterator(queue_.begin()),
                   std::make_move_iterator(queue_.end()));
      queue_.clear();
    }

    for (auto& item : batch) {
      if (stopped_.load(std::memory_order_acquire)) {
        break;
      }
      if (ShouldRoll()) {
        DoRoll();
      }

      // Format: {module}\t>> {content}\n
      std::string formatted = item.module_name + "\t>> ";
      formatted.append(*item.content);
      if (formatted.back() != '\n') {
        formatted.push_back('\n');
      }

      if (fd_ >= 0) {
        const char* data = formatted.data();
        std::size_t remaining = formatted.size();
        while (remaining > 0) {
          ssize_t written = ::write(fd_, data, remaining);
          if (written > 0) {
            current_file_size_ += static_cast<std::size_t>(written);
            written_bytes_.fetch_add(static_cast<std::size_t>(written), std::memory_order_relaxed);
            data += written;
            remaining -= static_cast<std::size_t>(written);
          } else if (written < 0 && errno != EINTR) {
            break;
          }
        }
      }
    }
    batch.clear();
  }

  // Flush remaining
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    batch.assign(std::make_move_iterator(queue_.begin()),
                 std::make_move_iterator(queue_.end()));
    queue_.clear();
  }
  for (auto& item : batch) {
    if (ShouldRoll()) {
      DoRoll();
    }
    std::string formatted = item.module_name + "\t>> ";
    formatted.append(*item.content);
    if (formatted.back() != '\n') {
      formatted.push_back('\n');
    }
    if (fd_ >= 0) {
      const char* data = formatted.data();
      std::size_t remaining = formatted.size();
      while (remaining > 0) {
        ssize_t written = ::write(fd_, data, remaining);
        if (written > 0) {
          current_file_size_ += static_cast<std::size_t>(written);
          written_bytes_.fetch_add(static_cast<std::size_t>(written), std::memory_order_relaxed);
          data += written;
          remaining -= static_cast<std::size_t>(written);
        } else if (written < 0 && errno != EINTR) {
          break;
        }
      }
    }
  }
}

bool LogDumpManager::ShouldRoll() const {
  return current_file_size_ >= config_.max_file_size;
}

void LogDumpManager::OpenNewFile() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }

  std::string tmp_path = GenTempFilePath();
  current_file_path_ = tmp_path;

  fd_ = ::open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd_ < 0) {
    LOG(WARNING) << "[LogDumpManager] failed to open log file " << tmp_path
                 << " errno=" << errno;
    current_file_size_ = 0;
    return;
  }
  current_file_size_ = 0;
}

void LogDumpManager::DoRoll() {
  if (fd_ < 0) {
    return;
  }

  // Flush and fsync before renaming.
  ::fsync(fd_);
  ::close(fd_);
  fd_ = -1;

  std::string tmp_path = current_file_path_;
  RenameToFinal(tmp_path);

  OpenNewFile();
}

void LogDumpManager::RenameToFinal(const std::string& tmp_path) {
  std::string final_path = GenFinalFilePath();

  std::error_code ec;
  fs::rename(tmp_path, final_path, ec);
  if (ec) {
    // Fallback: rename failed, try copy + delete.
    LOG(WARNING) << "[LogDumpManager] rename failed from " << tmp_path
                 << " to " << final_path << ": " << ec.message();
    fs::copy_file(tmp_path, final_path, ec);
    fs::remove(tmp_path, ec);
  }

  hist_files_.push_back(final_path);

  // Compress if enabled.
  if (config_.compression_enabled) {
    EnqueueCompress(final_path);
  }

  // Trim old files.
  while (hist_files_.size() > config_.max_file_count) {
    const auto& oldest = hist_files_.front();
    fs::remove(oldest, ec);
    hist_files_.pop_front();
  }
}

void LogDumpManager::CleanInvalidDumpFilesByFormat() {
  std::error_code ec;
  if (!fs::exists(config_.log_dir, ec)) {
    return;
  }
  for (auto& entry : fs::directory_iterator(config_.log_dir, ec)) {
    if (ec) break;
    if (!entry.is_regular_file()) continue;
    const std::string name = entry.path().filename().string();
    if (!IsValidHistFile(config_.file_name_prefix, name)) {
      fs::remove(entry.path(), ec);
    }
  }
}

void LogDumpManager::ScanHistFiles() {
  hist_files_.clear();
  std::error_code ec;
  if (!fs::exists(config_.log_dir, ec)) {
    return;
  }
  for (auto& entry : fs::directory_iterator(config_.log_dir, ec)) {
    if (ec) break;
    if (!entry.is_regular_file()) continue;
    const std::string name = entry.path().filename().string();
    if (IsValidHistFile(config_.file_name_prefix, name)) {
      hist_files_.push_back(entry.path().string());
    }
  }
  hist_files_.sort();
}

void LogDumpManager::ArchiveTmpLeftovers() {
  if (config_.log_tmp_dir.empty() || config_.log_tmp_dir == config_.log_dir) {
    return;
  }
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(config_.log_tmp_dir, ec)) {
    if (ec) {
      break;
    }
    if (!entry.is_regular_file()) {
      continue;
    }
    const std::string name = entry.path().filename().string();
    if (!IsValidTmpFile(config_.file_name_prefix, name)) {
      continue;
    }
    const std::string final_name = name.substr(0, name.size() - 4);  // 去掉 ".tmp"
    fs::path dest = fs::path(config_.log_dir) / final_name;
    const fs::path src = entry.path();
    if (fs::exists(dest, ec)) {
      dest = GenFinalFilePath();
    }
    ec.clear();
    fs::rename(src, dest, ec);
    if (ec) {
      LOG(WARNING) << "[LogDumpManager] archive tmp leftover failed " << src.string() << " -> "
                   << dest.string() << ": " << ec.message();
    }
  }
}

std::string LogDumpManager::GenTempFilePath() {
  return config_.log_tmp_dir.empty()
             ? GenFinalFilePath()
             : (fs::path(config_.log_tmp_dir) / GenTempFileName()).string();
}

std::string LogDumpManager::GenFinalFilePath() {
  return (fs::path(config_.log_dir) / AllocUniqueStem()).string();
}

std::string LogDumpManager::GenTempFileName() {
  return AllocUniqueStem() + ".tmp";
}

bool LogDumpManager::StemIsAvailable(const std::string& stem) const {
  std::error_code ec;
  const fs::path log_d(config_.log_dir);
  if (fs::exists(log_d / stem, ec)) {
    return false;
  }
  if (fs::exists(log_d / (stem + ".gz"), ec)) {
    return false;
  }
  if (!config_.log_tmp_dir.empty()) {
    const fs::path tmp_d(config_.log_tmp_dir);
    if (fs::exists(tmp_d / (stem + ".tmp"), ec)) {
      return false;
    }
  }
  return true;
}

std::string LogDumpManager::AllocUniqueStem() const {
  const pid_t pid = ::getpid();
  for (int bump = 0; bump < 1'000'000; ++bump) {
    const auto now =
        std::chrono::system_clock::now() + std::chrono::microseconds(bump);
    const std::string stem = FormatLogStem(config_.file_name_prefix, pid, now);
    if (StemIsAvailable(stem)) {
      return stem;
    }
  }
  LOG(WARNING) << "[LogDumpManager] AllocUniqueStem exhausted collision retries; using current time";
  return FormatLogStem(config_.file_name_prefix, pid, std::chrono::system_clock::now());
}

void LogDumpManager::EnqueueCompress(const std::string& file_path) {
  // Only compress non-.gz files.
  if (file_path.size() > 3 && file_path.substr(file_path.size() - 3) == ".gz") {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(compress_mutex_);
    compress_queue_.push_back(file_path);
  }
  compress_cv_.notify_one();
}

void LogDumpManager::CompressThreadLoop() {
  constexpr std::size_t kBufSize = 65536;
  auto buf = std::make_unique<char[]>(kBufSize);

  while (!compress_stopped_.load(std::memory_order_acquire)) {
    std::string file_path;
    {
      std::unique_lock<std::mutex> lock(compress_mutex_);
      compress_cv_.wait_for(lock, std::chrono::seconds(1), [this]() {
        return compress_stopped_.load(std::memory_order_acquire) || !compress_queue_.empty();
      });
      if (compress_queue_.empty()) {
        continue;
      }
      file_path = std::move(compress_queue_.front());
      compress_queue_.pop_front();
    }

    std::string gz_path = file_path + ".gz";
#ifdef MOULD_HAVE_ZLIB
    gzFile gz = gzopen(gz_path.c_str(), "wb");
    if (gz == nullptr) {
      LOG(WARNING) << "[LogDumpManager] failed to open gz file " << gz_path;
      continue;
    }

    int src_fd = ::open(file_path.c_str(), O_RDONLY);
    if (src_fd < 0) {
      LOG(WARNING) << "[LogDumpManager] failed to open source " << file_path
                   << " for compression";
      gzclose(gz);
      continue;
    }

    ssize_t n;
    while ((n = ::read(src_fd, buf.get(), kBufSize)) > 0) {
      if (gzwrite(gz, buf.get(), static_cast<unsigned int>(n)) <= 0) {
        break;
      }
    }
    ::close(src_fd);
    gzclose(gz);
#else
    // Fallback: shell gzip via popen.
    std::string cmd = "gzip -c \"" + file_path + "\" > \"" + gz_path + "\" 2>/dev/null";
    int rc = std::system(cmd.c_str());
    if (rc != 0) {
      LOG(WARNING) << "[LogDumpManager] gzip failed for " << file_path << " rc=" << rc;
      continue;
    }
#endif

    // Remove original on success if gz file exists.
    std::error_code ec;
    if (fs::exists(gz_path, ec) && fs::file_size(gz_path, ec) > 0) {
      fs::remove(file_path, ec);
      // Update hist entry: replace file_path with gz_path.
      std::lock_guard<std::mutex> lock(queue_mutex_);
      for (auto& h : hist_files_) {
        if (h == file_path) {
          h = gz_path;
          break;
        }
      }
    }
  }
}

void LogDumpManager::DropOne(bool newest) {
  // Increment drop count for the dropped item's module.
  if (!queue_.empty()) {
    if (newest) {
      IncrementDropCount(queue_.back().module_name);
      queue_.pop_back();
    } else {
      IncrementDropCount(queue_.front().module_name);
      queue_.pop_front();
    }
  }
}

void LogDumpManager::IncrementDropCount(const std::string& module_name) {
  std::lock_guard<std::mutex> lock(drop_mutex_);
  drop_counts_[module_name] += 1;
  total_dropped_ += 1;
}

std::uint64_t LogDumpManager::DroppedCount(const std::string& module_name) const {
  std::lock_guard<std::mutex> lock(drop_mutex_);
  auto it = drop_counts_.find(module_name);
  return it != drop_counts_.end() ? it->second : 0;
}

std::uint64_t LogDumpManager::TotalDroppedCount() const {
  std::lock_guard<std::mutex> lock(drop_mutex_);
  return total_dropped_;
}

std::uint64_t LogDumpManager::QueueSize() const {
  std::lock_guard<std::mutex> lock(queue_mutex_);
  return queue_.size();
}

}  // namespace mould::comm
