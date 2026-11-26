// Level Downloads - Geode mod
// - "Level Downloads" button in the Creator menu (next to Create)
// - download button on every level page (under like/dislike)
// - custom "Downloaded Levels" screen (offline levels + offline comments)
#include <Geode/Geode.hpp>
#include <Geode/modify/CreatorLayer.hpp>
#include <Geode/modify/LevelInfoLayer.hpp>
#include <Geode/modify/LevelCell.hpp>
#include <Geode/modify/GameLevelManager.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/Notification.hpp>
#include <algorithm>
#include <filesystem>
#include <vector>
#include <ctime>
#include <memory>
#include <set>

using namespace geode::prelude;

// ---------------------------------------------------------------------------
// small helpers that behave the same on Geode 5 (gd::string) and Geode 6 (std::string)
// ---------------------------------------------------------------------------
template <class T>
static std::string S(T const& s) { return std::string(s.c_str()); }

template <class T>
static int I(T& v) { return static_cast<int>(v); }

template <class P>
static std::string pathStr(P const& p) {
#if DL_GEODE6
    return p.string();
#else
    return geode::utils::string::pathToString(p);
#endif
}

static bool endsWith(std::string const& s, std::string const& suf) {
    return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

// ---------------------------------------------------------------------------
// data model
// ---------------------------------------------------------------------------
struct DLComment {
    std::string user;
    std::string text;
    int likes = 0;
};

struct DLLevel {
    int id = 0;
    std::string name, desc, levelString, creator;
    int userID = 0, difficulty = 0, stars = 0, demon = 0, demonDifficulty = 0;
    int downloads = 0, likes = 0, length = 0, songID = 0, audioTrack = 0;
    int featured = 0, epic = 0, coins = 0, version = 1, gameVersion = 0;
    int ratings = 0, ratingsSum = 0;
    bool autoLevel = false;
    long long savedAt = 0;
    std::vector<DLComment> recent, top;
    std::string path; // full path of the .gmd file (only set when loaded from disk)
};

// ---------------------------------------------------------------------------
// file format
//   {<recent comments>#<top comments>}#<gmd text>#
//   each comment:  'username>"comment text"'   (comments separated by ", ")
//   inside a comment  \  '  #  }  and line breaks are escaped with a backslash
// ---------------------------------------------------------------------------
// escapes: \ and ' always, line breaks as \n \r, and > in user names
static std::string escWith(std::string const& s, bool escGt) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '\\': o += "\\\\"; break;
            case '\'': o += "\\'"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '>': o += escGt ? "\\>" : ">"; break;
            default: o += c;
        }
    }
    return o;
}

static char unescChar(char e) {
    return e == 'n' ? '\n' : e == 'r' ? '\r' : e;
}

// one comment:  'LIKES" 'USERNAME>"COMMENT TEXT" '      comments are separated by ",  "
static std::string fmtComments(std::vector<DLComment> const& v) {
    std::string o;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) o += ",  ";
        o += "'" + std::to_string(v[i].likes) + "\" '" + escWith(v[i].user, true) + ">\"" + escWith(v[i].text, false) + "\" '";
    }
    return o;
}

static std::string xmlEsc(std::string const& s) {
    std::string o;
    for (char c : s) {
        if (c == '&') o += "&amp;";
        else if (c == '<') o += "&lt;";
        else if (c == '>') o += "&gt;";
        else o += c;
    }
    return o;
}

static std::string xmlUnesc(std::string s) {
    auto rep = [&](std::string const& a, std::string const& b) {
        size_t p = 0;
        while ((p = s.find(a, p)) != std::string::npos) {
            s.replace(p, a.size(), b);
            p += b.size();
        }
    };
    rep("&lt;", "<");
    rep("&gt;", ">");
    rep("&amp;", "&");
    return s;
}

static std::string buildGmd(DLLevel const& l) {
    std::string o = "<?xml version=\"1.0\"?><plist version=\"1.0\" gjver=\"2.0\"><dict>";
    auto num = [&](char const* k, long long v) { o += fmt::format("<k>{}</k><i>{}</i>", k, v); };
    auto str = [&](char const* k, std::string const& v) { o += fmt::format("<k>{}</k><s>{}</s>", k, xmlEsc(v)); };
    num("k1", l.id);
    str("k2", l.name);
    str("k3", l.desc);
    str("k4", l.levelString);
    str("k5", l.creator);
    o += "<k>k13</k><t/>";
    num("k21", 2);
    num("k50", 35);
    num("dl_userID", l.userID);
    num("dl_difficulty", l.difficulty);
    num("dl_stars", l.stars);
    num("dl_demon", l.demon);
    num("dl_demonDifficulty", l.demonDifficulty);
    num("dl_downloads", l.downloads);
    num("dl_likes", l.likes);
    num("dl_length", l.length);
    num("dl_songID", l.songID);
    num("dl_audioTrack", l.audioTrack);
    num("dl_featured", l.featured);
    num("dl_epic", l.epic);
    num("dl_coins", l.coins);
    num("dl_version", l.version);
    num("dl_gameVersion", l.gameVersion);
    num("dl_ratings", l.ratings);
    num("dl_ratingsSum", l.ratingsSum);
    num("dl_auto", l.autoLevel ? 1 : 0);
    num("dl_savedAt", l.savedAt);
    o += "</dict></plist>";
    return o;
}

static bool gmdGet(std::string const& g, std::string const& key, std::string& out) {
    auto k = "<k>" + key + "</k>";
    auto p = g.find(k);
    if (p == std::string::npos) return false;
    p += k.size();
    if (g.compare(p, 3, "<s>") == 0) {
        auto e = g.find("</s>", p + 3);
        if (e == std::string::npos) return false;
        out = xmlUnesc(g.substr(p + 3, e - (p + 3)));
        return true;
    }
    if (g.compare(p, 3, "<i>") == 0) {
        auto e = g.find("</i>", p + 3);
        if (e == std::string::npos) return false;
        out = g.substr(p + 3, e - (p + 3));
        return true;
    }
    return false;
}

static int gmdInt(std::string const& g, std::string const& key, int def = 0) {
    std::string v;
    if (!gmdGet(g, key, v)) return def;
    return atoi(v.c_str());
}

static bool parseGmd(std::string const& g, DLLevel& l) {
    std::string v;
    if (!gmdGet(g, "k1", v)) return false;
    l.id = atoi(v.c_str());
    gmdGet(g, "k2", l.name);
    gmdGet(g, "k3", l.desc);
    gmdGet(g, "k4", l.levelString);
    gmdGet(g, "k5", l.creator);
    l.userID = gmdInt(g, "dl_userID");
    l.difficulty = gmdInt(g, "dl_difficulty");
    l.stars = gmdInt(g, "dl_stars");
    l.demon = gmdInt(g, "dl_demon");
    l.demonDifficulty = gmdInt(g, "dl_demonDifficulty");
    l.downloads = gmdInt(g, "dl_downloads");
    l.likes = gmdInt(g, "dl_likes");
    l.length = gmdInt(g, "dl_length");
    l.songID = gmdInt(g, "dl_songID");
    l.audioTrack = gmdInt(g, "dl_audioTrack");
    l.featured = gmdInt(g, "dl_featured");
    l.epic = gmdInt(g, "dl_epic");
    l.coins = gmdInt(g, "dl_coins");
    l.version = gmdInt(g, "dl_version", 1);
    l.gameVersion = gmdInt(g, "dl_gameVersion");
    l.ratings = gmdInt(g, "dl_ratings");
    l.ratingsSum = gmdInt(g, "dl_ratingsSum");
    l.autoLevel = gmdInt(g, "dl_auto") != 0;
    l.savedAt = gmdInt(g, "dl_savedAt");
    return !l.levelString.empty();
}

// file:  {<recent comments> : <top comments> }#<gmd text>#
static std::string buildFile(DLLevel const& l) {
    return "{" + fmtComments(l.recent) + " : " + fmtComments(l.top) + " }#" + buildGmd(l) + "#";
}

// old format: {<recent>#<top>}#<gmd text>#  with 'user>"text"' comments (still readable)
static bool parseFileOld(std::string const& data, DLLevel& out) {
    if (data.empty() || data[0] != '{') return false;
    size_t i = 1;
    auto parseList = [&](char endCh, std::vector<DLComment>& list) -> bool {
        while (i < data.size()) {
            char c = data[i];
            if (c == endCh) { i++; return true; }
            if (c == '\'') {
                i++;
                std::string tok;
                while (i < data.size()) {
                    char d = data[i];
                    if (d == '\\' && i + 1 < data.size()) {
                        char e = data[i + 1];
                        tok += (e == 'n') ? '\n' : (e == 'r') ? '\r' : e;
                        i += 2;
                        continue;
                    }
                    if (d == '\'') { i++; break; }
                    tok += d;
                    i++;
                }
                DLComment cm;
                auto gt = tok.find('>');
                if (gt == std::string::npos) { cm.user = "?"; cm.text = tok; }
                else { cm.user = tok.substr(0, gt); cm.text = tok.substr(gt + 1); }
                if (cm.text.size() >= 2 && cm.text.front() == '"' && cm.text.back() == '"')
                    cm.text = cm.text.substr(1, cm.text.size() - 2);
                list.push_back(std::move(cm));
                continue;
            }
            i++; // separators
        }
        return false;
    };
    if (!parseList('#', out.recent)) return false;
    if (!parseList('}', out.top)) return false;
    if (i >= data.size() || data[i] != '#') return false;
    i++;
    std::string gmd = data.substr(i);
    while (!gmd.empty() && (gmd.back() == '\n' || gmd.back() == '\r')) gmd.pop_back();
    if (!gmd.empty() && gmd.back() == '#') gmd.pop_back();
    return parseGmd(gmd, out);
}

static bool parseCommentList(std::string const& d, size_t& i, std::vector<DLComment>& out) {
    while (i < d.size()) {
        char c = d[i];
        if (c == '\'') {
            size_t j = i + 1;
            std::string likes;
            while (j < d.size() && ((d[j] >= '0' && d[j] <= '9') || d[j] == '-')) likes += d[j++];
            if (j + 2 >= d.size() || d[j] != '"' || d[j + 1] != ' ' || d[j + 2] != '\'') return false;
            j += 3;
            std::string user;
            while (j < d.size() && d[j] != '>') {
                if (d[j] == '\\' && j + 1 < d.size()) { user += unescChar(d[j + 1]); j += 2; }
                else user += d[j++];
            }
            if (j + 1 >= d.size() || d[j] != '>' || d[j + 1] != '"') return false;
            j += 2;
            std::string text;
            bool closed = false;
            while (j < d.size()) {
                if (d[j] == '\\' && j + 1 < d.size()) { text += unescChar(d[j + 1]); j += 2; continue; }
                if (d[j] == '"' && j + 2 < d.size() && d[j + 1] == ' ' && d[j + 2] == '\'') { j += 3; closed = true; break; }
                text += d[j++];
            }
            if (!closed) return false;
            DLComment cm;
            cm.user = user;
            cm.text = text;
            cm.likes = atoi(likes.c_str());
            out.push_back(std::move(cm));
            i = j;
            continue;
        }
        if (c == ':' || c == '}') return true;
        i++; // separators: commas and spaces
    }
    return false;
}

static bool parseFileNew(std::string const& d, DLLevel& out) {
    if (d.empty() || d[0] != '{') return false;
    size_t i = 1;
    if (!parseCommentList(d, i, out.recent)) return false;
    if (i >= d.size() || d[i] != ':') return false;
    i++;
    if (!parseCommentList(d, i, out.top)) return false;
    if (i + 1 >= d.size() || d[i] != '}' || d[i + 1] != '#') return false;
    i += 2;
    std::string gmd = d.substr(i);
    while (!gmd.empty() && (gmd.back() == '\n' || gmd.back() == '\r')) gmd.pop_back();
    if (!gmd.empty() && gmd.back() == '#') gmd.pop_back();
    return parseGmd(gmd, out);
}

static bool parseFile(std::string const& data, DLLevel& out) {
    DLLevel a;
    if (parseFileNew(data, a)) { out = std::move(a); return true; }
    DLLevel b;
    if (parseFileOld(data, b)) { out = std::move(b); return true; }
    return false;
}

// ---------------------------------------------------------------------------
// storage (mod config folder, one <level name>.gmd per level)
// ---------------------------------------------------------------------------
static std::string safeFileName(std::string name, int id) {
    for (auto& c : name) {
        unsigned char u = static_cast<unsigned char>(c);
        if (u < 32 || u > 126 || std::string("\\/:*?\"<>|#").find(c) != std::string::npos) c = '_';
    }
    while (!name.empty() && (name.back() == ' ' || name.back() == '.')) name.pop_back();
    while (!name.empty() && name.front() == ' ') name.erase(name.begin());
    if (name.empty()) name = fmt::format("level-{}", id);
    return name;
}

static void removeFile(std::string const& p) {
    // plain std::filesystem on both Geode versions (asp::fs has no remove in newer v6 builds)
    // the string is UTF-8, so build the path from a u8string
    std::error_code ec;
    std::filesystem::remove(
        std::filesystem::path(std::u8string(reinterpret_cast<char8_t const*>(p.data()), p.size())),
        ec
    );
}

static std::vector<DLLevel> loadAllLevels() {
    std::vector<DLLevel> out;
    auto dir = Mod::get()->getConfigDir();
    auto res = file::readDirectory(dir);
    if (!res) return out;
    for (auto& p : res.unwrap()) {
        auto ps = pathStr(p);
        if (!endsWith(ps, ".gmd")) continue;
        auto data = file::readString(p);
        if (!data) continue;
        DLLevel l;
        if (parseFile(data.unwrap(), l)) {
            l.path = ps;
            out.push_back(std::move(l));
        }
    }
    std::sort(out.begin(), out.end(), [](DLLevel const& a, DLLevel const& b) {
        return a.savedAt > b.savedAt;
    });
    return out;
}

static bool loadLevelById(int id, DLLevel& out) {
    for (auto& l : loadAllLevels()) {
        if (l.id == id) { out = l; return true; }
    }
    return false;
}

// small index of downloaded level ids (index.txt) so the level page does not need to read every file
static void writeIdIndex();

static std::set<int>& idIndex() {
    static std::set<int> ids;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        auto dir = Mod::get()->getConfigDir();
        auto data = file::readString(dir / "index.txt");
        if (data) {
            std::string t = data.unwrap();
            size_t p = 0;
            while (p < t.size()) {
                size_t e = t.find('\n', p);
                if (e == std::string::npos) e = t.size();
                int v = atoi(t.substr(p, e - p).c_str());
                if (v) ids.insert(v);
                p = e + 1;
            }
        } else {
            for (auto& l : loadAllLevels()) ids.insert(l.id);
            writeIdIndex();
        }
    }
    return ids;
}

static void writeIdIndex() {
    std::string t;
    for (int id : idIndex()) t += std::to_string(id) + "\n";
    (void)file::writeString(Mod::get()->getConfigDir() / "index.txt", t);
}

static Result<> saveLevelToDisk(DLLevel& l) {
    l.savedAt = static_cast<long long>(std::time(nullptr));
    auto dir = Mod::get()->getConfigDir();
    auto all = loadAllLevels();

    std::string base = safeFileName(l.name, l.id);
    std::string fname = base + ".gmd";
    for (auto& o : all) {
        // another level already uses this file name -> add the id
        if (o.id != l.id && endsWith(o.path, "/" + fname)) {
            fname = fmt::format("{} ({}).gmd", base, l.id);
            break;
        }
    }
    auto target = dir / fname;
    auto targetStr = pathStr(target);
    // level was renamed -> drop the old file
    for (auto& o : all) {
        if (o.id == l.id && o.path != targetStr) removeFile(o.path);
    }
    auto res = file::writeString(target, buildFile(l));
    if (res) {
        idIndex().insert(l.id);
        writeIdIndex();
    }
    return res;
}

static void deleteLevelFromDisk(int id) {
    for (auto& o : loadAllLevels()) {
        if (o.id == id) removeFile(o.path);
    }
    idIndex().erase(id);
    writeIdIndex();
}

// ---------------------------------------------------------------------------
// GJGameLevel <-> DLLevel
// ---------------------------------------------------------------------------
static DLLevel snapshotLevel(GJGameLevel* l) {
    DLLevel d;
    d.id = I(l->m_levelID);
    d.name = S(l->m_levelName);
    d.desc = S(l->m_levelDesc);
    d.levelString = S(l->m_levelString);
    d.creator = S(l->m_creatorName);
    d.userID = I(l->m_userID);
    d.difficulty = static_cast<int>(l->m_difficulty);
    d.stars = I(l->m_stars);
    d.demon = I(l->m_demon);
    d.demonDifficulty = l->m_demonDifficulty;
    d.downloads = l->m_downloads;
    d.likes = l->m_likes;
    d.length = l->m_levelLength;
    d.songID = l->m_songID;
    d.audioTrack = l->m_audioTrack;
    d.featured = l->m_featured;
    d.epic = l->m_isEpic;
    d.coins = l->m_coins;
    d.version = l->m_levelVersion;
    d.gameVersion = l->m_gameVersion;
    d.ratings = l->m_ratings;
    d.ratingsSum = l->m_ratingsSum;
    d.autoLevel = l->m_autoLevel;
    return d;
}

static GJGameLevel* makeLevel(DLLevel const& d) {
    auto l = GJGameLevel::create();
    l->m_levelID = d.id;
    l->m_levelName = d.name;
    l->m_levelDesc = d.desc;
    l->m_levelString = d.levelString;
    l->m_creatorName = d.creator;
    l->m_userID = d.userID;
    l->m_difficulty = static_cast<GJDifficulty>(d.difficulty);
    l->m_stars = d.stars;
    l->m_demon = d.demon;
    l->m_demonDifficulty = d.demonDifficulty;
    l->m_downloads = d.downloads;
    l->m_likes = d.likes;
    l->m_levelLength = d.length;
    l->m_songID = d.songID;
    l->m_audioTrack = d.audioTrack;
    l->m_featured = d.featured;
    l->m_isEpic = d.epic;
    l->m_coins = d.coins;
    l->m_levelVersion = d.version;
    l->m_gameVersion = d.gameVersion;
    l->m_ratings = d.ratings;
    l->m_ratingsSum = d.ratingsSum;
    l->m_autoLevel = d.autoLevel;
    l->m_levelType = GJLevelType::Saved;
    l->m_levelNotDownloaded = false;
    l->setUserObject("offline"_spr, CCBool::create(true));
    return l;
}

// ---------------------------------------------------------------------------
// offline comment viewing
// ---------------------------------------------------------------------------
static int g_offlineLevelID = 0; // level currently opened from the Downloaded Levels screen

static GJComment* makeComment(DLComment const& c, int levelID, int index) {
    auto g = GJComment::create();
    g->m_userName = c.user;
    g->m_commentString = c.text;
    g->m_commentID = 900000000 + index;
    g->m_levelID = levelID;
    g->m_likeCount = c.likes;
    g->m_hasLevelID = true;
    auto score = GJUserScore::create();
    if (score) {
        score->m_userName = c.user;
        g->m_userScore = score;
        score->retain();
    }
    return g;
}

class $modify(DLGameLevelManager, GameLevelManager) {
    void getLevelComments(int ID, int page, int total, int mode, CommentKeyType keytype) {
        if (g_offlineLevelID != 0 && ID == g_offlineLevelID && keytype == CommentKeyType::Level && this->m_levelCommentDelegate) {
            DLLevel dl;
            if (loadLevelById(ID, dl)) {
                // mode 0 = recent, mode 1 = top
                auto& src = (mode == 0) ? dl.recent : dl.top;
                auto arr = CCArray::create();
                size_t start = static_cast<size_t>(std::max(page, 0)) * static_cast<size_t>(std::max(total, 1));
                for (size_t i = start; i < start + total && i < src.size(); ++i) {
                    arr->addObject(makeComment(src[i], ID, static_cast<int>(i)));
                }
                std::string key = S(this->getCommentKey(ID, page, mode, keytype));
                if (auto info = typeinfo_cast<InfoLayer*>(this->m_levelCommentDelegate)) {
                    info->m_commentKey = key;
                }
                this->m_levelCommentDelegate->loadCommentsFinished(arr, key.c_str());
                this->m_levelCommentDelegate->setupPageInfo(
                    fmt::format("{}:{}:{}", src.size(), start, total), key.c_str()
                );
                return;
            }
        }
        GameLevelManager::getLevelComments(ID, page, total, mode, keytype);
    }
};

// ---------------------------------------------------------------------------
// downloading: level + 20 recent + 20 top comments
// ---------------------------------------------------------------------------
static std::string commentUser(GJComment* c) {
    std::string u = S(c->m_userName);
    if (u.empty() && c->m_userScore) u = S(c->m_userScore->m_userName);
    return u.empty() ? std::string("?") : u;
}

static void refreshDownloadControl();

class CommentCollector : public CCObject, public LevelCommentDelegate {
public:
    DLLevel m_data;
    int m_mode = 0; // 0 = recent, 1 = top
    int m_page = 0;
    bool m_done = false;

    static CommentCollector* start(DLLevel data) {
        auto c = new CommentCollector();
        c->m_data = std::move(data);
        c->m_data.recent.clear();
        c->m_data.top.clear();
        c->retain();
        c->request();
        CCDirector::get()->getScheduler()->scheduleSelector(
            schedule_selector(CommentCollector::onTimeout), c, 30.f, 0, 0.f, false
        );
        return c;
    }

    // pages of 10 until there are 20 comments in the list
    void request() {
        auto glm = GameLevelManager::get();
        glm->m_levelCommentDelegate = this;
        glm->getLevelComments(m_data.id, m_page, 10, m_mode, CommentKeyType::Level);
    }

    int take(CCArray* comments) {
        auto& dst = (m_mode == 0) ? m_data.recent : m_data.top;
        int received = 0;
        if (comments) {
            for (auto obj : CCArrayExt<CCObject*>(comments)) {
                auto c = typeinfo_cast<GJComment*>(obj);
                if (!c) continue;
                received++;
                if (dst.size() < 20) {
                    DLComment cm;
                    cm.user = commentUser(c);
                    cm.text = S(c->m_commentString);
                    cm.likes = c->m_likeCount;
                    dst.push_back(std::move(cm));
                }
            }
        }
        return received;
    }

    void next(int received) {
        auto& dst = (m_mode == 0) ? m_data.recent : m_data.top;
        if (dst.size() < 20 && received >= 10 && m_page < 3) {
            m_page++;
            this->request();
            return;
        }
        if (m_mode == 0) {
            m_mode = 1;
            m_page = 0;
            this->request();
        } else {
            this->finish(true);
        }
    }

    void loadCommentsFinished(CCArray* comments, char const* key) override {
        if (m_done) return;
        int n = take(comments);
        next(n);
    }

    void loadCommentsFailed(char const* key) override {
        if (m_done) return;
        next(0);
    }

    void onTimeout(float) {
        if (!m_done) finish(false);
    }

    void finish(bool commentsOk) {
        if (m_done) return;
        m_done = true;
        CCDirector::get()->getScheduler()->unscheduleAllForTarget(this);
        auto glm = GameLevelManager::get();
        if (glm->m_levelCommentDelegate == this) glm->m_levelCommentDelegate = nullptr;
        auto res = saveLevelToDisk(m_data);
        if (res) {
            std::string msg = fmt::format("<cy>{}</c> was saved to <cg>Downloaded Levels</c>.", m_data.name);
            if (!commentsOk) msg += "\nSome comments could not be loaded.";
            FLAlertLayer::create("Level Downloads", msg, "OK")->show();
            refreshDownloadControl();
        } else {
            FLAlertLayer::create("Level Downloads", "The level could not be saved.", "OK")->show();
        }
        this->release();
    }
};

// ---------------------------------------------------------------------------
// sprites
// ---------------------------------------------------------------------------
static CCSprite* frameSprite(char const* name) {
    auto frame = CCSpriteFrameCache::get()->spriteFrameByName(name);
    if (!frame) return nullptr;
    return CCSprite::createWithSpriteFrame(frame);
}

static CCNode* makeCategoryButtonSprite() {
    auto base = frameSprite("geode.loader/baseCategory_Big_Green.png");
    if (!base) return ButtonSprite::create("Level Downloads");
    auto size = base->getContentSize();
    if (auto icon = CCSprite::create("download-icon.png"_spr)) {
        float s = std::min(size.width * 0.52f / icon->getContentWidth(), size.height * 0.46f / icon->getContentHeight());
        icon->setScale(s);
        icon->setPosition({size.width / 2, size.height * 0.64f});
        base->addChild(icon);
    }
    auto label = CCLabelBMFont::create("Level Downloads", "bigFont.fnt");
    label->limitLabelWidth(size.width * 0.9f, 0.55f, 0.1f);
    label->setPosition({size.width / 2, size.height * 0.2f});
    base->addChild(label);
    return base;
}

static CCNode* makeCircleButtonSprite() {
    auto base = frameSprite("geode.loader/baseCircle_Large_Green.png");
    if (!base) base = frameSprite("GJ_button_01.png");
    if (!base) return nullptr;
    auto size = base->getContentSize();
    if (auto icon = CCSprite::create("download-icon.png"_spr)) {
        float s = std::min(size.width * 0.6f / icon->getContentWidth(), size.height * 0.6f / icon->getContentHeight());
        icon->setScale(s);
        icon->setPosition(size / 2);
        base->addChild(icon);
    }
    return base;
}

// gray, not pressable "already downloaded" badge
static CCNode* makeBadgeSprite() {
    CCSprite* base = frameSprite("geode.loader/baseCircle_Large_Gray.png");
    if (!base) base = frameSprite("GJ_button_06.png");
    if (!base) {
        base = frameSprite("geode.loader/baseCircle_Large_Green.png");
        if (base) base->setColor({140, 140, 140});
    }
    if (!base) return nullptr;
    auto size = base->getContentSize();
    if (auto icon = CCSprite::create("download-icon-gray.png"_spr)) {
        float sc = std::min(size.width * 0.6f / icon->getContentWidth(), size.height * 0.6f / icon->getContentHeight());
        icon->setScale(sc);
        icon->setPosition(size / 2);
        base->addChild(icon);
    }
    return base;
}

// ---------------------------------------------------------------------------
// Downloaded Levels screen
// ---------------------------------------------------------------------------
class DownloadedLevelsLayer : public CCLayer {
public:
    GJListLayer* m_list = nullptr;
    CCLabelBMFont* m_empty = nullptr;

    static DownloadedLevelsLayer* instance;

    static DownloadedLevelsLayer* create() {
        auto r = new DownloadedLevelsLayer();
        if (r->init()) { r->autorelease(); return r; }
        delete r;
        return nullptr;
    }

    static CCScene* scene() {
        auto s = CCScene::create();
        s->addChild(DownloadedLevelsLayer::create());
        return s;
    }

    ~DownloadedLevelsLayer() override {
        if (instance == this) instance = nullptr;
    }

    bool init() override {
        if (!CCLayer::init()) return false;
        instance = this;
        this->setKeypadEnabled(true);
        auto ws = CCDirector::get()->getWinSize();

        if (auto bg = CCSprite::create("GJ_gradientBG.png")) {
            bg->setAnchorPoint({0, 0});
            bg->setPosition({-5, -5});
            bg->setScaleX((ws.width + 10) / bg->getContentWidth());
            bg->setScaleY((ws.height + 10) / bg->getContentHeight());
            bg->setColor({0, 102, 255});
            bg->setZOrder(-10);
            this->addChild(bg);
        }
        if (auto c = frameSprite("GJ_sideArt_001.png")) {
            c->setAnchorPoint({0, 0});
            c->setPosition({-1, -1});
            this->addChild(c, -5);
        }
        if (auto c = frameSprite("GJ_sideArt_001.png")) {
            c->setAnchorPoint({1, 0});
            c->setFlipX(true);
            c->setPosition({ws.width + 1, -1});
            this->addChild(c, -5);
        }

        auto menu = CCMenu::create();
        menu->setPosition({0, 0});
        if (auto arrow = frameSprite("GJ_arrow_03_001.png")) {
            auto back = CCMenuItemSpriteExtra::create(arrow, this, menu_selector(DownloadedLevelsLayer::onBack));
            back->setPosition({25, ws.height - 25});
            menu->addChild(back);
        }
        this->addChild(menu, 5);

        this->refresh();
        return true;
    }

    void refresh() {
        if (m_list) { m_list->removeFromParent(); m_list = nullptr; }
        if (m_empty) { m_empty->removeFromParent(); m_empty = nullptr; }
        auto ws = CCDirector::get()->getWinSize();
        float w = 356.f, h = 220.f;

        auto arr = CCArray::create();
        for (auto& d : loadAllLevels()) arr->addObject(makeLevel(d));

        auto view = CustomListView::create(arr, BoomListType::Level, h, w);
        m_list = GJListLayer::create(view, "Downloaded Levels", ccc4(191, 114, 62, 255), w, h, 0);
        m_list->setPosition({ws.width / 2 - w / 2, ws.height / 2 - h / 2 - 8});
        this->addChild(m_list, 1);

        if (arr->count() == 0) {
            m_empty = CCLabelBMFont::create("You have not downloaded any levels yet!", "bigFont.fnt");
            m_empty->limitLabelWidth(w - 40.f, 0.6f, 0.1f);
            m_empty->setPosition({ws.width / 2, ws.height / 2 - 8});
            this->addChild(m_empty, 3);
        }
    }

    void onBack(CCObject*) { this->keyBackClicked(); }

    void keyBackClicked() override {
        CCDirector::get()->popSceneWithTransition(0.5f, PopTransition::kPopTransitionFade);
    }
};
DownloadedLevelsLayer* DownloadedLevelsLayer::instance = nullptr;

// open a downloaded level (also from the level cell tap): make the game treat it as downloaded
class $modify(DLLevelCell, LevelCell) {
    void loadFromLevel(GJGameLevel* level) {
        LevelCell::loadFromLevel(level);
        if (!level || !level->getUserObject("offline"_spr) || !m_mainMenu) return;
        auto spr = frameSprite("GJ_trashBtn_001.png");
        if (!spr) return;
        spr->setScale(0.55f);
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(DLLevelCell::onDeleteDownloaded));
        btn->setID("delete-download-button"_spr);
        float x = m_button ? m_button->getPositionX() : 130.f;
        float y = m_button ? m_button->getPositionY() : 0.f;
        btn->setPosition({x, y + (m_compactView ? 20.f : 36.f)});
        m_mainMenu->addChild(btn);
    }

    void onClick(CCObject* sender) {
        if (m_level && m_level->getUserObject("offline"_spr)) {
            m_level->m_levelNotDownloaded = false;
            auto glm = GameLevelManager::get();
            if (glm->m_onlineLevels) {
                glm->m_onlineLevels->setObject(m_level, fmt::format("{}", I(m_level->m_levelID)));
            }
        }
        LevelCell::onClick(sender);
    }

    void onDeleteDownloaded(CCObject*) {
        if (!m_level) return;
        int id = I(m_level->m_levelID);
        std::string name = S(m_level->m_levelName);
        createQuickPopup(
            "Delete Level",
            fmt::format("Delete <cy>{}</c> from your downloads?", name),
            "Cancel", "Delete",
            [id](FLAlertLayer*, bool btn2) {
                if (!btn2) return;
                deleteLevelFromDisk(id);
                if (DownloadedLevelsLayer::instance) DownloadedLevelsLayer::instance->refresh();
            }
        );
    }
};

// ---------------------------------------------------------------------------
// Creator menu: "Level Downloads" button next to Create
// ---------------------------------------------------------------------------
class $modify(DLCreatorLayer, CreatorLayer) {
    bool init() {
        if (!CreatorLayer::init()) return false;

        // find the menu that holds the creator buttons (the one with the most buttons) - no node ids needed
        CCMenu* menu = nullptr;
        int bestCount = 0;
        for (auto child : CCArrayExt<CCNode*>(this->getChildren())) {
            auto m = typeinfo_cast<CCMenu*>(child);
            if (!m || !m->getChildren()) continue;
            int count = 0;
            for (auto it : CCArrayExt<CCNode*>(m->getChildren())) {
                if (typeinfo_cast<CCMenuItemSpriteExtra*>(it)) count++;
            }
            if (count > bestCount) { bestCount = count; menu = m; }
        }

        // rows of buttons: sort by height (top first), a new row starts when y drops by more than ~40% of a button
        std::vector<CCNode*> row;
        if (menu && bestCount >= 8) {
            std::vector<CCNode*> items;
            for (auto it : CCArrayExt<CCNode*>(menu->getChildren())) {
                if (!typeinfo_cast<CCMenuItemSpriteExtra*>(it) || !it->isVisible()) continue;
                items.push_back(it);
            }
            std::sort(items.begin(), items.end(), [](CCNode* a, CCNode* b) {
                return a->getPositionY() > b->getPositionY();
            });
            float tol = 30.f;
            if (!items.empty()) tol = std::max(10.f, items[0]->getContentHeight() * items[0]->getScaleY() * 0.4f);
            size_t i = 0;
            while (i < items.size()) {
                std::vector<CCNode*> cur;
                float y0 = items[i]->getPositionY();
                while (i < items.size() && y0 - items[i]->getPositionY() <= tol) {
                    cur.push_back(items[i]);
                    i++;
                }
                if (cur.size() >= 4) { row = cur; break; } // first (top) row with 4+ buttons
            }
            std::sort(row.begin(), row.end(), [](CCNode* a, CCNode* b) {
                return a->getPositionX() < b->getPositionX();
            });
        }
        log::info("Level Downloads: creator menu buttons = {}, top row = {}", bestCount, row.size());

        auto spr = makeCategoryButtonSprite();
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(DLCreatorLayer::onLevelDownloads));
        btn->setID("level-downloads-button"_spr);

        auto dlMenu = CCMenu::create();
        dlMenu->setID("level-downloads-menu"_spr);

        if (menu && row.size() >= 4) {
            // separate menu on top of the creator buttons, so the game's own layout is not changed
            dlMenu->setAnchorPoint(menu->getAnchorPoint());
            dlMenu->ignoreAnchorPointForPosition(menu->isIgnoreAnchorPointForPosition());
            dlMenu->setContentSize(menu->getContentSize());
            dlMenu->setPosition(menu->getPosition());

            // same size as the game's buttons
            float h = row[0]->getContentHeight() * row[0]->getScaleY();
            float base = btn->getContentHeight();
            if (base > 0.f) btn->setScale(h / base);

            size_t n = row.size();
            float step = row[1]->getPositionX() - row[0]->getPositionX();
            float cx = (row.front()->getPositionX() + row.back()->getPositionX()) / 2.f;
            float y = row[0]->getPositionY();
            // new order: Create, Level Downloads, Saved, Scores, ...
            for (size_t i = 0; i <= n; ++i) {
                float x = cx + (static_cast<float>(i) - static_cast<float>(n) / 2.f) * step;
                if (i == 1) btn->setPosition({x, y});
                else row[i == 0 ? 0 : i - 1]->setPositionX(x);
            }
            dlMenu->addChild(btn);
            this->addChild(dlMenu, menu->getZOrder() + 1);
        } else {
            // could not find the row: put it in the bottom right corner
            auto ws = CCDirector::get()->getWinSize();
            dlMenu->setContentSize(ws);
            dlMenu->setAnchorPoint({0, 0});
            dlMenu->ignoreAnchorPointForPosition(false);
            dlMenu->setPosition({0, 0});
            btn->setScale(0.7f);
            btn->setPosition({ws.width - 60.f, 60.f});
            dlMenu->addChild(btn);
            this->addChild(dlMenu, 10);
        }
        return true;
    }

    void onLevelDownloads(CCObject*) {
        CCDirector::get()->pushScene(CCTransitionFade::create(0.5f, DownloadedLevelsLayer::scene()));
    }
};

// ---------------------------------------------------------------------------
// Level page: download button under like/dislike
// ---------------------------------------------------------------------------
// the column of round buttons on the right side of the level page (found without node ids)
static CCMenu* findRightMenu(CCNode* layer) {
    auto ws = CCDirector::get()->getWinSize();
    CCMenu* best = nullptr;
    int bestCount = 0;
    for (auto child : CCArrayExt<CCNode*>(layer->getChildren())) {
        auto menu = typeinfo_cast<CCMenu*>(child);
        if (!menu || !menu->getChildren()) continue;
        int count = 0;
        float sumX = 0.f;
        for (auto it : CCArrayExt<CCNode*>(menu->getChildren())) {
            if (!typeinfo_cast<CCMenuItemSpriteExtra*>(it)) continue;
            sumX += menu->convertToWorldSpace(it->getPosition()).x;
            count++;
        }
        if (count >= 4 && sumX / count > ws.width * 0.75f && count > bestCount) {
            best = menu;
            bestCount = count;
        }
    }
    return best;
}

static bool itemShowsFrame(CCNode* item, char const* frameName) {
    auto ext = typeinfo_cast<CCMenuItemSpriteExtra*>(item);
    if (!ext) return false;
    auto spr = typeinfo_cast<CCSprite*>(ext->getNormalImage());
    auto frame = CCSpriteFrameCache::get()->spriteFrameByName(frameName);
    if (!spr || !frame) return false;
    return spr->isFrameDisplayed(frame);
}

// lives as a child of the level page, so we always know if the page is still open
class DLWatcher : public CCNode {
public:
    static DLWatcher* instance;
    static DLWatcher* create() {
        auto r = new DLWatcher();
        if (r->init()) { r->autorelease(); return r; }
        delete r;
        return nullptr;
    }
    ~DLWatcher() override {
        if (instance == this) instance = nullptr;
    }
};
DLWatcher* DLWatcher::instance = nullptr;

class $modify(DLLevelInfoLayer, LevelInfoLayer) {
    bool init(GJGameLevel* level, bool challenge) {
        if (!LevelInfoLayer::init(level, challenge)) return false;

        bool offline = level && level->getUserObject("offline"_spr);
        g_offlineLevelID = offline ? I(level->m_levelID) : 0;

        if (auto watcher = DLWatcher::create()) {
            watcher->setID("level-downloads-watcher"_spr);
            this->addChild(watcher);
            DLWatcher::instance = watcher;
        }
        this->setupDownloadControl();
        return true;
    }

    bool isLevelDownloaded() {
        if (!m_level) return false;
        if (m_level->getUserObject("offline"_spr)) return true;
        return idIndex().count(I(m_level->m_levelID)) > 0;
    }

    // download button (or the gray "downloaded" badge) right next to the refresh button
    void setupDownloadControl() {
        if (auto old = this->getChildByID("level-downloads-state"_spr)) old->removeFromParent();

        auto menu = findRightMenu(this);
        if (!menu) return;

        std::vector<CCNode*> items;
        CCNode* refresh = nullptr;
        for (auto it : CCArrayExt<CCNode*>(menu->getChildren())) {
            if (!typeinfo_cast<CCMenuItemSpriteExtra*>(it)) continue;
            items.push_back(it);
            if (!refresh && itemShowsFrame(it, "GJ_updateBtn_001.png")) refresh = it;
        }
        if (items.empty()) return;
        if (!refresh) {
            std::sort(items.begin(), items.end(), [](CCNode* a, CCNode* b) {
                return a->getPositionY() > b->getPositionY();
            });
            refresh = items.size() >= 2 ? items[1] : items[0];
        }

        auto world = menu->convertToWorldSpace(refresh->getPosition());
        auto pos = this->convertToNodeSpace(world);
        float h = refresh->getContentHeight() * refresh->getScaleY();
        float w = refresh->getContentWidth() * refresh->getScaleX();
        if (h <= 0.f) h = 44.f;
        if (w <= 0.f) w = h;
        pos.x -= w + 6.f;

        auto holder = CCNode::create();
        holder->setID("level-downloads-state"_spr);

        if (this->isLevelDownloaded()) {
            auto badge = makeBadgeSprite();
            if (!badge) return;
            if (badge->getContentHeight() > 0.f) badge->setScale(h / badge->getContentHeight());
            badge->setPosition(pos);
            holder->addChild(badge);
        } else {
            auto spr = makeCircleButtonSprite();
            if (!spr) return;
            auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(DLLevelInfoLayer::onDownloadLevel));
            btn->setID("download-level-button"_spr);
            if (btn->getContentHeight() > 0.f) btn->setScale(h / btn->getContentHeight());
            btn->setPosition(pos);
            auto m = CCMenu::create();
            m->setPosition({0, 0});
            m->addChild(btn);
            holder->addChild(m);
        }
        this->addChild(holder, 20);
    }

    void onDownloadLevel(CCObject*) {
        auto level = m_level;
        if (!level) return;
        if (level->getUserObject("offline"_spr)) {
            FLAlertLayer::create("Level Downloads", "This level is already in <cg>Downloaded Levels</c>.", "OK")->show();
            return;
        }
        if (S(level->m_levelString).empty()) {
            FLAlertLayer::create("Level Downloads", "Wait until the level has finished downloading, then try again.", "OK")->show();
            return;
        }
        CommentCollector::start(snapshotLevel(level));
        Notification::create("Downloading level and comments...", NotificationIcon::Loading, 2.f)->show();
    }
};

static void refreshDownloadControl() {
    if (!DLWatcher::instance) return;
    if (auto layer = typeinfo_cast<LevelInfoLayer*>(DLWatcher::instance->getParent())) {
        static_cast<DLLevelInfoLayer*>(layer)->setupDownloadControl();
    }
}
