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
#include <vector>
#include <ctime>
#include <memory>

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
static std::string esc(std::string const& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '\\': o += "\\\\"; break;
            case '\'': o += "\\'"; break;
            case '#': o += "\\#"; break;
            case '}': o += "\\}"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            default: o += c;
        }
    }
    return o;
}

static std::string fmtComments(std::vector<DLComment> const& v) {
    std::string o;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) o += ", ";
        o += "'" + esc(v[i].user) + ">\"" + esc(v[i].text) + "\"'";
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

static std::string buildFile(DLLevel const& l) {
    return "{" + fmtComments(l.recent) + "#" + fmtComments(l.top) + "}#" + buildGmd(l) + "#";
}

static bool parseFile(std::string const& data, DLLevel& out) {
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
#if DL_GEODE6
    (void)asp::fs::remove(asp::fs::Path(p));
#else
    std::error_code ec;
    std::filesystem::remove(std::filesystem::path(p), ec);
#endif
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
    return file::writeString(target, buildFile(l));
}

static void deleteLevelFromDisk(int id) {
    for (auto& o : loadAllLevels()) {
        if (o.id == id) removeFile(o.path);
    }
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
    g->m_likeCount = 0;
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
class CommentCollector : public CCObject, public LevelCommentDelegate {
public:
    DLLevel m_data;
    int m_step = 0; // 0 = waiting for recent, 1 = waiting for top, 2 = done
    bool m_done = false;

    static CommentCollector* start(DLLevel data) {
        auto c = new CommentCollector();
        c->m_data = std::move(data);
        c->retain();
        c->request();
        CCDirector::get()->getScheduler()->scheduleSelector(
            schedule_selector(CommentCollector::onTimeout), c, 15.f, 0, 0.f, false
        );
        return c;
    }

    void request() {
        auto glm = GameLevelManager::get();
        glm->m_levelCommentDelegate = this;
        glm->getLevelComments(m_data.id, 0, 20, m_step, CommentKeyType::Level);
    }

    void take(CCArray* comments) {
        auto& dst = (m_step == 0) ? m_data.recent : m_data.top;
        dst.clear();
        if (comments) {
            for (auto obj : CCArrayExt<CCObject*>(comments)) {
                auto c = typeinfo_cast<GJComment*>(obj);
                if (!c) continue;
                dst.push_back({S(c->m_userName), S(c->m_commentString)});
                if (dst.size() >= 20) break;
            }
        }
    }

    void advance() {
        m_step++;
        if (m_step >= 2) finish(true);
        else this->request();
    }

    void loadCommentsFinished(CCArray* comments, char const* key) override {
        if (m_done) return;
        take(comments);
        advance();
    }

    void loadCommentsFailed(char const* key) override {
        if (m_done) return;
        advance();
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
        if (!menu || bestCount < 8) return true;

        // top row of buttons: Create, Saved, Scores, Quests, Versus
        float maxY = -1e9f;
        std::vector<CCNode*> items;
        for (auto it : CCArrayExt<CCNode*>(menu->getChildren())) {
            if (!typeinfo_cast<CCMenuItemSpriteExtra*>(it)) continue;
            items.push_back(it);
            maxY = std::max(maxY, it->getPositionY());
        }
        std::vector<CCNode*> row;
        for (auto it : items) {
            if (it->getPositionY() > maxY - 5.f) row.push_back(it);
        }
        std::sort(row.begin(), row.end(), [](CCNode* a, CCNode* b) { return a->getPositionX() < b->getPositionX(); });

        auto spr = makeCategoryButtonSprite();
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(DLCreatorLayer::onLevelDownloads));
        btn->setID("level-downloads-button"_spr);

        // separate menu on top of the creator buttons, so the game's own layout is not changed
        auto dlMenu = CCMenu::create();
        dlMenu->setID("level-downloads-menu"_spr);
        dlMenu->setAnchorPoint(menu->getAnchorPoint());
        dlMenu->ignoreAnchorPointForPosition(menu->isIgnoreAnchorPointForPosition());
        dlMenu->setContentSize(menu->getContentSize());
        dlMenu->setPosition(menu->getPosition());

        if (row.size() >= 4) {
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

class $modify(DLLevelInfoLayer, LevelInfoLayer) {
    bool init(GJGameLevel* level, bool challenge) {
        if (!LevelInfoLayer::init(level, challenge)) return false;

        bool offline = level && level->getUserObject("offline"_spr);
        g_offlineLevelID = offline ? I(level->m_levelID) : 0;

        auto menu = findRightMenu(this);
        if (!menu) return true;
        auto spr = makeCircleButtonSprite();
        if (!spr) return true;
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(DLLevelInfoLayer::onDownloadLevel));
        btn->setID("download-level-button"_spr);

        if (menu->getLayout()) {
            // the menu is laid out automatically: add at the end (= under like/dislike)
            menu->addChild(btn);
            menu->updateLayout();
        } else {
            // place it under the lowest button, with the same spacing
            CCNode* low = nullptr;
            for (auto it : CCArrayExt<CCNode*>(menu->getChildren())) {
                if (!typeinfo_cast<CCMenuItemSpriteExtra*>(it)) continue;
                if (!low || it->getPositionY() < low->getPositionY()) low = it;
            }
            CCNode* prev = nullptr;
            for (auto it : CCArrayExt<CCNode*>(menu->getChildren())) {
                if (it == low || !typeinfo_cast<CCMenuItemSpriteExtra*>(it)) continue;
                if (!prev || it->getPositionY() < prev->getPositionY()) prev = it;
            }
            float step = prev ? prev->getPositionY() - low->getPositionY() : 55.f;
            if (step < 20.f) step = 55.f;
            float lowH = low->getContentHeight() * low->getScaleY();
            if (btn->getContentHeight() > 0.f && lowH > 0.f) btn->setScale(lowH / btn->getContentHeight());
            btn->setPosition({low->getPositionX(), low->getPositionY() - step});
            menu->addChild(btn);
        }
        return true;
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
