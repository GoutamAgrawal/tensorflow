/* Copyright 2023 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "xla/service/gpu/model/gpu_cost_model_stats_collection.h"

#include <stdint.h>

#include <memory>

#include "absl/status/statusor.h"
#include "xla/service/gpu/backend_configs.pb.h"
#include "xla/service/gpu/gpu_device_info_for_tests.h"
#include "xla/service/gpu/model/gpu_hlo_cost_analysis.h"
#include "xla/service/hlo_cost_analysis.h"
#include "xla/shape.h"
#include "xla/shape_util.h"
#include "xla/tests/hlo_test_base.h"
#include "xla/tests/verified_hlo_module.h"
#include "tsl/lib/core/status_test_util.h"

namespace xla {
namespace gpu {

class GpuCostModelStatsCollectionTest : public HloTestBase {
  HloCostAnalysis::ShapeSizeFunction ShapeSizeBytesFunction() const {
    return [&](const Shape& shape) {
      constexpr int64_t kPointerSize = 8;
      return ShapeUtil::ByteSizeOf(shape, kPointerSize);
    };
  }

 public:
  GpuCostModelStatsCollection cost_model_stats_{
      TestGpuDeviceInfo::RTXA6000DeviceInfo(),
      GpuHloCostAnalysis::Options{ShapeSizeBytesFunction(),
                                  /*per_second_rates=*/{},
                                  /*count_multiple_input_accesses=*/true}};
};

TEST_F(GpuCostModelStatsCollectionTest, FusinInEntryComputation) {
  TF_ASSERT_OK_AND_ASSIGN(auto module, ParseAndReturnVerifiedModule(R"(
    HloModule test_module

    log {
      p = f32[16384]{0} parameter(0)
      ROOT l = f32[16384]{0} log(p)
    }

    ENTRY main {
      %p0 = f32[16384] parameter(0)
      ROOT %res = f32[16384]{0} fusion(p0), kind=kInput, calls=log
    }
    )"));

  EXPECT_FALSE(cost_model_stats_.Run(module.get()).value());

  HloInstruction* root = module->entry_computation()->root_instruction();
  TF_ASSERT_OK_AND_ASSIGN(auto backend_config,
                          root->backend_config<FusionBackendConfig>());
  EXPECT_TRUE(backend_config.has_reification_cost());
  EXPECT_GT(backend_config.reification_cost().end_to_end_cycles(), 0);
}

TEST_F(GpuCostModelStatsCollectionTest, FusinInWhileComputation) {
  TF_ASSERT_OK_AND_ASSIGN(auto module, ParseAndReturnVerifiedModule(R"(
    HloModule test_module

    cond {
      p = f32[16384]{0} parameter(0)
      ROOT %constant.2 = pred[] constant(true)
    }

    log {
      p = f32[16384]{0} parameter(0)
      ROOT l = f32[16384]{0} log(p)
    }

    loop {
      %p0 = f32[16384] parameter(0)
      ROOT %res = f32[16384]{0} fusion(p0), kind=kInput, calls=log
    }

    ENTRY main {
      %p0 = f32[16384] parameter(0)
      ROOT %while = f32[16384] while(%p0), body=%loop, condition=%cond
    })"));

  EXPECT_FALSE(cost_model_stats_.Run(module.get()).value());

  HloInstruction* root = module->entry_computation()
                             ->root_instruction()
                             ->while_body()
                             ->root_instruction();
  TF_ASSERT_OK_AND_ASSIGN(auto backend_config,
                          root->backend_config<FusionBackendConfig>());
  EXPECT_TRUE(backend_config.has_reification_cost());
  EXPECT_GT(backend_config.reification_cost().end_to_end_cycles(), 0);
}

}  // namespace gpu
}  // namespace xla
