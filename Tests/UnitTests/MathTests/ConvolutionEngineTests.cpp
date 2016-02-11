//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.md file in the project root for full license information.
//
#include "stdafx.h"
#include <algorithm>
#include <array>
#include <random>
#include "../../../Source/Math/Matrix.h"
#include "../../../Source/Math/CPUMatrix.h"
#include "../../../Source/Math/GPUMatrix.h"
#include "../../../Source/Math/ConvolutionEngine.h"
#include "../../../Source/Math/CuDnnConvolutionEngine.h"

namespace Microsoft { namespace MSR { namespace CNTK { namespace Test {

using ConvFact = ConvolutionEngineFactory<float>;
using ConvFactPtr = std::unique_ptr<ConvolutionEngineFactory<float>>;
using ConvFactSPtr = std::shared_ptr<ConvolutionEngineFactory<float>>;
using vec = std::vector<float>;
using Tensor4DPtr = ConvFact::Tensor4DPtr;

static int GetNumOut(int i, int k, int s, bool pad)
{
    return (i - (pad ? 1 : k)) / s + 1;
}

static bool IsCuDnnSupported()
{
    fprintf(stderr, "ConvolutionEngineTests.cpp %d\n", __LINE__);
    try
    {
        // TODO: Will this ever return nullptr?
        return ConvFact::Create(0, ConvFact::EngineType::CuDnn, ImageLayoutKind::CHW) != nullptr;
    }
    catch (std::runtime_error)
    {
        fprintf(stderr, "ConvolutionEngineTests.cpp %d\n", __LINE__);
        return false;
    }
    fprintf(stderr, "ConvolutionEngineTests.cpp %d\n", __LINE__);
}

BOOST_AUTO_TEST_SUITE(ConvolutionSuite)

BOOST_AUTO_TEST_CASE(ConvolutionForward)
{
    if (!IsCuDnnSupported())
        return;

    int n = 2;
    int cmapIn = 3;
    int inW = 5;
    int inH = 5;
    int kW = 3;
    int kH = 3;
    int sW = 2;
    int sH = 2;
    int cmapOut = 2;
    int outW = GetNumOut(inW, kW, sW, false);
    int outH = GetNumOut(inH, kH, sH, false);

    for (int deviceId : {0})
    {
        // BUGBUG: These will fail depending on whether we built with cuDNN or not. Without cuDNN we should use HWC
        auto fact = ConvFact::Create(deviceId, ConvFact::EngineType::Auto, ImageLayoutKind::CHW);
        auto tt = typeid(fact).name();
        UNUSED(tt);
        auto eng = fact->CreateConvEngine(deviceId, 0);
        auto inT = fact->CreateTensor(inW, inH, cmapIn, n);
        auto filtT = fact->CreateFilter(kW, kH, cmapIn, cmapOut);
        auto outT = fact->CreateTensor(outW, outH, cmapOut, n);
        auto convT = fact->CreateConvDescriptor(*inT, *filtT, sW, sH, false);
        auto biasT = fact->CreateTensor(1, 1, cmapOut, 1);

        vec buf(inW * inH * cmapIn * n);
        int seed = 0;
        // Create input, cmapIn feature maps, inW x inH each (NCHW format).
        std::generate(buf.begin(), buf.end(), [=, &seed]
                      {
                          return seed++ % (inW * inH * cmapIn);
                      });
        SingleMatrix in(inW * inH * cmapIn, n, buf.data(), deviceId, matrixFlagNormal);

        seed = 0;
        buf.resize(kW * kH * cmapIn * cmapOut);
        // Create cmapOut filters, each kW x kH x cmapIn (NCHW format).
        std::generate(buf.begin(), buf.end(), [=, &seed]
                      {
                          return seed++ % (kW * kH * cmapIn);
                      });
        SingleMatrix filt(cmapOut, kW * kH * cmapIn, buf.data(), deviceId, matrixFlagNormal);

        SingleMatrix out(outW * outH * cmapOut, n, deviceId);
        SingleMatrix temp(deviceId);

        eng->Forward(*inT, in, *filtT, filt, *convT, *outT, out, temp);

        // Output is in NCHW format.
        std::array<float, 4 * 2 * 2> expBuf = {
            15219.0f, 15921.0f, 18729.0f, 19431.0f,
            15219.0f, 15921.0f, 18729.0f, 19431.0f,
            15219.0f, 15921.0f, 18729.0f, 19431.0f,
            15219.0f, 15921.0f, 18729.0f, 19431.0f};
        SingleMatrix exp(outW * outH * cmapOut, n, expBuf.data(), deviceId, matrixFlagNormal);
        BOOST_CHECK_MESSAGE(out.IsEqualTo(exp), "Unexpected convolution output.");

        float b[] = {1.0f, 2.0f};
        SingleMatrix bias(cmapOut, 1, b, deviceId, matrixFlagNormal);

        SingleMatrix plusB(outW * outH * cmapOut, n, expBuf.data(), deviceId, matrixFlagNormal);
        eng->AddBias(*outT, out, *biasT, bias, plusB);

        // Bias is per-channel.
        seed = 0;
        std::transform(expBuf.begin(), expBuf.end(), expBuf.begin(),
                       [=, &seed, &b](const float& a)
                       {
                           return a + b[(seed++ % (outW * outH * cmapOut)) / (outW * outH)];
                       });
        SingleMatrix expPlusB(outW * outH * cmapOut, n, expBuf.data(), deviceId, matrixFlagNormal);
        BOOST_CHECK_MESSAGE(plusB.IsEqualTo(expPlusB), "Unexpected (convolution + bias) output.");
    }
}

// REVIEW alexeyk: this really should be rolled into ConvolutionForward, make it data-driven.
BOOST_AUTO_TEST_CASE(ConvolutionForwardPad)
{
    if (!IsCuDnnSupported())
        return;

    int n = 2;
    int cmapIn = 1;
    int inW = 4;
    int inH = 4;
    int kW = 3;
    int kH = 3;
    int sW = 2;
    int sH = 2;
    int cmapOut = 1;
    bool pad = true;
    int outW = GetNumOut(inW, kW, sW, pad);
    int outH = GetNumOut(inH, kH, sH, pad);

    for (int deviceId : {-1, 0})
    {
        fprintf(stderr, "ConvolutionEngineTests.cpp %d\n", __LINE__);
        auto fact = ConvFact::Create(deviceId, ConvFact::EngineType::Auto, deviceId >= 0 ? ImageLayoutKind::CHW : ImageLayoutKind::HWC);
        fprintf(stderr, "ConvolutionEngineTests.cpp %d\n", __LINE__);
        auto eng = fact->CreateConvEngine(deviceId, 0);
        fprintf(stderr, "ConvolutionEngineTests.cpp %d\n", __LINE__);
        auto inT = fact->CreateTensor(inW, inH, cmapIn, n);
        fprintf(stderr, "ConvolutionEngineTests.cpp %d\n", __LINE__);
        auto filtT = fact->CreateFilter(kW, kH, cmapIn, cmapOut);
        fprintf(stderr, "ConvolutionEngineTests.cpp %d\n", __LINE__);
        auto outT = fact->CreateTensor(outW, outH, cmapOut, n);
        fprintf(stderr, "ConvolutionEngineTests.cpp %d\n", __LINE__);
        auto convT = fact->CreateConvDescriptor(*inT, *filtT, sW, sH, pad);
        fprintf(stderr, "ConvolutionEngineTests.cpp %d\n", __LINE__);

        // Input in NCHW format.
        fprintf(stderr, "ConvolutionEngineTests.cpp %d\n", __LINE__);
        SingleMatrix in(inW * inH * cmapIn, n, vec(inW * inH * cmapIn * n, 1.0f).data(), deviceId, matrixFlagNormal);
        // Create cmapOut filters, each kW x kH x cmapIn (NCHW format).
        SingleMatrix filt(cmapOut, kW * kH * cmapIn, vec(kW * kH * cmapIn * cmapOut, 1.0f).data(), deviceId, matrixFlagNormal);

        SingleMatrix out(outW * outH * cmapOut, n, deviceId);
        SingleMatrix temp(deviceId);

        fprintf(stderr, "ConvolutionEngineTests.cpp %d\n", __LINE__);
        eng->Forward(*inT, in, *filtT, filt, *convT, *outT, out, temp);
        fprintf(stderr, "ConvolutionEngineTests.cpp %d\n", __LINE__);

        // Output is in NCHW format.
        float expBuf[] = {
            4.0f, 6.0f, 6.0f, 9.0f,
            4.0f, 6.0f, 6.0f, 9.0f,
        };
        SingleMatrix exp(outW * outH * cmapOut, n, expBuf, deviceId, matrixFlagNormal);
        BOOST_CHECK(out.IsEqualTo(exp));
    }
}

BOOST_AUTO_TEST_CASE(ConvolutionBackwardData)
{
    if (!IsCuDnnSupported())
        return;

    // REVIEW alexeyk: very simple test, improve.
    int n = 2;
    int cmapIn = 3;
    int inW = 3;
    int inH = 3;
    int kW = 3;
    int kH = 3;
    int sW = 1;
    int sH = 1;
    int cmapOut = 2;
    int outW = GetNumOut(inW, kW, sW, false);
    int outH = GetNumOut(inH, kH, sH, false);

    for (int deviceId : {0})
    {
        auto fact = ConvFact::Create(deviceId, ConvFact::EngineType::Auto, ImageLayoutKind::CHW);
        auto eng = fact->CreateConvEngine(deviceId, 0);
        auto srcGradT = fact->CreateTensor(outW, outH, cmapOut, n);
        auto filtT = fact->CreateFilter(kW, kH, cmapIn, cmapOut);
        auto gradT = fact->CreateTensor(inW, inH, cmapIn, n);
        auto convT = fact->CreateConvDescriptor(*gradT, *filtT, sW, sH, false);

        // Source grads is in NCHW format.
        float srcGradBuf[] = {
            1.0f, 1.0f,
            1.0f, 1.0f};
        SingleMatrix srcGrad(outW * outH * cmapOut, n, srcGradBuf, deviceId, matrixFlagNormal);

        vec filtB(kW * kH * cmapIn * cmapOut);
        // Create cmapOut filters, each kW x kH x cmapIn (NCHW format).
        int seed = 0;
        std::generate(filtB.begin(), filtB.end(), [=, &seed]
                      {
                          return seed++ % (kW * kH * cmapIn);
                      });
        SingleMatrix filt(cmapOut, kW * kH * cmapIn, filtB.data(), deviceId, matrixFlagNormal);

        SingleMatrix grad(inW * inH * cmapIn, n, deviceId);
        grad.SetValue(1);
        SingleMatrix temp(deviceId);

        eng->BackwardData(*srcGradT, srcGrad, *filtT, filt, *convT, *gradT, grad, temp);

        // Target grads is in NCHW format.
        vec gradB(inW * inH * cmapIn * n);
        seed = 0;
        std::generate(gradB.begin(), gradB.end(), [=, &seed]
                      {
                          return 2 * (seed++ % (kW * kH * cmapIn)) + 1;
                      });

        SingleMatrix exp(inW * inH * cmapIn, n, gradB.data(), deviceId, matrixFlagNormal);
        BOOST_CHECK(grad.IsEqualTo(exp));
    }
}

BOOST_AUTO_TEST_CASE(ConvolutionBackwardFilter)
{
    if (!IsCuDnnSupported())
        return;

    // REVIEW alexeyk: very simple test, improve.
    int n = 2;
    int cmapIn = 3;
    int inW = 3;
    int inH = 3;
    int kW = 3;
    int kH = 3;
    int sW = 1;
    int sH = 1;
    int cmapOut = 2;
    int outW = GetNumOut(inW, kW, sW, false);
    int outH = GetNumOut(inH, kH, sH, false);

    for (int deviceId : {0})
    {
        auto fact = ConvFact::Create(deviceId, ConvFact::EngineType::Auto, ImageLayoutKind::CHW);
        auto eng = fact->CreateConvEngine(deviceId, 0);
        auto srcGradT = fact->CreateTensor(outW, outH, cmapOut, n);
        auto filtT = fact->CreateFilter(kW, kH, cmapIn, cmapOut);
        auto inT = fact->CreateTensor(inW, inH, cmapIn, n);
        auto convT = fact->CreateConvDescriptor(*inT, *filtT, sW, sH, false);
        auto biasT = fact->CreateTensor(1, 1, cmapOut, 1);

        // Source grads is in NCHW format.
        float srcGradBuf[] = {
            1.0f, 1.0f,
            1.0f, 1.0f,
        };
        SingleMatrix srcGrad(outW * outH * cmapOut, n, srcGradBuf, deviceId, matrixFlagNormal);

        vec buf(inW * inH * cmapIn * n);
        int seed = 0;
        // Create input, cmapIn feature maps, inW x inH each, NCHW format.
        std::generate(buf.begin(), buf.end(), [=, &seed]
                      {
                          return seed++ % (inW * inH * cmapIn);
                      });
        SingleMatrix in(inW * inH * cmapIn, n, buf.data(), deviceId, matrixFlagNormal);

        SingleMatrix filt(cmapOut, kW * kH * cmapIn, deviceId);
        filt.SetValue(1);
        SingleMatrix temp(deviceId);

        eng->BackwardFilter(*srcGradT, srcGrad, *inT, in, *convT, *filtT, filt, false, temp);

        // Expected filter values in NCHW format.
        vec expFiltB(cmapOut * kW * kH * cmapIn);
        seed = 0;
        std::generate(expFiltB.begin(), expFiltB.end(), [=, &seed]
                      {
                          return 2 * (seed++ % (kW * kH * cmapIn)) + 1;
                      });
        SingleMatrix exp(cmapOut, kW * kH * cmapIn, expFiltB.data(), deviceId, matrixFlagNormal);
        BOOST_CHECK_MESSAGE(filt.IsEqualTo(exp), "Unexpected convolution gradient.");

        // Verify bias backpropagation.
        float b[] = {1.0f, 2.0f};
        SingleMatrix biasGrad(cmapOut, 1, b, deviceId, matrixFlagNormal);

        eng->BackwardBias(*srcGradT, srcGrad, *biasT, biasGrad);

        // Bias is per-channel.
        float bg[] = {b[0] + srcGradBuf[0] + srcGradBuf[2], b[1] + srcGradBuf[1] + srcGradBuf[3]};
        SingleMatrix expBiasGrad(cmapOut, 1, bg, deviceId, matrixFlagNormal);
        BOOST_CHECK_MESSAGE(biasGrad.IsEqualTo(expBiasGrad), "Unexpected bias gradient.");
    }
}

BOOST_AUTO_TEST_CASE(MaxPoolForward)
{
    if (!IsCuDnnSupported())
        return;

    int n = 2;
    int cmap = 2;
    int inW = 4;
    int inH = 4;
    int kW = 2;
    int kH = 2;
    int sW = 2;
    int sH = 2;
    int outW = GetNumOut(inW, kW, sW, false);
    int outH = GetNumOut(inH, kH, sH, false);

    for (int deviceId : {0})
    {
        auto fact = ConvFact::Create(deviceId, ConvFact::EngineType::Auto, ImageLayoutKind::CHW);
        auto eng = fact->CreatePoolEngine(deviceId);
        auto inT = fact->CreateTensor(inW, inH, cmap, n);
        auto outT = fact->CreateTensor(outW, outH, cmap, n);
        auto poolT = fact->CreatePoolDescriptor(PoolingDescriptor::PoolKind::Max, kW, kH, sW, sH, 0, 0);

        vec buf(inW * inH * cmap * n);
        int seed = 0;
        // Create input, cmapIn feature maps, inW x inH each (NCHW format).
        std::generate(buf.begin(), buf.end(), [=, &seed]
                      {
                          return seed++ % (inW * inH * cmap);
                      });
        SingleMatrix in(inW * inH * cmap, n, buf.data(), deviceId, matrixFlagNormal);

        SingleMatrix out(outW * outH * cmap, n, deviceId);

        eng->Forward(*inT, in, *poolT, *outT, out);

        // Output is in NCHW format.
        float expBuf[] = {
            5.0f, 7.0f,
            13.0f, 15.0f,
            21.0f, 23.0f,
            29.0f, 31.0f,
            5.0f, 7.0f,
            13.0f, 15.0f,
            21.0f, 23.0f,
            29.0f, 31.0f,
        };
        SingleMatrix exp(outW * outH * cmap, n, expBuf, deviceId, matrixFlagNormal);
        BOOST_CHECK(out.IsEqualTo(exp));
    }
}

BOOST_AUTO_TEST_CASE(MaxPoolBackward)
{
    if (!IsCuDnnSupported())
        return;

    int n = 2;
    int cmap = 2;
    int inW = 4;
    int inH = 4;
    int kW = 2;
    int kH = 2;
    int sW = 2;
    int sH = 2;
    int outW = GetNumOut(inW, kW, sW, false);
    int outH = GetNumOut(inH, kH, sH, false);

    for (int deviceId : {0})
    {
        auto fact = ConvFact::Create(deviceId, ConvFact::EngineType::Auto, ImageLayoutKind::CHW);
        auto eng = fact->CreatePoolEngine(deviceId);
        auto inT = fact->CreateTensor(inW, inH, cmap, n);
        auto outT = fact->CreateTensor(outW, outH, cmap, n);
        auto poolT = fact->CreatePoolDescriptor(PoolingDescriptor::PoolKind::Max, kW, kH, sW, sH, 0, 0);

        vec buf(inW * inH * cmap * n);
        int seed = 0;
        // Create input, cmapIn feature maps, inW x inH each (NCHW format).
        std::generate(buf.begin(), buf.end(), [=, &seed]
                      {
                          return seed++ % (inW * inH * cmap);
                      });
        SingleMatrix in(inW * inH * cmap, n, buf.data(), deviceId, matrixFlagNormal);
        SingleMatrix out(outW * outH * cmap, n, deviceId);
        // Do forward pass first.
        eng->Forward(*inT, in, *poolT, *outT, out);

        // For gradients, use the same values as outputs.
        SingleMatrix srcGrad(out);
        SingleMatrix grad(inW * inH * cmap, n, deviceId);
        grad.SetValue(1);

        eng->Backward(*outT, out, srcGrad, *poolT, *inT, in, grad);

        // Output is in NCHW format.
        std::fill(buf.begin(), buf.end(), 1.0f);
        vec expMap = {
            5.0f, 7.0f,
            13.0f, 15.0f,
            21.0f, 23.0f,
            29.0f, 31.0f,
            5.0f, 7.0f,
            13.0f, 15.0f,
            21.0f, 23.0f,
            29.0f, 31.0f,
        };
        for (size_t i = 0; i < expMap.size(); i++)
            buf[(int) expMap[i] + inW * inH * cmap * (i / (expMap.size() / n))] += expMap[i];
        SingleMatrix exp(inW * inH * cmap, n, buf.data(), deviceId, matrixFlagNormal);

        BOOST_CHECK(grad.IsEqualTo(exp));
    }
}

BOOST_AUTO_TEST_CASE(AvgPoolForward)
{
    if (!IsCuDnnSupported())
        return;

    int n = 2;
    int cmap = 2;
    int inW = 4;
    int inH = 4;
    int kW = 2;
    int kH = 2;
    int sW = 2;
    int sH = 2;
    int outW = GetNumOut(inW, kW, sW, false);
    int outH = GetNumOut(inH, kH, sH, false);

    for (int deviceId : {0})
    {
        auto fact = ConvFact::Create(deviceId, ConvFact::EngineType::Auto, ImageLayoutKind::CHW);
        auto eng = fact->CreatePoolEngine(deviceId);
        auto inT = fact->CreateTensor(inW, inH, cmap, n);
        auto outT = fact->CreateTensor(outW, outH, cmap, n);
        auto poolT = fact->CreatePoolDescriptor(PoolingDescriptor::PoolKind::Average, kW, kH, sW, sH, 0, 0);

        vec buf(inW * inH * cmap * n);
        int seed = 0;
        // Create input, cmapIn feature maps, inW x inH each (NCHW format).
        std::generate(buf.begin(), buf.end(), [=, &seed]
                      {
                          return seed++ % (inW * inH * cmap);
                      });
        SingleMatrix in(inW * inH * cmap, n, buf.data(), deviceId, matrixFlagNormal);

        SingleMatrix out(outW * outH * cmap, n, deviceId);

        eng->Forward(*inT, in, *poolT, *outT, out);

        // Output is in NCHW format.
        float expBuf[] = {
            2.5f, 4.5f,
            10.5f, 12.5f,
            18.5f, 20.5f,
            26.5f, 28.5f,
            2.5f, 4.5f,
            10.5f, 12.5f,
            18.5f, 20.5f,
            26.5f, 28.5f,
        };
        SingleMatrix exp(outW * outH * cmap, n, expBuf, deviceId, matrixFlagNormal);
        BOOST_CHECK(out.IsEqualTo(exp));
    }
}

BOOST_AUTO_TEST_CASE(AvgPoolBackward)
{
    if (!IsCuDnnSupported())
        return;

    int n = 1;
    int cmap = 1;
    int inW = 4;
    int inH = 4;
    int kW = 2;
    int kH = 2;
    int sW = 2;
    int sH = 2;
    int outW = GetNumOut(inW, kW, sW, false);
    int outH = GetNumOut(inH, kH, sH, false);

    for (int deviceId : {0})
    {
        auto fact = ConvFact::Create(deviceId, ConvFact::EngineType::Auto, ImageLayoutKind::CHW);
        auto eng = fact->CreatePoolEngine(deviceId);
        auto inT = fact->CreateTensor(inW, inH, cmap, n);
        auto outT = fact->CreateTensor(outW, outH, cmap, n);
        auto poolT = fact->CreatePoolDescriptor(PoolingDescriptor::PoolKind::Average, kW, kH, sW, sH, 0, 0);

        vec buf(inW * inH * cmap * n);
        int seed = 0;
        // Create input, cmapIn feature maps, inW x inH each (NHWC format).
        std::generate(buf.begin(), buf.end(), [=, &seed]
                      {
                          return seed++ % (inW * inH * cmap);
                      });
        SingleMatrix in(inW * inH * cmap, n, buf.data(), deviceId, matrixFlagNormal);
        SingleMatrix out(outW * outH * cmap, n, deviceId);
        // Do forward pass first.
        eng->Forward(*inT, in, *poolT, *outT, out);

        // For gradients, use the same values as outputs.
        SingleMatrix srcGrad(out);
        SingleMatrix grad(inW * inH * cmap, n, deviceId);
        grad.SetValue(1);

        eng->Backward(*outT, out, srcGrad, *poolT, *inT, in, grad);

        // Output is in NHWC format.
        float expBuf[] = {
            1.625f, 1.625f, 2.125f, 2.125f,
            1.625f, 1.625f, 2.125f, 2.125f,
            3.625f, 3.625f, 4.125f, 4.125f,
            3.625f, 3.625f, 4.125f, 4.125f,
        };
        SingleMatrix exp(inW * inH * cmap, n, expBuf, deviceId, matrixFlagNormal);
        BOOST_CHECK(grad.IsEqualTo(exp));
    }
}

BOOST_AUTO_TEST_SUITE_END()

// Batch normalization unit tests.
// REVIEW alexeyk: is this a right place?

std::vector<std::tuple<Tensor4DPtr, bool>> GenerateBNTestConfigs(ConvFact& fact)
{
    std::vector<std::tuple<Tensor4DPtr, bool>> res;
    for (bool spatial : {false})
    {
        for (size_t n : { 6, 13, 62, 512})
        {
            for (size_t c : {1})
            {
                for (size_t h : {1})
                {
                    for (size_t w : { 6, 17, 126, 2048})
                    {
                        res.push_back(std::make_tuple(std::move(fact.CreateTensor(w, h, c, n)), spatial));
                    }
                }
            }
        }
    }
    return res;
}

BOOST_AUTO_TEST_SUITE(BatchNormalizationSuite)

BOOST_AUTO_TEST_CASE(BatchNormalizationForwardTrain)
{
    std::mt19937 rng(0);
    std::normal_distribution<float> nd;

    for (int deviceId : {0})
    {
        auto fact = ConvFact::Create(deviceId, ConvFact::EngineType::Auto, ImageLayoutKind::CHW);
        auto engCudnn = fact->CreateConvEngine(deviceId, 0, BatchNormImpl::CuDnn);
        auto engCntk = fact->CreateConvEngine(deviceId, 0, BatchNormImpl::Cntk);
        for (auto& cfg : GenerateBNTestConfigs(*fact))
        {
            auto& t = *std::move(std::get<0>(cfg));
            bool spatial = std::get<1>(cfg);
            double expAvg = 1;

            size_t crow = t.w() * t.h() * t.c();
            size_t ccol = t.n();

            vec buf(crow * t.n());
            std::generate(begin(buf), end(buf), [&] { return nd(rng); });
            SingleMatrix in(crow, ccol, buf.data(), deviceId, matrixFlagNormal);

            Tensor4DPtr scaleBiasT = spatial ? fact->CreateTensor(1, 1, t.c(), 1) : fact->CreateTensor(t.w(), t.h(), t.c(), 1);
            buf.resize(scaleBiasT->w() * scaleBiasT->h() * scaleBiasT->c());

            std::generate(begin(buf), end(buf), [&] { return nd(rng); });
            SingleMatrix scale(buf.size(), 1, buf.data(), deviceId, matrixFlagNormal);
            std::generate(begin(buf), end(buf), [&] { return nd(rng); });
            SingleMatrix bias(buf.size(), 1, buf.data(), deviceId, matrixFlagNormal);

            std::generate(begin(buf), end(buf), [&] { return nd(rng); });
            SingleMatrix runMean(buf.size(), 1, buf.data(), deviceId, matrixFlagNormal);
            SingleMatrix runMeanExp(buf.size(), 1, buf.data(), deviceId, matrixFlagNormal);
            std::generate(begin(buf), end(buf), [&] { return nd(rng); });
            SingleMatrix runInvStdDev(buf.size(), 1, buf.data(), deviceId, matrixFlagNormal);
            SingleMatrix runInvStdDevExp(buf.size(), 1, buf.data(), deviceId, matrixFlagNormal);

            std::generate(begin(buf), end(buf), [&] { return nd(rng); });
            SingleMatrix saveMean(buf.size(), 1, buf.data(), deviceId, matrixFlagNormal);
            SingleMatrix saveMeanExp(buf.size(), 1, buf.data(), deviceId, matrixFlagNormal);
            std::generate(begin(buf), end(buf), [&] { return nd(rng); });
            SingleMatrix saveInvStdDev(buf.size(), 1, buf.data(), deviceId, matrixFlagNormal);
            SingleMatrix saveInvStdDevExp(buf.size(), 1, buf.data(), deviceId, matrixFlagNormal);

            SingleMatrix outBuf(crow, 3 * ccol, deviceId);
            outBuf.SetValue(std::numeric_limits<float>::quiet_NaN());
            SingleMatrix out = outBuf.ColumnSlice(ccol, ccol);
            out.SetValue(0);
            SingleMatrix outExp(out);

            CudaTimer time1;
            time1.Start();
            engCntk->NormalizeBatch(t, in, *scaleBiasT, scale, bias, spatial, expAvg, runMean, runInvStdDev,
                                    out, saveMean, saveInvStdDev);
            time1.Stop();

            CudaTimer time2;
            time2.Start();
            engCudnn->NormalizeBatch(t, in, *scaleBiasT, scale, bias, spatial, expAvg, runMeanExp, runInvStdDevExp,
                                     outExp, saveMeanExp, saveInvStdDevExp);
            time2.Stop();

            std::stringstream tmsg;
            tmsg << "tensor: (w = " << t.w() << ", h = " << t.h() << ", c = " << t.c() << ", n = " << t.n() << ")";
            std::string msg = " are not equal " + tmsg.str();

            BOOST_REQUIRE_MESSAGE(out.IsEqualTo(outExp, 1e-5f), "out" << msg);
            //BOOST_REQUIRE_MESSAGE(runMean.IsEqualTo(runMeanExp), "runMean" << msg);
            //BOOST_REQUIRE_MESSAGE(runInvStdDev.IsEqualTo(runInvStdDevExp), "runInvStdDev" << msg);
            BOOST_REQUIRE_MESSAGE(saveMean.IsEqualTo(saveMeanExp, 1e-5f), "saveMean" << msg);
            BOOST_REQUIRE_MESSAGE(saveInvStdDev.IsEqualTo(saveInvStdDevExp, 1e-5f), "saveInvStdDev" << msg);
#ifndef _DEBUG
            float elapsedCntk = time1.Elapsed();
            float elapsedCudnn = time2.Elapsed();
            // Check performance. Current version of cuDNN (v4 RC) is significanlty slower than CNTK implementation.
            // For optimal cases (vectorSize % 4 == 0), CNTK implementation can be >5x faster than cuDNN.
            if (crow >= 128 && ccol >= 32)
            {
                // Use conservative estimates.
                BOOST_REQUIRE_MESSAGE(2 * elapsedCntk < elapsedCudnn,
                                      "CNTK implementation (" << elapsedCntk << "ms) must be faster than cuDNN (" << elapsedCudnn << "ms) by at least 2x, what's changed? " << tmsg.str());
            }
#endif
        }
    }
}

BOOST_AUTO_TEST_SUITE_END()

}
} } }
