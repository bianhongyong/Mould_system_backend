#include <gtest/gtest.h>

#include <google/protobuf/any.pb.h>
#include <google/protobuf/util/message_differencer.h>

#include <string>

#include "gateway_frame_ingress.pb.h"
#include "image_meta_extensions/defect_detection.pb.h"
#include "image_meta_extensions/quality_inspection.pb.h"
#include "image_meta_extensions/surface_metering.pb.h"

namespace mould {
namespace gateway {
namespace scenario = ::mould::gateway::scenario;
namespace {

// ============================================================
// 场景扩展序列化/反序列化测试
// ============================================================

TEST(ImageMetaExtensionsTest, PackAndUnpackDefectDetection) {
    // 创建扩展消息
    scenario::DefectDetection defect;
    defect.set_product_type("type-A");
    defect.set_confidence_threshold(0.85);
    defect.add_regions_of_interest("region-01");
    defect.add_regions_of_interest("region-02");

    // 包成 Any
    google::protobuf::Any any;
    any.PackFrom(defect);

    // 塞入 ImageFrameIngress
    ImageFrameIngress frame;
    frame.set_request_id("gw-test-001");
    frame.set_node_id("camera-01");
    frame.set_capture_time("2026-05-09T10:00:00Z");
    frame.set_image_format("jpeg");
    frame.set_image_data("fake_image_bytes");
    *frame.add_scenario_extensions() = any;

    // 序列化 → 反序列化
    std::string serialized = frame.SerializeAsString();
    ImageFrameIngress parsed;
    ASSERT_TRUE(parsed.ParseFromString(serialized));

    // 验证公共字段
    EXPECT_EQ(parsed.request_id(), "gw-test-001");
    EXPECT_EQ(parsed.node_id(), "camera-01");
    EXPECT_EQ(parsed.capture_time(), "2026-05-09T10:00:00Z");
    EXPECT_EQ(parsed.image_format(), "jpeg");
    EXPECT_EQ(parsed.image_data(), "fake_image_bytes");

    // 验证扩展
    ASSERT_EQ(parsed.scenario_extensions_size(), 1);
    EXPECT_TRUE(parsed.scenario_extensions(0).Is<scenario::DefectDetection>());

    scenario::DefectDetection unpacked;
    ASSERT_TRUE(parsed.scenario_extensions(0).UnpackTo(&unpacked));
    EXPECT_EQ(unpacked.product_type(), "type-A");
    EXPECT_DOUBLE_EQ(unpacked.confidence_threshold(), 0.85);
    ASSERT_EQ(unpacked.regions_of_interest_size(), 2);
    EXPECT_EQ(unpacked.regions_of_interest(0), "region-01");
    EXPECT_EQ(unpacked.regions_of_interest(1), "region-02");
}

TEST(ImageMetaExtensionsTest, MultipleExtensions) {
    // 同时打包多个场景扩展
    scenario::DefectDetection defect;
    defect.set_product_type("type-B");
    defect.set_confidence_threshold(0.95);

    scenario::QualityInspection quality;
    quality.set_batch_id(20260509);
    quality.set_inspection_standard("v2.1");

    google::protobuf::Any any_defect, any_quality;
    any_defect.PackFrom(defect);
    any_quality.PackFrom(quality);

    ImageFrameIngress frame;
    frame.set_request_id("gw-test-002");
    frame.set_node_id("camera-02");
    frame.set_capture_time("2026-05-09T11:00:00Z");
    frame.set_image_format("png");
    frame.set_image_data("png_bytes");
    *frame.add_scenario_extensions() = any_defect;
    *frame.add_scenario_extensions() = any_quality;

    std::string serialized = frame.SerializeAsString();
    ImageFrameIngress parsed;
    ASSERT_TRUE(parsed.ParseFromString(serialized));

    ASSERT_EQ(parsed.scenario_extensions_size(), 2);

    // 按顺序验证
    EXPECT_TRUE(parsed.scenario_extensions(0).Is<scenario::DefectDetection>());
    scenario::DefectDetection d;
    ASSERT_TRUE(parsed.scenario_extensions(0).UnpackTo(&d));
    EXPECT_EQ(d.product_type(), "type-B");

    EXPECT_TRUE(parsed.scenario_extensions(1).Is<scenario::QualityInspection>());
    scenario::QualityInspection q;
    ASSERT_TRUE(parsed.scenario_extensions(1).UnpackTo(&q));
    EXPECT_EQ(q.batch_id(), 20260509);
    EXPECT_EQ(q.inspection_standard(), "v2.1");
}

TEST(ImageMetaExtensionsTest, SurfaceMeteringRoundTrip) {
    scenario::SurfaceMetering surface;
    surface.set_length_mm(250.5);
    surface.set_width_mm(180.2);
    surface.set_area_mm2(250.5 * 180.2);

    google::protobuf::Any any;
    any.PackFrom(surface);

    ImageFrameIngress frame;
    frame.set_request_id("gw-test-003");
    frame.set_node_id("camera-03");
    frame.set_capture_time("2026-05-09T12:00:00Z");
    frame.set_image_format("bmp");
    frame.set_image_data("bmp_bytes");
    *frame.add_scenario_extensions() = any;

    std::string serialized = frame.SerializeAsString();
    ImageFrameIngress parsed;
    ASSERT_TRUE(parsed.ParseFromString(serialized));

    ASSERT_EQ(parsed.scenario_extensions_size(), 1);
    EXPECT_TRUE(parsed.scenario_extensions(0).Is<scenario::SurfaceMetering>());

    scenario::SurfaceMetering unpacked;
    ASSERT_TRUE(parsed.scenario_extensions(0).UnpackTo(&unpacked));
    EXPECT_DOUBLE_EQ(unpacked.length_mm(), 250.5);
    EXPECT_DOUBLE_EQ(unpacked.width_mm(), 180.2);
    EXPECT_DOUBLE_EQ(unpacked.area_mm2(), 250.5 * 180.2);
}

TEST(ImageMetaExtensionsTest, NoExtensions) {
    // 验证空的 scenario_extensions 可正常序列化
    ImageFrameIngress frame;
    frame.set_request_id("gw-test-004");
    frame.set_node_id("camera-01");
    frame.set_capture_time("2026-05-09T00:00:00Z");
    frame.set_image_format("jpeg");
    frame.set_image_data("data");

    std::string serialized = frame.SerializeAsString();
    ImageFrameIngress parsed;
    ASSERT_TRUE(parsed.ParseFromString(serialized));

    EXPECT_EQ(parsed.scenario_extensions_size(), 0);
    EXPECT_EQ(parsed.request_id(), "gw-test-004");
}

TEST(ImageMetaExtensionsTest, WrongTypeUnpackReturnsFalse) {
    // 用错误类型解包应返回 false
    scenario::DefectDetection defect;
    defect.set_product_type("type-A");

    google::protobuf::Any any;
    any.PackFrom(defect);

    ImageFrameIngress frame;
    frame.set_request_id("gw-test-005");
    frame.set_node_id("cam-01");
    frame.set_capture_time("2026-05-09T00:00:00Z");
    frame.set_image_format("jpeg");
    frame.set_image_data("data");
    *frame.add_scenario_extensions() = any;

    std::string serialized = frame.SerializeAsString();
    ImageFrameIngress parsed;
    ASSERT_TRUE(parsed.ParseFromString(serialized));

    ASSERT_EQ(parsed.scenario_extensions_size(), 1);
    // 用 SurfaceMetering 解包 DefectDetection 应失败
    scenario::SurfaceMetering wrong;
    EXPECT_FALSE(parsed.scenario_extensions(0).UnpackTo(&wrong));
}

TEST(ImageMetaExtensionsTest, ExtraMetadataAlongsideExtensions) {
    scenario::DefectDetection defect;
    defect.set_product_type("type-C");

    google::protobuf::Any any;
    any.PackFrom(defect);

    ImageFrameIngress frame;
    frame.set_request_id("gw-test-006");
    frame.set_node_id("cam-01");
    frame.set_capture_time("2026-05-09T00:00:00Z");
    frame.set_image_format("jpeg");
    frame.set_image_data("data");
    *frame.add_scenario_extensions() = any;
    (*frame.mutable_extra_metadata())["custom_key"] = "custom_value";
    (*frame.mutable_extra_metadata())["another_key"] = "123";

    std::string serialized = frame.SerializeAsString();
    ImageFrameIngress parsed;
    ASSERT_TRUE(parsed.ParseFromString(serialized));

    // 扩展层正常
    ASSERT_EQ(parsed.scenario_extensions_size(), 1);
    EXPECT_TRUE(parsed.scenario_extensions(0).Is<scenario::DefectDetection>());

    // 透传层正常
    EXPECT_EQ(parsed.extra_metadata().at("custom_key"), "custom_value");
    EXPECT_EQ(parsed.extra_metadata().at("another_key"), "123");
    EXPECT_EQ(parsed.extra_metadata_size(), 2);
}

}  // namespace
}  // namespace gateway
}  // namespace mould

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
