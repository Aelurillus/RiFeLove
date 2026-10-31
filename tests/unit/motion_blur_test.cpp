#include "core/pipeline/motion_blur.hpp"

#include <gtest/gtest.h>

#include <array>
#include <algorithm>
#include <cstdint>
#include <vector>

// Compiled through the existing pipeline-enabled test target so production
// integration coverage does not require a second build configuration.
#include "../integration/motion_blur_pipeline_test.hpp"
#include "../integration/automatic_denoise_pipeline_test.hpp"
#include "../integration/restoration_diagnostics_pipeline_test.hpp"

using rifelove::Rational;
using rifelove::quality::PairAction;
using namespace rifelove::pipeline;

// Keep registrations in this source: gtest_add_tests scans it at configure
// time, while the substantive production-pipeline fixtures live together.
TEST_F(MotionBlurPipeline, DenseSourcesIntegrateAt60And5994ThroughRealPipeline) { verifyDenseSources(); }
TEST_F(MotionBlurPipeline, ShortClipDrainsBothRealSourceEdgeHoldsAtEof) { verifyBothSourceEdges(); }
TEST_F(MotionBlurPipeline, SceneCutsAreCountedAsFallbackAndNeverExposureMixed) { verifySceneCut(); }
TEST_F(MotionBlurPipeline, InsufficientFullShutterBudgetFailsWithoutPublishingOrLeakingPartialFiles) { verifyBudgetFailure(); }
TEST_F(MotionBlurPipeline, CancellationAfterProcessingRemovesUnpublishedOutput) { verifyCancellation(); }
TEST_F(AutomaticDenoisePipeline, ConditionsRealTemporalRequestsResetsAtCutsAndPreservesTimeline) { verifyConditioningAndTiming(); }
TEST_F(AutomaticDenoisePipeline, CompressionDeblockRunsBeforeFastDvdnetAndPreservesScenePts) { verifyCompressionStage(); }
TEST_F(AutomaticDenoisePipeline, AutomaticCompressionAnalyzesSourcePtsAndBypassesCleanScene) { verifyAutomaticCompression(); }
TEST_F(AutomaticDenoisePipeline, AutomaticCompressionReplaysControlsAndDecodedPixelsAcrossCheckpointInterruptions) { verifyAutomaticCompressionReplay(); }
TEST_F(AutomaticDenoisePipeline, CompressionDeblockDrainsOneFrameSceneAtClipEdges) { verifyCompressionShortClip(); }
TEST_F(AutomaticDenoisePipeline, RejectsUnboundCheckpointAndAbsentModelBeforePublication) { verifyConfigurationSafety(); }
TEST_F(AutomaticDenoisePipeline, CancelsWithoutPublishingPartialOutput) { verifyAutomaticCancellation(); }
TEST_F(AutomaticDenoisePipeline, ReplaysSourcePtsAndConditioningExactlyAcrossCheckpointInterruptions) { verifyCheckpointReplay(); }
TEST_F(RestorationDiagnosticsPipeline, RefreshesQualifiedCompressionDiagnosticsAfterEofDrainAndBeforePublication) { verifyCompressionDiagnostics(false); }
TEST_F(RestorationDiagnosticsPipeline, AutomaticZeroBypassKeepsCompressionProfileVerificationPending) { verifyCompressionDiagnostics(true); }

TEST(MotionBlurSampling, UsesExactSymmetricExposureOffsets) {
    const auto samples = motionBlurSampleTimes(Rational(1,60),Rational(60),180,4);
    ASSERT_EQ(samples.size(),4U);
    EXPECT_EQ(samples[0],Rational(13,960));
    EXPECT_EQ(samples[1],Rational(1,64));
    EXPECT_EQ(samples[2],Rational(17,960));
    EXPECT_EQ(samples[3],Rational(19,960));
}

TEST(MotionBlurSampling, ZeroAngleCollapsesEverySampleToTheCenter) {
    const auto samples = motionBlurSampleTimes(Rational(17,60),Rational(60000,1001),0,8);
    ASSERT_EQ(samples.size(),8U);
    for (const auto& sample : samples) EXPECT_EQ(sample,Rational(17,60));
}

TEST(MotionBlurSampling, BlendingUsesLinearLightTransferRoundTrip) {
    const auto linearHalf = (sdrToLinear(0.0F,"iec61966-2-1") + sdrToLinear(1.0F,"iec61966-2-1")) / 2.0F;
    const auto encodedHalf = linearToSdr(linearHalf,"iec61966-2-1");
    EXPECT_GT(encodedHalf,0.7F);
    EXPECT_NEAR(sdrToLinear(encodedHalf,"iec61966-2-1"),linearHalf,1e-5F);
    EXPECT_NEAR(linearToSdr(sdrToLinear(0.42F,"bt709"),"bt709"),0.42F,1e-5F);
}

TEST(MotionBlurSampling, ResolvesCoincidentSourceFrameWithoutInterpolation) {
    const std::array<Rational,3> timestamps{Rational(0),Rational(1,24),Rational(1,12)};
    const std::array<PairAction,2> actions{PairAction::interpolate,PairAction::interpolate};
    const auto location = resolveMotionBlurSample(timestamps,actions,Rational(1,24));
    EXPECT_TRUE(location.sourceFrame);
    EXPECT_FALSE(location.blocked);
    EXPECT_EQ(location.sourceIndex,1U);
}

TEST(MotionBlurSampling, ResolvesSamplesAcrossAdjacentIntervals) {
    const std::array<Rational,3> timestamps{Rational(0),Rational(1,24),Rational(1,8)};
    const std::array<PairAction,2> actions{PairAction::interpolate,PairAction::interpolate};
    const auto location = resolveMotionBlurSample(timestamps,actions,Rational(1,12));
    EXPECT_FALSE(location.sourceFrame);
    EXPECT_FALSE(location.blocked);
    EXPECT_EQ(location.leftIndex,1U);
    EXPECT_EQ(location.rightIndex,2U);
    EXPECT_EQ(location.timestep,Rational(1,2));
}

TEST(MotionBlurSampling, UsesEdgeFramesAtFirstAndFinalExposureBoundaries) {
    const std::array<Rational,3> timestamps{Rational(0),Rational(1,24),Rational(1,12)};
    const std::array<PairAction,2> actions{PairAction::interpolate,PairAction::interpolate};
    const auto first = resolveMotionBlurSample(timestamps,actions,Rational(-1,240));
    const auto last = resolveMotionBlurSample(timestamps,actions,Rational(1,8));
    EXPECT_TRUE(first.sourceFrame); EXPECT_EQ(first.sourceIndex,0U);
    EXPECT_TRUE(last.sourceFrame); EXPECT_EQ(last.sourceIndex,2U);
}

TEST(MotionBlurSampling, BlocksSamplesAndWindowsAtSceneCutBarriers) {
    const std::array<Rational,4> timestamps{Rational(0),Rational(1,24),Rational(1,12),Rational(1,8)};
    const std::array<PairAction,3> actions{PairAction::interpolate,PairAction::sceneCutHold,PairAction::interpolate};
    const auto inside = resolveMotionBlurSample(timestamps,actions,Rational(1,16));
    EXPECT_TRUE(inside.blocked);
    EXPECT_TRUE(motionBlurWindowCrossesBlockedPair(timestamps,actions,Rational(1,48),Rational(1,10)));
    EXPECT_FALSE(motionBlurWindowCrossesBlockedPair(timestamps,actions,Rational(1,12),Rational(1,10)));
}

namespace {
struct ExposureStreamEvidence {
    std::size_t integrated = 0;
    std::size_t peakWindow = 0;
    std::size_t edgeHeld = 0;
};

// Drive exactly the production planner/pruning contract one source PTS at a
// time. No frame-rate estimate is used to decide lookahead or evict history.
ExposureStreamEvidence verifyExposureStream(const std::vector<Rational>& source,
    Rational end, Rational rate, int angle, int sampleCount) {
    ExposureStreamEvidence evidence;
    std::vector<Rational> window;
    std::vector<PairAction> actions;
    bool sourceStart = true;
    const auto center = [&] { return source.front() + Rational(static_cast<std::int64_t>(evidence.integrated)) / rate; };
    const auto prune = [&] {
        const auto discard = motionBlurDiscardableFrames(window,motionBlurExposureBounds(center(),rate,angle).first);
        if (discard != 0) {
            window.erase(window.begin(),window.begin() + static_cast<std::ptrdiff_t>(discard));
            actions.erase(actions.begin(),actions.begin() + static_cast<std::ptrdiff_t>(discard));
            sourceStart = false;
        }
    };
    const auto drain = [&](bool eof) {
        while (center() < end) {
            const auto plan = planMotionBlurExposure(window,actions,center(),rate,angle,sampleCount,sourceStart,eof);
            if (!plan.ready) break;
            EXPECT_TRUE(plan.fallbackReason.empty());
            EXPECT_FALSE(plan.center.blocked);
            EXPECT_EQ(plan.samples.size(),static_cast<std::size_t>(sampleCount));
            const auto samples = motionBlurSampleTimes(center(),rate,angle,sampleCount);
            for (std::size_t index = 0; index < plan.samples.size(); ++index) {
                const auto& location = plan.samples[index];
                EXPECT_FALSE(location.blocked);
                if (location.sourceFrame) {
                    const auto expected = std::clamp(samples[index],source.front(),source.back());
                    EXPECT_EQ(window[location.sourceIndex],expected);
                } else {
                    EXPECT_GT(location.timestep,Rational(0)); EXPECT_LT(location.timestep,Rational(1));
                    EXPECT_EQ(window[location.leftIndex] +
                        (window[location.rightIndex] - window[location.leftIndex]) * location.timestep,samples[index]);
                }
            }
            if (plan.edgeHeld) ++evidence.edgeHeld;
            ++evidence.integrated;
            prune();
        }
        prune();
    };
    for (const auto timestamp : source) {
        if (!window.empty()) actions.push_back(PairAction::interpolate);
        window.push_back(timestamp);
        evidence.peakWindow = std::max(evidence.peakWindow,window.size());
        drain(false);
    }
    drain(true);
    const auto exactCount = (end - source.front()) * rate;
    const auto expectedCount = exactCount.numerator() / exactCount.denominator() +
        (exactCount.numerator() % exactCount.denominator() != 0 ? 1 : 0);
    EXPECT_EQ(evidence.integrated,static_cast<std::size_t>(expectedCount));
    EXPECT_GT(evidence.edgeHeld,0U);
    return evidence;
}
}

TEST(MotionBlurExposure, HighRateSourcesCoverFullShutterAt60AndFractionalTargetsWithBoundedHistory) {
    for (const int sourceRate : {240,300,360,480,600}) {
        std::vector<Rational> timestamps;
        const Rational origin(7,3);
        for (int index = 0; index < sourceRate * 2; ++index) timestamps.push_back(origin + Rational(index,sourceRate));
        for (const auto target : {Rational(60),Rational(60000,1001)}) {
            for (const int angle : {180,360}) for (const int samples : {4,16}) {
                SCOPED_TRACE("source=" + std::to_string(sourceRate) + " target=" + target.toString() +
                    " angle=" + std::to_string(angle) + " samples=" + std::to_string(samples));
                const auto evidence = verifyExposureStream(timestamps,origin + Rational(2),target,angle,samples);
                // At most one shutter plus its two source brackets and one
                // arriving timestamp; storage is independent of clip length.
                EXPECT_LE(evidence.peakWindow,static_cast<std::size_t>(sourceRate / 59 + 4));
                if (angle == 360) EXPECT_GT(evidence.peakWindow,4U);
            }
        }
    }
}

TEST(MotionBlurExposure, VfrTimestampsAndNonzeroOriginDetermineTheWindow) {
    const std::vector<Rational> timestamps{Rational(5),Rational(5001,1000),Rational(501,100),
        Rational(503,100),Rational(51,10),Rational(5101,1000),Rational(26,5)};
    const auto evidence = verifyExposureStream(timestamps,Rational(521,100),Rational(60000,1001),360,16);
    EXPECT_LE(evidence.peakWindow,timestamps.size());
}

TEST(MotionBlurExposure, WaitsForFullShutterEvenWhenAllMidpointSamplesAreAvailable) {
    const std::array<Rational,3> timestamps{Rational(0),Rational(1,60),Rational(19,960)};
    const std::array<PairAction,2> actions{PairAction::interpolate,PairAction::interpolate};
    const auto bounds = motionBlurExposureBounds(Rational(1,60),Rational(60),180);
    EXPECT_EQ(bounds.first,Rational(1,80)); EXPECT_EQ(bounds.second,Rational(1,48));
    EXPECT_FALSE(planMotionBlurExposure(timestamps,actions,Rational(1,60),Rational(60),180,4,true,false).ready);
    const auto eof = planMotionBlurExposure(timestamps,actions,Rational(1,60),Rational(60),180,4,true,true);
    EXPECT_TRUE(eof.ready); EXPECT_TRUE(eof.fallbackReason.empty());
}

TEST(MotionBlurExposure, WholeExposureCannotSkipAnUnsampledQualityBarrier) {
    const std::array<Rational,9> timestamps{Rational(0),Rational(1,2),Rational(51,100),Rational(52,100),
        Rational(3,4),Rational(1),Rational(5,4),Rational(3,2),Rational(2)};
    std::array<PairAction,8> actions;
    actions.fill(PairAction::interpolate);
    for (const auto action : {PairAction::sceneCutHold,PairAction::abnormalMotionHold,PairAction::duplicateHold}) {
        actions[2] = action;
        const auto plan = planMotionBlurExposure(timestamps,actions,Rational(1),Rational(1),360,2,true,false);
        EXPECT_TRUE(plan.ready); EXPECT_FALSE(plan.center.blocked); EXPECT_TRUE(plan.samples.empty());
        EXPECT_EQ(plan.fallbackReason,rifelove::quality::toString(action));
    }
}

TEST(MotionBlurExposure, FirstAndFinalExposureUseOnlyTrueSourceEdgeHolds) {
    const std::array<Rational,3> timestamps{Rational(5),Rational(1201,240),Rational(601,120)};
    const std::array<PairAction,2> actions{PairAction::interpolate,PairAction::interpolate};
    const auto first = planMotionBlurExposure(timestamps,actions,Rational(5),Rational(60),180,16,true,false);
    ASSERT_TRUE(first.ready); EXPECT_TRUE(first.edgeHeld); EXPECT_TRUE(first.fallbackReason.empty());
    EXPECT_EQ(first.samples.front().sourceIndex,0U);
    const auto final = planMotionBlurExposure(timestamps,actions,Rational(301,60),Rational(60),180,16,false,true);
    ASSERT_TRUE(final.ready); EXPECT_TRUE(final.edgeHeld); EXPECT_TRUE(final.fallbackReason.empty());
    for (const auto& sample : final.samples) { EXPECT_TRUE(sample.sourceFrame); EXPECT_EQ(sample.sourceIndex,2U); }
    EXPECT_THROW((void)planMotionBlurExposure(timestamps,actions,Rational(5),Rational(60),180,4,false,false),std::logic_error);
}
