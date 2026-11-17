#include <gtest/gtest.h>

#include "encode/video_profile.h"

TEST(VideoProfileTest, DefaultIsFullHd30Fps) {
    std::string note;
    const VideoProfile p = video_profile::resolve("", &note);
    EXPECT_EQ(p.width, 1920);
    EXPECT_EQ(p.height, 1080);
    EXPECT_EQ(p.fps, 30);
    EXPECT_STREQ(p.name, "1080p30");
    EXPECT_TRUE(note.empty());
}

TEST(VideoProfileTest, EveryNamedProfileResolvesToItself) {
    for (const VideoProfile& expected : video_profile::kProfiles) {
        std::string note;
        const VideoProfile p = video_profile::resolve(expected.name, &note);
        EXPECT_STREQ(p.name, expected.name);
        EXPECT_EQ(p.width, expected.width);
        EXPECT_EQ(p.height, expected.height);
        EXPECT_EQ(p.fps, expected.fps);
        EXPECT_TRUE(note.empty()) << expected.name;
    }
}

TEST(VideoProfileTest, LegacyProfileKeepsTheOldSizeAndFps) {
    const VideoProfile p = video_profile::resolve("720p15");
    EXPECT_EQ(p.width, 1280);
    EXPECT_EQ(p.height, 720);
    EXPECT_EQ(p.fps, 15);
}

TEST(VideoProfileTest, NameIsCaseAndWhitespaceInsensitive) {
    EXPECT_STREQ(video_profile::resolve("  720P30\n").name, "720p30");
}

TEST(VideoProfileTest, UnknownNameFallsBackToDefaultWithAnExplanation) {
    std::string note;
    const VideoProfile p = video_profile::resolve("4k60", &note);
    EXPECT_STREQ(p.name, "1080p30");
    EXPECT_NE(note.find("4k60"), std::string::npos);
    EXPECT_NE(note.find("1080p30"), std::string::npos);
}

TEST(VideoProfileTest, AllProfilesAreEvenSizedForYuv420) {
    for (const VideoProfile& p : video_profile::kProfiles) {
        EXPECT_EQ(p.width % 2, 0);
        EXPECT_EQ(p.height % 2, 0);
        EXPECT_EQ(p.width * 9, p.height * 16) << p.name;  // 16:9 (QML のレイアウトの前提)
    }
}

TEST(VideoProfileTest, CrfIsLowerForFullHdThanForHd) {
    EXPECT_EQ(video_profile::crfFor(1920, 1080), 30);
    EXPECT_EQ(video_profile::crfFor(1280, 720), 32);
}
