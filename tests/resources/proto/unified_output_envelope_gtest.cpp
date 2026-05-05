#include <gtest/gtest.h>

#include "unified_output_envelope.pb.h"

namespace {

TEST(UnifiedOutputEnvelopeGtest, OneofKeepsOnlyLastPayloadType) {
  mould::test::unified::UnifiedOutputEnvelope envelope;
  envelope.set_envelope_seq(1001);
  envelope.set_emit_ts_us(1714500000123456ULL);
  envelope.set_source_module("RiskEvalModule");
  envelope.set_source_channel("RiskScore");

  auto* telemetry = envelope.mutable_telemetry_ingress();
  telemetry->set_seq(88);
  telemetry->set_quality(92.5F);
  ASSERT_EQ(
      envelope.payload_case(),
      mould::test::unified::UnifiedOutputEnvelope::kTelemetryIngress);
  ASSERT_TRUE(envelope.has_telemetry_ingress());

  auto* risk = envelope.mutable_risk_score();
  risk->set_seq(89);
  risk->set_score(77);
  risk->set_trigger("telemetry");

  // oneof 语义：后设置的字段会覆盖之前的字段。
  EXPECT_EQ(
      envelope.payload_case(),
      mould::test::unified::UnifiedOutputEnvelope::kRiskScore);
  EXPECT_TRUE(envelope.has_risk_score());
  EXPECT_FALSE(envelope.has_telemetry_ingress());
  EXPECT_EQ(envelope.risk_score().score(), 77U);

  // 非 oneof 的元信息字段不会受 payload 切换影响。
  EXPECT_EQ(envelope.envelope_seq(), 1001U);
  EXPECT_EQ(envelope.source_module(), "RiskEvalModule");
}

TEST(UnifiedOutputEnvelopeGtest, SerializationRoundTripPreservesPayloadCase) {
  mould::test::unified::UnifiedOutputEnvelope src;
  src.set_envelope_seq(2026);
  src.set_source_module("ActuatorCoordModule");
  src.set_source_channel("ActuatorCommand");
  auto* cmd = src.mutable_actuator_command();
  cmd->set_seq(9);
  cmd->set_throttle(14);
  cmd->set_brake(2);
  cmd->set_steer(7);
  cmd->set_trigger("risk");
  cmd->set_fault_level(1);

  std::string bytes;
  ASSERT_TRUE(src.SerializeToString(&bytes));

  mould::test::unified::UnifiedOutputEnvelope dst;
  ASSERT_TRUE(dst.ParseFromString(bytes));
  ASSERT_EQ(
      dst.payload_case(),
      mould::test::unified::UnifiedOutputEnvelope::kActuatorCommand);
  EXPECT_EQ(dst.actuator_command().throttle(), 14U);
  EXPECT_EQ(dst.actuator_command().trigger(), "risk");
}

}  // namespace


#include <iostream>

// 1. 订单上下文数据：用来传递给策略的计算参数
struct OrderContext {
    double amount;  // 订单金额
    std::string region; // 订单地区（用来演示工厂如何选择策略）
};

// 2. 抽象策略类（TaxStrategy）：定义税率计算的统一接口
class TaxStrategy {
public:
    // 纯虚函数：所有具体策略都必须实现这个方法
    virtual double Calculate(const OrderContext& context) = 0;
    // 虚析构函数：保证子类对象能被正确释放
    virtual ~TaxStrategy() = default;
};

// 3. 具体策略1：中国税率计算（示例：13%增值税）
class ChinaTaxStrategy : public TaxStrategy {
public:
    double Calculate(const OrderContext& context) override {
        // 实际场景中可以加更多业务逻辑，比如免税额度、阶梯税率等
        return context.amount * 0.13; 
    }
};

// 3. 具体策略2：美国税率计算（示例：8%销售税）
class USTaxStrategy : public TaxStrategy {
public:
    double Calculate(const OrderContext& context) override {
        return context.amount * 0.08;
    }
};

// 4. 策略工厂类：负责创建具体的策略对象
class StrategyFactory {
public:
    // 根据地区参数，创建对应的策略对象
    TaxStrategy* NewStrategy(const std::string& region) {
        if (region == "China") {
            return new ChinaTaxStrategy();
        } else if (region == "US") {
            return new USTaxStrategy();
        } else {
            // 默认返回中国策略，或者抛异常
            std::cout << "未知地区，使用默认中国税率策略" << std::endl;
            return new ChinaTaxStrategy();
        }
    }
};

// 5. 上下文类（SalesOrder）：持有策略对象，对外提供统一接口
class SalesOrder {
private:
    TaxStrategy* strategy; // 持有抽象策略指针（和你截图里的代码完全对应）
    OrderContext context;  // 订单上下文数据
public:
    // 构造函数：通过工厂创建策略对象（和你截图里的代码完全对应）
    SalesOrder(StrategyFactory* factory, const OrderContext& ctx) : context(ctx) {
        this->strategy = factory->NewStrategy(ctx.region);
    }

    // 析构函数：释放策略对象，避免内存泄漏（和你截图里的代码完全对应）
    ~SalesOrder() {
        delete this->strategy;
    }

    // 对外提供的计算接口：委托给策略对象执行（和你截图里的代码完全对应）
    double CalculateTax() {
        // 多态调用：实际执行的是具体策略的Calculate方法
        double tax = strategy->Calculate(context); 
        return tax;
    }
};

// 主函数：测试不同策略的运行效果
int main() {
    // 创建策略工厂
    StrategyFactory factory;

    // 测试1：中国订单
    OrderContext chinaOrder{1000, "China"};
    SalesOrder order1(&factory, chinaOrder);
    std::cout << "中国订单金额：" << chinaOrder.amount 
              << " 元，应缴税款：" << order1.CalculateTax() << " 元" << std::endl;

    // 测试2：美国订单
    OrderContext usOrder{1000, "US"};
    SalesOrder order2(&factory, usOrder);
    std::cout << "美国订单金额：" << usOrder.amount 
              << " 美元，应缴税款：" << order2.CalculateTax() << " 美元" << std::endl;

    // 测试3：未知地区订单（使用默认策略）
    OrderContext otherOrder{1000, "UK"};
    SalesOrder order3(&factory, otherOrder);
    std::cout << "英国订单金额：" << otherOrder.amount 
              << " 英镑，应缴税款：" << order3.CalculateTax() << " 英镑" << std::endl;

    return 0;
}