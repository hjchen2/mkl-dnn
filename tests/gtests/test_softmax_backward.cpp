/*******************************************************************************
* Copyright 2016-2018 Intel Corporation
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/

#include "gtest/gtest.h"
#include "mkldnn_test_common.hpp"

#include "mkldnn.hpp"
#include <memory>

namespace mkldnn {

template <typename data_t>
void check_softmax_bwd(memory& dst, memory& diff_dst, memory &diff_src, int axis)
{
    data_t *dst_ptr = (data_t *)dst.get_data_handle();
    data_t *diff_dst_ptr = (data_t *)diff_dst.get_data_handle();
    data_t *diff_src_ptr = (data_t *)diff_src.get_data_handle();

    const memory::desc dst_pd = dst.get_primitive_desc().desc();
    const memory::desc diff_dst_pd = diff_dst.get_primitive_desc().desc();

    ASSERT_EQ(diff_dst_pd.data.data_type,
            memory::data_type::f32); // TODO: type assert

    // Allocate buffer for reference BW result
    auto ndims = diff_dst_pd.data.ndims;
    int total_dim_size = 1;
    for (int i=0; i<ndims; ++i) {
      total_dim_size *= diff_dst_pd.data.dims[i];
    }
    std::unique_ptr<data_t[]> diff_src_ref_ptr(new float[total_dim_size]);

    const float eps = 1e-7; //TODO: What should be the threshold?

    int OU = 1;
    for (int d = 0; d < axis; ++d) OU *= diff_dst_pd.data.dims[d];
    const int C = diff_dst_pd.data.dims[axis];
    int IN = 1;
    for (int d = axis + 1; d < ndims; ++d) IN *= diff_dst_pd.data.dims[d];

    mkldnn::impl::parallel_nd(OU, IN, [&](int ou, int in) {
        const int idx_start = ou * C * IN + in;

        float sbr = 0.0;
        for (int c=0; c < C ; ++c) {
            auto off_d = map_index(dst_pd, idx_start + c * IN);
            auto off_dd = map_index(diff_dst_pd, idx_start + c * IN);
            sbr += dst_ptr[off_d] * diff_dst_ptr[off_dd];
        }

        for (int c=0; c < C ; ++c) {
            auto off_d = map_index(dst_pd, idx_start + c * IN);
            auto off_dd = map_index(diff_dst_pd, idx_start + c * IN);
            diff_src_ref_ptr[off_dd] =
                dst_ptr[off_d] * (diff_dst_ptr[off_dd] - sbr);
        }
    });

    // Actual check
    for (int i=0; i < OU*C*IN; ++i)
        EXPECT_NEAR(diff_src_ptr[i], diff_src_ref_ptr[i], eps);
}

template <typename data_t>
struct softmax_test_params {
    engine::kind engine_kind;
    memory::format data_memory_format;
    memory::format diff_memory_format;
    memory::dims dims;
    int axis;
    bool expect_to_fail;
    mkldnn_status_t expected_status;
};

template <typename data_t>
class softmax_test : public ::testing::TestWithParam<softmax_test_params<data_t>> {
    softmax_test_params<data_t> p;
protected:
    virtual void SetUp() {
        p = ::testing::TestWithParam<softmax_test_params<data_t>>::GetParam();
        catch_expected_failures([=](){Test();}, p.expect_to_fail,
                    p.expected_status);
    }

    void Test() {
        ASSERT_TRUE(p.engine_kind == engine::kind::cpu);
        auto eng = engine(p.engine_kind, 0);

        memory::data_type prec = data_traits<data_t>::data_type;

        auto data_mem_desc = memory::desc(p.dims, prec, p.data_memory_format);
        auto data_mem_prim_desc = memory::primitive_desc(data_mem_desc, eng);
        auto diff_mem_desc = memory::desc(p.dims, prec, p.data_memory_format);
        auto diff_mem_prim_desc = memory::primitive_desc(diff_mem_desc, eng);

        std::unique_ptr<data_t[]> src_data(new data_t[data_mem_prim_desc.get_size()]);
        std::unique_ptr<data_t[]> dst_data(new data_t[data_mem_prim_desc.get_size()]);

        std::unique_ptr<data_t[]> src_diff(new data_t[diff_mem_prim_desc.get_size()]);
        std::unique_ptr<data_t[]> dst_diff(new data_t[diff_mem_prim_desc.get_size()]);

        auto src = memory(data_mem_prim_desc, src_data.get());
        auto dst = memory(data_mem_prim_desc, dst_data.get());

        auto diff_src = memory(diff_mem_prim_desc, src_diff.get());
        auto diff_dst = memory(diff_mem_prim_desc, dst_diff.get());

        // Create softmax backward descriptor
        // before forward so its exceptions can be tested
        auto softmax_desc
            = softmax_backward::desc(diff_mem_desc, data_mem_desc, p.axis);

        // Create softmax forward (hint for backward)
        auto softmax_fwd_desc = softmax_forward::desc(prop_kind::forward_scoring,
                data_mem_desc, p.axis);
        auto softmax_fwd_pdesc = softmax_forward::primitive_desc(softmax_fwd_desc,
                eng);

        auto softmax = softmax_forward(softmax_fwd_pdesc, src, dst);

        auto softmax_prim_desc
            = softmax_backward::primitive_desc(softmax_desc, eng, softmax_fwd_pdesc);
        auto softmax_bwd = softmax_backward(softmax_prim_desc, dst, diff_dst, diff_src);

        auto test_with_given_fill = [&](data_t mean, data_t var) {
            // Fill the softmax forward input
            fill_data<data_t>(data_mem_prim_desc.get_size(),
                    (data_t *)src.get_data_handle(), mean, var);

            // Fill the softmax backward diffs
            // eg. data diff that comes from upper primitive/layer
            fill_data<data_t>(diff_mem_prim_desc.get_size(),
                    (data_t *)diff_dst.get_data_handle(), data_t(0), data_t(1));

            stream(stream::kind::lazy).submit({softmax, softmax_bwd}).wait();
            check_softmax_bwd<data_t>(dst, diff_dst, diff_src, p.axis);
        };

        test_with_given_fill(-200, 1);
        test_with_given_fill(   0, 1);
        test_with_given_fill( 200, 1);
    }
};

using softmax_backward_test_float = softmax_test<float>;
using softmax_bwd_test_params_float = softmax_test_params<float>;

TEST_P(softmax_backward_test_float, TestsSoftmax) { }
INSTANTIATE_TEST_CASE_P(TestSoftmaxBackward, softmax_backward_test_float,
        ::testing::Values(
            softmax_bwd_test_params_float{ engine::kind::cpu, memory::format::nchw, memory::format::nchw, {2, -2, 128, 256}, 0, true, mkldnn_invalid_arguments},
            softmax_bwd_test_params_float{ engine::kind::cpu, memory::format::nchw, memory::format::nchw, {2, 19, 128, 256}, 5, true, mkldnn_invalid_arguments},
            softmax_bwd_test_params_float{ engine::kind::cpu, memory::format::nchw, memory::format::nchw, {2, 0, 5, 5}, 0},
            softmax_bwd_test_params_float{ engine::kind::cpu, memory::format::nchw, memory::format::nchw, {2, 0, 5, 5}, 1},
            softmax_bwd_test_params_float{ engine::kind::cpu, memory::format::nchw, memory::format::nchw, {2, 19, 128, 256}, 0},
            softmax_bwd_test_params_float{ engine::kind::cpu, memory::format::nchw, memory::format::nchw, {2, 19, 128, 256}, 2},
            softmax_bwd_test_params_float{ engine::kind::cpu, memory::format::nchw, memory::format::nchw, {2, 19, 128, 256}, 3},
            softmax_bwd_test_params_float{ engine::kind::cpu, memory::format::nc, memory::format::nc, {16, 300}, 0},
            softmax_bwd_test_params_float{ engine::kind::cpu, memory::format::nc, memory::format::nc, {16, 30000}, 1},
            softmax_bwd_test_params_float{ engine::kind::cpu, memory::format::nc, memory::format::nc, {2, 1000}, 1}
));
}
