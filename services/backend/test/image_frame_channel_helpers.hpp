#pragma once

#include "interfaces.hpp"
#include "image_frame.pb.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <glog/logging.h>
#include <string>
#include <vector>

namespace mould::backend::image_frame_channel {

inline constexpr const char* kMvtecBottleGoodImageDir =
    "/media/honeywell/D/bhy/dataset/MVTec/bottle/train/good";
inline constexpr const char* kReceivedImageSubdir = "mould_backend_received_images";

inline std::filesystem::path ReceivedImagesDir() {
  return std::filesystem::current_path() / kReceivedImageSubdir;
}

inline bool IsImageExtension(const std::filesystem::path& p) {
  const std::string ext = p.extension().string();
  if (ext.size() < 2) {
    return false;
  }
  std::string lower;
  lower.reserve(ext.size());
  for (unsigned char c : ext) {
    lower.push_back(static_cast<char>(std::tolower(c)));
  }
  return lower == ".png" || lower == ".jpg" || lower == ".jpeg" || lower == ".bmp" || lower == ".webp";
}

inline std::vector<std::filesystem::path> ListDatasetImagePaths(const std::filesystem::path& root) {
  std::vector<std::filesystem::path> out;
  std::error_code ec;
  if (!std::filesystem::exists(root, ec) || ec) {
    LOG(ERROR) << "[image_frame_channel] dataset root missing or inaccessible path=" << root.string()
               << " ec=" << ec.message();
    return out;
  }
  for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
    if (ec) {
      break;
    }
    if (!entry.is_regular_file()) {
      continue;
    }
    const auto& p = entry.path();
    if (IsImageExtension(p)) {
      out.push_back(p);
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

inline mould::common::image::ImageEncoding EncodingForPath(const std::filesystem::path& path) {
  std::string ext = path.extension().string();
  for (char& c : ext) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  if (ext == ".png") {
    return mould::common::image::IMAGE_ENCODING_PNG;
  }
  if (ext == ".jpg" || ext == ".jpeg") {
    return mould::common::image::IMAGE_ENCODING_JPEG;
  }
  return mould::common::image::IMAGE_ENCODING_RAW;
}

inline bool ReadFileBytes(const std::filesystem::path& path, std::string* out_bytes) {
  if (out_bytes == nullptr) {
    return false;
  }
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) {
    return false;
  }
  const auto end = in.tellg();
  if (end <= 0) {
    return false;
  }
  in.seekg(0, std::ios::beg);
  out_bytes->assign(static_cast<std::size_t>(end), '\0');
  in.read(out_bytes->data(), end);
  return static_cast<bool>(in);
}

inline bool FillImageFrameFromFile(const std::filesystem::path& path, std::uint64_t seq,
                                   mould::common::image::ImageFrame* frame) {
  if (frame == nullptr) {
    return false;
  }
  std::string bytes;
  if (!ReadFileBytes(path, &bytes)) {
    LOG(WARNING) << "[image_frame_channel] failed to read image file=" << path.string();
    return false;
  }
  const auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
  frame->Clear();
  frame->set_frame_seq(seq);
  frame->set_capture_ts_us(static_cast<std::uint64_t>(now_us));
  frame->set_width(0);
  frame->set_height(0);
  frame->set_channels(3);
  frame->set_format(path.filename().string());
  frame->set_encoding(EncodingForPath(path));
  frame->set_checksum_type("none");
  frame->set_checksum_value("");
  frame->set_lamination_detect_flag(false);
  frame->set_image_bytes(std::move(bytes));
  return true;
}

inline mould::comm::ByteBuffer SerializeImageFrame(const mould::common::image::ImageFrame& frame) {
  const std::string serialized = frame.SerializeAsString();
  return mould::comm::ByteBuffer(serialized.begin(), serialized.end());
}

inline bool SaveReceivedImageFrameFromMessage(const mould::common::image::ImageFrame& frame,
                                              const std::string& module_name,
                                              std::filesystem::path* out_saved_path = nullptr) {
  if (frame.image_bytes().empty()) {
    LOG(WARNING) << "[" << module_name << "] ImageFrame has empty image_bytes";
    return false;
  }
  std::error_code ec;
  const auto dir = ReceivedImagesDir();
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    LOG(ERROR) << "[" << module_name << "] create_directories failed path=" << dir.string() << " ec=" << ec.message();
    return false;
  }
  std::string base = frame.format();
  if (base.empty()) {
    base = "frame_" + std::to_string(frame.frame_seq()) + ".bin";
  }
  const std::filesystem::path orig_as_path(base);
  const std::string stem = orig_as_path.stem().string();
  const std::string ext = orig_as_path.extension().string();
  const std::string ext_use = ext.empty() ? std::string(".bin") : ext;
  const auto out_path = dir / (stem + "_" + module_name + ext_use);
  std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
  if (!out) {
    LOG(ERROR) << "[" << module_name << "] open output failed path=" << out_path.string();
    return false;
  }
  out.write(frame.image_bytes().data(), static_cast<std::streamsize>(frame.image_bytes().size()));
  if (!out) {
    LOG(ERROR) << "[" << module_name << "] write output failed path=" << out_path.string();
    return false;
  }
  LOG(INFO) << "[" << module_name << "] saved received image to " << out_path.string() << " bytes="
            << frame.image_bytes().size();
  if (out_saved_path != nullptr) {
    *out_saved_path = out_path;
  }
  return true;
}

inline bool SaveReceivedImageFrame(const mould::comm::ByteBuffer& payload, const std::string& module_name,
                                   std::filesystem::path* out_saved_path = nullptr) {
  mould::common::image::ImageFrame frame;
  if (!frame.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
    LOG(WARNING) << "[" << module_name << "] ImageFrame protobuf parse failed payload_size=" << payload.size();
    return false;
  }
  return SaveReceivedImageFrameFromMessage(frame, module_name, out_saved_path);
}

}  // namespace mould::backend::image_frame_channel
