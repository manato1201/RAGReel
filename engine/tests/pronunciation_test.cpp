#include <gtest/gtest.h>

#include <QByteArray>

#include "narration/pronunciation.h"

using namespace pronunciation;

namespace {
std::vector<Entry> sample() {
    return parseDictionary(QByteArray(R"([
      {"surface": "Houdini", "pronunciation": "フーディニ", "accent_type": 0},
      {"surface": "Wrangle", "pronunciation": "ラングル", "accent_type": 0},
      {"surface": "Attribute Wrangle", "pronunciation": "アトリビュートラングル", "accent_type": 0},
      {"surface": "VEX", "pronunciation": "ベックス", "accent_type": 0}
    ])"));
}
}  // namespace

TEST(Pronunciation, ParsesDictionaryAndSkipsMalformedEntries) {
    const auto d = parseDictionary(QByteArray(R"([{"surface":"A","pronunciation":"エー"},
        {"surface":"","pronunciation":"x"}, {"surface":"B"}, 5])"));
    ASSERT_EQ(d.size(), 1u);
    EXPECT_EQ(d[0].surface, QStringLiteral("A"));
    EXPECT_TRUE(parseDictionary(QByteArray("not json")).empty());
    EXPECT_TRUE(loadDictionary(QStringLiteral("Z:/does/not/exist.json")).empty());
}

TEST(Pronunciation, LongestSurfaceWinsOverItsPart) {
    EXPECT_EQ(replaceTerms(QStringLiteral("Attribute Wrangleを作ります"), sample()),
              QStringLiteral("アトリビュートラングルを作ります"));
    EXPECT_EQ(replaceTerms(QStringLiteral("Wrangleを作ります"), sample()), QStringLiteral("ラングルを作ります"));
}

TEST(Pronunciation, AsciiTermsMatchWholeWordsOnly) {
    EXPECT_EQ(replaceTerms(QStringLiteral("VEXで書く"), sample()), QStringLiteral("ベックスで書く"));
    // "VEXpression" is a different word: it must not become "ベックスpression".
    EXPECT_EQ(replaceTerms(QStringLiteral("VEXpressionの欄"), sample()), QStringLiteral("VEXpressionの欄"));
}

TEST(Pronunciation, AsciiTermsAreCaseInsensitive) {
    EXPECT_EQ(replaceTerms(QStringLiteral("houdiniを開く"), sample()), QStringLiteral("フーディニを開く"));
}

TEST(Pronunciation, NonAsciiTermsMatchAnywhere) {
    const auto d = parseDictionary(QByteArray(R"([{"surface":"頂点","pronunciation":"チョウテン"}])"));
    EXPECT_EQ(replaceTerms(QStringLiteral("頂点を選ぶ"), d), QStringLiteral("チョウテンを選ぶ"));
}

TEST(Pronunciation, SpellsOutRemainingAcronyms) {
    EXPECT_EQ(spellOutAcronyms(QStringLiteral("ABCを使う")), QStringLiteral("エービーシーを使う"));
    EXPECT_EQ(spellOutAcronyms(QStringLiteral("HDAにする")), QStringLiteral("エイチディーエーにする"));
    // Mixed-case words and single letters are left alone.
    EXPECT_EQ(spellOutAcronyms(QStringLiteral("Vexと A案")), QStringLiteral("Vexと A案"));
    EXPECT_EQ(spellOutAcronyms(QStringLiteral("SOPsのこと")), QStringLiteral("SOPsのこと"));
}

TEST(Pronunciation, EmptyDictionaryChangesNothing) {
    EXPECT_EQ(replaceTerms(QStringLiteral("Houdini"), {}), QStringLiteral("Houdini"));
}
