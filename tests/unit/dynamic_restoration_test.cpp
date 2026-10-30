#include "restoration/dynamic_control.hpp"
#include "restoration/temporal_window.hpp"

#include <gtest/gtest.h>
#include <limits>

using namespace rifelove;
using namespace rifelove::restoration;

namespace {
Capabilities temporalCaps() {
    return {"five-frame-eval","fastdvdnet-rgb-f32-nchw16-v1",1,{Task::denoise},
        {Placement::sourceRateBeforeInterpolation},{"bt709-sdr8"},2,2,1,1,true};
}
media::DecodedFrame frame(std::size_t index) {
    media::DecodedFrame value{};
    value.index = index; value.width = 16; value.height = 16;
    value.timestamp = Rational(static_cast<std::int64_t>(index * index + index),1000);
    value.duration = Rational(1,30);
    value.color.pixelFormat = "rgb24"; value.color.bitDepth = 8; value.color.primaries = "bt709";
    value.color.transfer = "iec61966-2-1"; value.color.matrix = "gbr"; value.color.range = "pc";
    auto pixels = std::make_shared<std::vector<std::uint8_t>>(16 * 16 * 3,127);
    value.planes[0] = {pixels->data(),16 * 3}; value.storage = pixels;
    return value;
}
DynamicObservation observation(Rational pts, double automatic, double confidence = 1.0, bool cut = false) {
    DynamicObservation result{pts,{},cut};
    result.effects.fill({automatic,confidence});
    return result;
}
}

TEST(DynamicRestoration, RelativeControlsPreserveAutomaticAndHaveExactEndpoints) {
    EXPECT_DOUBLE_EQ(DynamicController::relativeStrength(.2,0),.2);
    EXPECT_DOUBLE_EQ(DynamicController::relativeStrength(.2,-1),0);
    EXPECT_DOUBLE_EQ(DynamicController::relativeStrength(.2,1),1);
    EXPECT_DOUBLE_EQ(DynamicController::relativeStrength(.2,-.5),.1);
    EXPECT_NEAR(DynamicController::relativeStrength(.2,.5),.6,1e-15);
    EXPECT_THROW((void)DynamicController::relativeStrength(.2,2),std::invalid_argument);
    EXPECT_THROW((void)DynamicController::relativeStrength(-.1,0),std::invalid_argument);
}

TEST(DynamicRestoration, ThreeSettingsAreIndependentAndMissingEstimatesStayDisabled) {
    DynamicSettings settings;
    settings.effects[0].relative = -.5;
    settings.effects[1].relative = .5;
    settings.effects[2].enabled = false;
    DynamicController controller(settings,"model-and-settings-v1");
    const auto result = controller.update(observation(0,.2));
    EXPECT_DOUBLE_EQ(result.strength[0],.1);
    EXPECT_NEAR(result.strength[1],.6,1e-15);
    EXPECT_DOUBLE_EQ(result.strength[2],0);
    DynamicController uncertain(settings,"model-and-settings-v1");
    const auto absent = uncertain.update(observation(0,.2,0));
    for (std::size_t index = 0; index < 3; ++index) {
        EXPECT_FALSE(absent.hasEstimate[index]);
        EXPECT_DOUBLE_EQ(absent.strength[index],0);
    }
}

TEST(DynamicRestoration, PtsBasedSmoothingAndSlewLimitPreventFrameToFrameJumps) {
    DynamicSettings settings;
    settings.maximumChangePerSecond = .3;
    DynamicController controller(settings,"identity");
    (void)controller.update(observation(0,.1));
    const auto next = controller.update(observation(Rational(1,30),.9));
    EXPECT_LE(next.automatic[0],.11000000000001);
    EXPECT_GT(next.automatic[0],.1);
    const auto held = controller.update(observation(Rational(2,30),.0,.1));
    EXPECT_DOUBLE_EQ(next.automatic[0],held.automatic[0]);
}

TEST(DynamicRestoration, SceneCutsResetEveryEffectWithoutLeakingPreviousScene) {
    DynamicController controller({},"identity");
    (void)controller.update(observation(0,.9));
    auto cut = observation(Rational(1,30),.1,1,true);
    cut.effects[1].confidence = 0;
    const auto result = controller.update(cut);
    EXPECT_DOUBLE_EQ(result.automatic[0],.1);
    EXPECT_DOUBLE_EQ(result.automatic[2],.1);
    EXPECT_FALSE(result.hasEstimate[1]);
    EXPECT_DOUBLE_EQ(result.strength[1],0);
    EXPECT_EQ(controller.checkpoint().sceneSerial,1U);
}

TEST(DynamicRestoration, CheckpointResumeExactlyReproducesContinuousControl) {
    DynamicController continuous({},"hash-model-analyzer-settings");
    for (int index = 0; index < 17; ++index)
        (void)continuous.update(observation(Rational(index,30),index % 2 ? .1 : .8));
    DynamicController resumed({},"hash-model-analyzer-settings");
    resumed.restore(continuous.checkpoint());
    for (int index = 17; index < 50; ++index) {
        const auto input = observation(Rational(index,30),index % 2 ? .8 : .1,1,index == 31);
        const auto a = continuous.update(input), b = resumed.update(input);
        EXPECT_EQ(a.automatic,b.automatic);
        EXPECT_EQ(a.strength,b.strength);
        EXPECT_EQ(a.hasEstimate,b.hasEstimate);
    }
}

TEST(DynamicRestoration, RejectsWrongIdentityInvalidStateAndUnorderedPtsWithoutMutation) {
    DynamicController controller({},"identity");
    (void)controller.update(observation(1,.5));
    const auto original = controller.checkpoint();
    EXPECT_THROW((void)controller.update(observation(1,.6)),std::invalid_argument);
    auto invalid = observation(2,.3); invalid.effects[2].confidence = std::numeric_limits<double>::quiet_NaN();
    EXPECT_THROW((void)controller.update(invalid),std::invalid_argument);
    auto wrong = original; wrong.identity = "other-model";
    EXPECT_THROW(controller.restore(wrong),std::invalid_argument);
    wrong = original; wrong.automatic[1] = 2;
    EXPECT_THROW(controller.restore(wrong),std::invalid_argument);
    EXPECT_EQ(controller.checkpoint().automatic,original.automatic);
    EXPECT_EQ(controller.checkpoint().lastTimestamp,original.lastTimestamp);
}

TEST(TemporalRestorationWindow, DeliversEveryRealVfrFrameInOrderWithBoundedMemory) {
    TemporalWindow window(temporalCaps());
    std::vector<Request> requests;
    for (std::size_t index = 0; index < 50; ++index) {
        auto ready = window.push(frame(index));
        requests.insert(requests.end(),ready.begin(),ready.end());
        EXPECT_LE(window.bufferedFrames(),4U);
    }
    auto end = window.finish(); requests.insert(requests.end(),end.begin(),end.end());
    ASSERT_EQ(requests.size(),50U);
    for (std::size_t index = 0; index < requests.size(); ++index) {
        EXPECT_EQ(requests[index].context[requests[index].centerIndex].index,index);
        EXPECT_EQ(requests[index].context[requests[index].centerIndex].timestamp,frame(index).timestamp);
        EXPECT_NO_THROW(validateRequest(temporalCaps(),requests[index]));
    }
    EXPECT_EQ(requests[0].missingFramesBefore,2U);
    EXPECT_EQ(requests[1].missingFramesBefore,1U);
    EXPECT_EQ(requests[48].missingFramesAfter,1U);
    EXPECT_EQ(requests[49].missingFramesAfter,2U);
    EXPECT_TRUE(window.finish().empty());
    EXPECT_THROW((void)window.push(frame(50)),std::invalid_argument);
}

TEST(TemporalRestorationWindow, NeverIncludesTheOtherSceneEvenForOneFrameClips) {
    TemporalWindow window(temporalCaps());
    std::vector<Request> requests;
    for (std::size_t index = 0; index < 9; ++index) {
        auto ready = window.push(frame(index),index == 1 || index == 3 || index == 7);
        requests.insert(requests.end(),ready.begin(),ready.end());
    }
    auto end = window.finish(); requests.insert(requests.end(),end.begin(),end.end());
    ASSERT_EQ(requests.size(),9U);
    auto scene = [](std::size_t index) { return index < 1 ? 0 : index < 3 ? 1 : index < 7 ? 2 : 3; };
    for (const auto& request : requests)
        for (const auto& context : request.context)
            EXPECT_EQ(scene(context.index),scene(request.context[request.centerIndex].index));
    EXPECT_EQ(requests[0].context.size(),1U);
    EXPECT_EQ(requests[0].missingFramesBefore,2U);
    EXPECT_EQ(requests[0].missingFramesAfter,2U);
}

TEST(TemporalRestorationWindow, RejectsBadInputBeforeChangingThePendingWindow) {
    TemporalWindow window(temporalCaps());
    EXPECT_TRUE(window.push(frame(0)).empty());
    EXPECT_THROW((void)window.push(frame(0)),std::invalid_argument);
    auto wrong = frame(1); wrong.width = 32;
    EXPECT_THROW((void)window.push(wrong),std::invalid_argument);
    EXPECT_EQ(window.bufferedFrames(),1U);
    EXPECT_TRUE(window.push(frame(1)).empty());
    EXPECT_EQ(window.push(frame(2)).size(),1U);
}
