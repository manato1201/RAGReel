#pragma once

// 書き出す動画の「大きさ・fps・画質」の組(プロファイル)。標準ライブラリだけで書いてあるので、単体テストできる。
//
// 既定は 1080p30(1920x1080・30 fps)。2026-11-17 の実証実験で、「15 fps・1280x720 で荒い。VEX など文字が多い所が
// つぶれる」と出たため(それまでの既定は 720p15)。古い設定に戻したい・速さを優先したいときは、次のどちらかで選ぶ
// (コマンドラインが環境変数より優先):
//   --video-profile 720p15           (コマンドライン)
//   RAGREEL_VIDEO_PROFILE=720p15     (環境変数。ランチャー・Houdini 側から渡すときはこちら)
// 選べる名前: 1080p30 / 1080p15 / 720p30 / 720p15。知らない名前は、既定(1080p30)にして、理由を note に返す。

#include <algorithm>
#include <string>

struct VideoProfile {
    const char* name = "1080p30";
    int width = 1920;
    int height = 1080;
    int fps = 30;
};

namespace video_profile {

inline constexpr VideoProfile kProfiles[] = {
    {"1080p30", 1920, 1080, 30},
    {"1080p15", 1920, 1080, 15},
    {"720p30", 1280, 720, 30},
    {"720p15", 1280, 720, 15},  // 2026-11-17 より前の既定
};
inline constexpr const char* kDefaultName = "1080p30";

inline std::string lowerTrimmed(std::string s) {
    const auto notSpace = [](unsigned char c) { return c != ' ' && c != '\t' && c != '\r' && c != '\n'; };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    });
    return s;
}

// 名前(空なら既定)からプロファイルを返す。知らない名前のときは既定を返し、*note に理由を入れる。
inline VideoProfile resolve(const std::string& requestedName, std::string* note = nullptr) {
    const std::string wanted = lowerTrimmed(requestedName);
    if (note) note->clear();
    if (wanted.empty()) {
        return kProfiles[0];
    }
    for (const VideoProfile& p : kProfiles) {
        if (wanted == p.name) return p;
    }
    if (note) {
        *note = "unknown video profile '" + requestedName + "' (use 1080p30 / 1080p15 / 720p30 / 720p15); using " +
                kDefaultName;
    }
    return kProfiles[0];
}

// 画素数に応じた VP9 の crf(小さいほど高画質・大きいほどファイルが小さい)。文字が多い 1080p は、720p の 32 では
// 縁がにじむので 30 にする(2026-11-17。実測は docs/technical-reference.md の「書き出しの大きさ・速さ」)。
inline int crfFor(int width, int height) {
    return static_cast<long long>(width) * height >= 1600LL * 900 ? 30 : 32;
}

}  // namespace video_profile
