// TreeFern — a cladogram editor that live-edits a JSON file (Qt 6).
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License, version 3, as published by the
// Free Software Foundation. It is distributed WITHOUT ANY WARRANTY; see the
// LICENSE file for details.
//
// Architecture
//  * Model owns the Node tree and its file: every commit() is written
//    atomically, and the file is watched so external edits are reloaded.
//  * Layout gives every node a frame *relative to its parent* (origin offset +
//    complex scale/rotation). Children are ordered lightest -> heaviest, so the
//    heaviest clade is always the last (rightmost / clockwise) branch.
//  * Canvas anchors the camera to one node's on-screen frame and re-anchors to
//    a child or the parent while zooming; all other frames are derived from it,
//    so coordinates stay in pixel range and zoom depth is effectively unlimited.
//
// File format: {"root": id, "nodes": [{id, parent?, name, info, collapsed, color?}, …]}
// color is an optional "#rrggbb" that the clade's subtree inherits.

#include <QSvgGenerator>
#include <QtWidgets>
#include <algorithm>
#include <cmath>
#include <complex>
#include <functional>
#include <memory>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>
#ifdef Q_OS_UNIX
#include <sys/resource.h>
#include <unistd.h>
#endif

// ------------------------------------------------------------- config ---
constexpr double PI = 3.14159265358979323846, DEG = PI / 180;
constexpr double DEFAULT_SPREAD = 120 * DEG, DEFAULT_SHRINK = 0.68;
constexpr double ZOOM_STEP = 1.15, FAST_ZOOM_STEP = 2.0;
constexpr int ANIM_MS = 380;  // relayout animation; camera flights take twice as long
constexpr int MAX_LABELS = 400, MAX_RECENT = 10, MAX_SEARCH = 200;
constexpr int CONFIRM_DELETE_OVER = 30;  // ask before deleting more clades than this
// export
constexpr int MAX_EXPORT_SIDE = 16384;          // px
constexpr double MAX_EXPORT_PIXELS = 40e6;      // about 160 MB of image memory
constexpr int MAX_SVG_CLADES = 20000;           // larger SVGs bring most viewers to a halt
constexpr int SLOW_EXPORT_CLADES = 50000;
// labels
constexpr double LABEL_MIN_SCREEN_LEN = 35;  // disks: smaller discs get no label
constexpr double LABEL_GAP = 2;              // min spacing between label boxes
constexpr double WRAP_COST = 0.12;           // placement penalty per extra wrapped line
constexpr int MAX_WRAP_LINES = 6;            // most lines a label may wrap onto
constexpr double STEM_COST = 0.8;            // soft penalty for covering the node's own branches
// disks style (nested discs)
constexpr double POLY_MARGIN = 0.96;      // usable fraction of a parent's radius
constexpr double POLY_GAP = 0.94;         // fraction of its wedge a child fills (both styles)
constexpr double POLY_ONLY_CHILD = 0.78;  // radius of a single child
constexpr double POLY_EXP = 0.68;         // wedge share = weight^POLY_EXP (both styles)
// standard style
constexpr double STD_NODE = 0.05;          // leaf / collapsed node radius, in its own units
constexpr double STD_WIDTH = 0.09;         // branch width, in units of the frame it leaves
constexpr double STD_FAN_STEP = 15 * DEG;  // extra fan angle per child beyond two
constexpr double STD_MAX_FAN = 300 * DEG;  // fan cap (unless the spread itself is wider)
constexpr double STD_CURVE = 0.1;          // how far a branch keeps its parent's heading
constexpr double STD_WEIGHT_EXP = 0.35;    // light sisters are scaled by (w / w_max)^this
constexpr double DEFAULT_CURL = 0.35;      // -1..1, fraction of the safe fan rotation
constexpr double STD_BACK_GAP = 25 * DEG;  // keep wedges this far from the incoming stem
constexpr double STD_CLEARANCE = 0.2;      // 3+ forks: min distance between sibling subtrees, in fork units
// camera
constexpr double MIN_PX = 0.6;          // disks: cull subtrees smaller than this on screen
constexpr double FIT_MARGIN = 120;      // px left free around a fitted tree
constexpr double CAM_MAX_SCALE = 1e9;   // max anchor scale in px (zooming into a gap)
constexpr double CAM_MIN_ROOT_PX = 20;  // min on-screen size of the whole tree
constexpr double CAM_MAX_UP = 1e9;      // stop walking up past this scale (precision)

using C = std::complex<double>;
static QPointF qpt(C c) { return {c.real(), c.imag()}; }
static C cpx(QPointF p) { return {p.x(), p.y()}; }
static QString newId() { return QUuid::createUuid().toString(QUuid::Id128).left(10); }
static QString now() { return QTime::currentTime().toString("HH:mm:ss"); }

// -------------------------------------------------------------- theme ---
struct Theme {
    bool dark;
    QColor bg, fg, dim, accent, cut, labelBg, labelBgHover, link;
    double sat, val;  // saturation / value of the depth colours
};

static Theme makeTheme(bool dark) {
    if (dark)
        return {true, {14, 17, 26}, {225, 230, 240}, {130, 140, 160}, {255, 200, 80}, {255, 90, 90},
                {20, 24, 36, 190}, {28, 34, 50, 240}, {170, 200, 255}, 0.55, 0.95};
    return {false, {246, 247, 250}, {28, 32, 42}, {105, 113, 130}, {200, 120, 0}, {215, 45, 45},
            {255, 255, 255, 205}, {255, 255, 255, 245}, {40, 90, 190}, 0.70, 0.72};
}

static Theme T = makeTheme(true);

static QString colorCss(const QColor& c) { return "color:" + c.name(); }

static QColor depthColor(int d, double bright = 1.0) {
    return QColor::fromHsvF(((200 + d * 37) % 360) / 360.0, T.sat, std::min(1.0, T.val * bright));
}

static void applyPalette(const Theme& t) {
    const auto pick = [&](QColor d, QColor l) { return t.dark ? d : l; };
    const QColor window = pick({26, 31, 46}, {234, 236, 242});
    QPalette pal = qApp->style()->standardPalette();
    for (auto [role, c] : std::initializer_list<std::pair<QPalette::ColorRole, QColor>>{
             {QPalette::Window, window}, {QPalette::ToolTipBase, window},
             {QPalette::WindowText, t.fg}, {QPalette::Text, t.fg}, {QPalette::ButtonText, t.fg},
             {QPalette::ToolTipText, t.fg}, {QPalette::PlaceholderText, t.dim},
             {QPalette::Base, pick({16, 19, 30}, {255, 255, 255})},
             {QPalette::AlternateBase, pick({22, 26, 40}, {244, 245, 248})},
             {QPalette::Button, pick({45, 54, 78}, {218, 222, 231})},
             {QPalette::Highlight, pick({58, 92, 160}, {80, 120, 200})},
             {QPalette::HighlightedText, Qt::white}})
        pal.setColor(role, c);
    for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        pal.setColor(QPalette::Disabled, role, t.dim);
    qApp->setPalette(pal);
}

// --------------------------------------------------------- data model ---
struct Node {
    QString id, name, info;
    bool collapsed = false;
    QString color;  // "#rrggbb" or empty: inherit
    std::vector<std::unique_ptr<Node>> kids;
    Node* parent = nullptr;

    static std::unique_ptr<Node> make(const QString& name, const QString& info = {}) {
        return std::unique_ptr<Node>(new Node{newId(), name, info});
    }
    Node* addChild(std::unique_ptr<Node> c) {
        c->parent = this;
        return kids.emplace_back(std::move(c)).get();
    }
    int countDesc() const {
        int n = 0;
        for (auto& k : kids) n += 1 + k->countDesc();
        return n;
    }
    int depth() const {
        int d = 0;
        for (auto p = parent; p; p = p->parent) ++d;
        return d;
    }
    bool contains(const Node* n) const {  // n is this node or one of its descendants
        for (; n; n = n->parent)
            if (n == this) return true;
        return false;
    }
    template <class F> void walk(F&& f) {  // pre-order over this subtree
        f(this);
        for (auto& k : kids) k->walk(f);
    }
};

struct Seed { QString name, info; std::vector<Seed> kids; };

static std::unique_ptr<Node> grow(const Seed& s) {
    auto n = Node::make(s.name, s.info);
    for (const Seed& k : s.kids) n->addChild(grow(k));
    return n;
}

static std::unique_ptr<Node> sampleTree() {
    return grow({"Reptiles",
        "Reptiles are an important clade of land animal, sister to modern mammals. Prior to the "
        "K-Pg Boundary Event 65 million years ago, the clade had significantly more diversity.", {
        {"Archelosauria",
         "This is the least diverse of the two basal clades of reptile, and was discovered through "
         "molecular evidence from genome sequencing, challenging the traditional belief that turtles "
         "were more closely-related to snakes than to birds. Subsequent anatomical studies have "
         "identified the presence of a sagittal crest on the supraoccipital bone and a lack of the "
         "entepicondylar foramen on the humerus as diagnostic of this clade.", {
            {"Turtles",
             "Turtles are a clade of reptile distinguished by a bony composite shell over their bodies. "
             "As with all reptiles, turtles descend from swimming, underwater vertebrates - but in the "
             "case of modern surviving turtles, their ancestors actually went back into the water, and "
             "the modern land-dwelling turtles emerged again. "},
            {"Archosauria",
             "The clade which contains modern birds and crocodilians - along with all extinct dinosaurs "
             "and pterosaurs. Diagnostic of this clade are the fenestrae behind the eyes, as well as the "
             "unique four-chambered heart design.", {
                {"Crocodilians",
                 "A clade of large, predatory, semi-aquatic reptiles. These large species are covered in "
                 "bony armor-plates and have long, flat tails and webbed feet.", {
                    {"Alligatoridae",
                     "A clade distinct for its broad snout, hidden lower teeth, and freshwater environment.", {
                        {"Alligators", "Distinct for their bony septum and cold-temperature tolerance."},
                        {"Caimans", "A clade unique for the bony ridge between the eyes and needle-like nose."}}},
                    {"Crocodiles",
                     "The clade of crocodilian most amenable to salt water thanks to specialized exchange "
                     "glands as well as an interlocking and exposed fourth-tooth."}}},
                {"Dinosaurs",
                 "The clade containing the modern birds, and also containing the most famous extinct "
                 "animals in the world thanks to books, television, and movies. Unique features of this "
                 "clade include an open hip socket and fused vertebrae in the lower back to accommodate an "
                 "upright posture.", {
                    {"Birds",
                     "The only living clade of flying reptiles, birds possess a characteristic wing with an "
                     "elongated upper arm - even in flightless species. They also lack the tails of other "
                     "theropods."}}}}}}},
        {"Lepidosaurs", "The most diverse clade of reptiles, containing the tuatara, and all lizards.", {
            {"Lizards (Squamata)",
             "The most diverse clade of lepidosaur, featuring a common \"lizard-like\" body plan and its "
             "derivatives. Unique for their paired hemipenes in male reproduction, as well as skull "
             "kinesis and skin shedding.", {
                {"Snakes",
                 "Snakes are a legless clade of lizard. They possess a highly mobile, kinetic skull; they "
                 "lack movable eyelids; and, they accommodate their paired organs sequentially in their "
                 "long, slender bodies."}}},
            {"Tuatara",
             "The sole surviving species of the ancient clade Rhynchocephalia, unique for their "
             "possession of the ancestral form of the diapsid skull, keeping intact both the upper and "
             "lower temporal bars."}}}}});
}

// Owns the tree and its backing file. Mutations go through commit(), which
// reindexes, saves, and emits treeChanged(). Functions returning QString
// return an error message, or an empty string on success.
class Model : public QObject {
    Q_OBJECT
public:
    QString path, status;

    Model() {
        auto onChange = [this] { QTimer::singleShot(150, this, &Model::checkExternal); };
        connect(&watcher_, &QFileSystemWatcher::fileChanged, this, onChange);
        connect(&watcher_, &QFileSystemWatcher::directoryChanged, this, onChange);
    }

    bool isOpen() const { return root_ != nullptr; }
    Node* root() const { return root_.get(); }
    Node* find(const QString& id) const { return id.isEmpty() ? nullptr : index_.value(id); }

    QString open(const QString& p) {  // on failure the current tree stays open
        QString err;
        auto tree = parseFile(p, err);
        if (!tree) return err;
        setTree(p, std::move(tree));
        status = "Opened " + path;
        emit treeChanged();
        return {};
    }
    QString create(const QString& p, std::unique_ptr<Node> tree) {  // overwrites p
        setTree(p, std::move(tree));
        return saveAndNotify();
    }
    QString saveAs(const QString& p) {  // editing continues in the new file
        if (!root_) return "Nothing to save";
        setPath(p);
        return saveAndNotify();
    }
    QString removeSubtree(Node* n) {
        if (!n->parent) return "Cannot delete the root clade";
        detach(n);
        commit();
        return {};
    }
    QString moveSubtree(Node* n, Node* target) {  // graft n (with descendants) under target
        if (!n->parent) return "Cannot move the root clade";
        if (n->contains(target)) return "Cannot paste a clade into itself or its descendants";
        target->addChild(detach(n));
        target->collapsed = false;
        commit();
        return {};
    }
    void commit() {
        undo_.push_back(std::exchange(current_, toJson(root_.get())));
        if (undo_.size() > 200) undo_.erase(undo_.begin());
        redo_.clear();
        reindex();
        save();
        emit treeChanged();
    }
    QString undo() { return step(undo_, redo_, "Undone"); }
    QString redo() { return step(redo_, undo_, "Redone"); }

signals:
    void treeChanged();
    void statusChanged();

private:
    std::unique_ptr<Node> root_;
    QHash<QString, Node*> index_;
    QDateTime lastMtime_;
    QFileSystemWatcher watcher_;
    // Undo history: whole-tree snapshots. current_ is the tree as of the last commit.
    QJsonObject current_;
    std::vector<QJsonObject> undo_, redo_;

    void resetHistory() {
        current_ = toJson(root_.get());
        undo_.clear();
        redo_.clear();
    }

    QString step(std::vector<QJsonObject>& from, std::vector<QJsonObject>& to, const char* done) {
        if (from.empty()) return QString("Nothing to %1").arg(&from == &undo_ ? "undo" : "redo");
        QString err;
        auto tree = fromJson(from.back(), err);
        if (!tree) return err;
        to.push_back(std::exchange(current_, from.back()));
        from.pop_back();
        root_ = std::move(tree);
        reindex();
        save();
        status = QString("%1 (%2)").arg(done, status);
        emit treeChanged();
        return {};
    }

    // Watch the file and its directory (the latter catches delete + recreate).
    void setPath(const QString& p) {
        path = QFileInfo(p).absoluteFilePath();
        for (const QStringList& old : {watcher_.files(), watcher_.directories()})
            if (!old.isEmpty()) watcher_.removePaths(old);
        if (QFile::exists(path)) watcher_.addPath(path);
        watcher_.addPath(QFileInfo(path).absolutePath());
    }

    void setTree(const QString& p, std::unique_ptr<Node> tree) {
        setPath(p);
        root_ = std::move(tree);
        reindex();
        resetHistory();
        lastMtime_ = QFileInfo(path).lastModified();
    }

    void reindex() {
        index_.clear();
        if (root_) root_->walk([this](Node* n) { index_[n->id] = n; });
    }

    static QString readColor(const QString& c) {
        static const QRegularExpression re("^#[0-9a-fA-F]{6}$");
        return re.match(c).hasMatch() ? c.toLower() : QString();
    }

    static std::unique_ptr<Node> detach(Node* n) {
        auto& v = n->parent->kids;
        auto it = std::find_if(v.begin(), v.end(), [n](auto& p) { return p.get() == n; });
        std::unique_ptr<Node> up = std::move(*it);
        v.erase(it);
        up->parent = nullptr;
        return up;
    }

    // Flat format: {"root": id, "nodes": [{id, parent?, name, info, collapsed}, …]}.
    // Children keep file order. Repairs: duplicate/missing ids get fresh ones;
    // nodes whose parent is missing, or that sit on a parent cycle, are dropped.
    static std::unique_ptr<Node> fromJson(const QJsonObject& top, QString& err) {
        struct Rec { std::unique_ptr<Node> n; QString parent; };
        std::vector<Rec> recs;
        QHash<QString, size_t> byId;
        for (const auto& v : top.value("nodes").toArray()) {
            if (!v.isObject()) continue;
            const QJsonObject o = v.toObject();
            QString id = o.value("id").toString();
            if (id.isEmpty() || byId.contains(id)) id = newId();
            byId[id] = recs.size();
            recs.push_back({std::unique_ptr<Node>(new Node{id, o.value("name").toString("Unnamed"),
                                                           o.value("info").toString(),
                                                           o.value("collapsed").toBool(false),
                                                           readColor(o.value("color").toString())}),
                            o.value("parent").toString()});
        }
        const QString rootId = top.value("root").toString();
        if (!byId.contains(rootId)) {
            err = "JSON error: missing or unknown \"root\"";
            return nullptr;
        }
        const size_t rootIdx = byId[rootId];

        // Keep only nodes whose parent chain reaches the root (iterative: no cycles, no recursion).
        std::vector<char> state(recs.size(), 0);  // 0 unknown, 1 visiting, 2 ok, 3 bad
        state[rootIdx] = 2;
        for (size_t i = 0; i < recs.size(); ++i) {
            std::vector<size_t> chain;
            size_t j = i;
            while (state[j] == 0) {
                state[j] = 1;
                chain.push_back(j);
                const auto it = byId.constFind(recs[j].parent);
                if (it == byId.cend()) { state[j] = 3; break; }
                j = *it;
            }
            const char verdict = state[j] == 2 ? 2 : 3;  // reached root, or hit missing/cycle/bad
            for (size_t c : chain) state[c] = verdict;
        }
        // Link in file order. Raw pointers are taken first because addChild moves ownership.
        std::vector<Node*> raw(recs.size());
        for (size_t i = 0; i < recs.size(); ++i) raw[i] = recs[i].n.get();
        std::unique_ptr<Node> root = std::move(recs[rootIdx].n);
        for (size_t i = 0; i < recs.size(); ++i)
            if (i != rootIdx && state[i] == 2) raw[byId[recs[i].parent]]->addChild(std::move(recs[i].n));
        return root;  // dropped records are freed with `recs`
    }

    static QJsonObject toJson(const Node* root) {
        QJsonArray nodes;
        std::vector<const Node*> stack{root};
        while (!stack.empty()) {  // pre-order; siblings keep their order
            const Node* n = stack.back();
            stack.pop_back();
            QJsonObject o{{"id", n->id}, {"name", n->name}, {"info", n->info}, {"collapsed", n->collapsed}};
            if (n->parent) o["parent"] = n->parent->id;
            if (!n->color.isEmpty()) o["color"] = n->color;
            nodes.append(o);
            for (auto it = n->kids.rbegin(); it != n->kids.rend(); ++it) stack.push_back(it->get());
        }
        return {{"root", root->id}, {"nodes", nodes}};
    }

    static std::unique_ptr<Node> parseFile(const QString& p, QString& err) {
        QFile f(p);
        if (!f.open(QIODevice::ReadOnly)) {
            err = "Cannot open " + p + ": " + f.errorString();
            return nullptr;
        }
        QJsonParseError pe;
        const auto doc = QJsonDocument::fromJson(f.readAll(), &pe);
        if (pe.error != QJsonParseError::NoError)
            err = QString("JSON error in %1 (offset %2): %3").arg(p).arg(pe.offset).arg(pe.errorString());
        else if (!doc.isObject())
            err = "JSON error in " + p + ": top level must be an object";
        else {
            auto tree = fromJson(doc.object(), err);
            if (!tree) err = err.replace("JSON error", "JSON error in " + p);
            return tree;
        }
        return nullptr;
    }

    bool save() {
        if (!root_ || path.isEmpty()) return false;
        const QJsonObject json = toJson(root_.get());
        QSaveFile f(path);  // atomic write-and-rename
        if (!f.open(QIODevice::WriteOnly) || f.write(QJsonDocument(json).toJson()) < 0 || !f.commit()) {
            status = "Cannot write " + path;
            return false;
        }
        lastMtime_ = QFileInfo(path).lastModified();
        if (!watcher_.files().contains(path)) watcher_.addPath(path);  // the rename drops the watch
        status = "Saved " + now();
        return true;
    }

    QString saveAndNotify() {
        if (!save()) return "Cannot write " + path;
        emit treeChanged();
        return {};
    }

    // Reload if the file's mtime differs from our last write; keep the old tree on parse errors.
    void checkExternal() {
        if (path.isEmpty()) return;
        if (QFile::exists(path) && !watcher_.files().contains(path)) watcher_.addPath(path);
        const QDateTime m = QFileInfo(path).lastModified();
        if (!m.isValid() || m == lastMtime_) return;
        lastMtime_ = m;
        QString err;
        if (auto tree = parseFile(path, err)) {
            root_ = std::move(tree);
            reindex();
            resetHistory();
            status = "Reloaded external change " + now();
            emit treeChanged();
        } else {
            status = err + " (kept old tree)";
            emit statusChanged();
        }
    }
};

// ------------------------------------------------------------- frames ---
// A node's frame relative to its parent's: origin o and complex scale+rotation
// z (both in parent units). dir = the direction the node grows in, in its own
// frame (used to place beside-labels).
struct Local { C o{0, 0}; C z{1, 0}; double dir = -PI / 2; };
// A frame on screen: local point q maps to pixel o + z*q.
struct Frame { C o{0, 0}; C z{1, 0}; };

static Frame compose(const Frame& f, const Local& l) { return {f.o + f.z * l.o, f.z * l.z}; }
static Frame uncompose(const Frame& f, const Local& l) {
    const C pz = f.z / l.z;
    return {f.o - pz * l.o, pz};
}
static bool finiteFrame(const Frame& f) {
    for (double v : {f.o.real(), f.o.imag(), f.z.real(), f.z.imag()})
        if (!std::isfinite(v)) return false;
    return std::abs(f.z) > 0;
}
static double lerpAngle(double a, double b, double t) { return a + std::remainder(b - a, 2 * PI) * t; }
// Rotate along the shorter arc; magnitude linearly, or geometrically for camera zoom.
static C lerpZ(C a, C b, double t, bool geometric = false) {
    const double ma = std::abs(a), mb = std::abs(b);
    return std::polar(geometric ? ma * std::pow(mb / ma, t) : ma + (mb - ma) * t,
                      lerpAngle(std::arg(a), std::arg(b), t));
}
static Local lerp(const Local& a, const Local& b, double t) {
    return {a.o + (b.o - a.o) * t, lerpZ(a.z, b.z, t), lerpAngle(a.dir, b.dir, t)};
}

// ------------------------------------------------------------- layout ---
struct Layout {
    enum Style { Standard, Disks };

    Style style = Standard;
    double spread = DEFAULT_SPREAD;  // standard: fan angle of a fork
    double shrink = DEFAULT_SHRINK;  // standard: branch length ratio
    double curl = DEFAULT_CURL;      // standard: how far lopsided forks turn toward the heavy side
    QHash<const Node*, Local> loc;   // parent-relative frames (root: relative to the view)
    QHash<const Node*, double> ext;  // subtree radius around the node's origin, own units
    QRectF bbox;                     // whole tree, in view units

    static QString styleName(Style s) { return s == Standard ? "Standard" : "Disks"; }
    static bool hidesKids(const Node* n) { return n->collapsed || n->kids.empty(); }

    int count() const { return int(loc.size()); }
    double extent(const Node* n) const { return ext.value(n, 1.0); }
    int weight(const Node* n) const { return size_.value(n, 1); }  // clade size, ignoring collapse
    double own(const Node* n) const {  // radius of the node's own footprint, own units
        return style == Disks ? 1.0 : hidesKids(n) ? STD_NODE : STD_WIDTH;
    }

    void compute(const Node* root) {
        loc.clear();
        ext.clear();
        size_.clear();
        leaves_.clear();
        bbox = {};
        if (!root) return;
        weigh(root);
        if (style == Standard) {
            loc[root] = Local{C(0, 0), C(0, -1), 0};  // grows upwards
            standard(root);
        } else {
            loc[root] = Local{};
            disks(root);
        }
        extents(root);
        bounds(root);
    }

    // Display order: lightest first, heaviest last; ties broken by visible leaves.
    std::vector<const Node*> ordered(const Node* n) const {
        std::vector<const Node*> v;
        for (auto& k : n->kids) v.push_back(k.get());
        std::stable_sort(v.begin(), v.end(), [this](const Node* a, const Node* b) {
            return std::pair(size_.value(a), leaves_.value(a)) < std::pair(size_.value(b), leaves_.value(b));
        });
        return v;
    }

private:
    QHash<const Node*, int> size_;    // node + all descendants (ignores collapse)
    QHash<const Node*, int> leaves_;  // visible leaves (respects collapse)

    void weigh(const Node* n) {
        int s = 1, l = 0;
        for (auto& k : n->kids) {
            weigh(k.get());
            s += size_[k.get()];
            l += leaves_[k.get()];
        }
        size_[n] = s;
        leaves_[n] = hidesKids(n) ? 1 : l;
    }

    double extents(const Node* n) {
        double e = own(n);
        if (!hidesKids(n))
            for (auto& k : n->kids) {
                const Local L = loc.value(k.get());
                e = std::max(e, std::abs(L.o) + std::abs(L.z) * extents(k.get()));
            }
        return ext[n] = e;
    }

    void bounds(const Node* root) {
        double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
        auto add = [&](C c, double r) {
            x0 = std::min(x0, c.real() - r); x1 = std::max(x1, c.real() + r);
            y0 = std::min(y0, c.imag() - r); y1 = std::max(y1, c.imag() + r);
        };
        std::function<void(const Node*, const Frame&)> rec = [&](const Node* n, const Frame& f) {
            add(f.o, std::abs(f.z) * own(n));
            if (!hidesKids(n))
                for (auto& k : n->kids) rec(k.get(), compose(f, loc.value(k.get())));
        };
        const Local L = loc.value(root);
        if (style == Standard) add(L.o - L.z, 0);  // the trunk below the root
        rec(root, Frame{L.o, L.z});
        bbox = QRectF(QPointF(x0, y0), QPointF(x1, y1));
    }

    // Angle of each child's wedge: proportional to weight^POLY_EXP, summing to `total`.
    std::vector<double> wedges(const std::vector<const Node*>& kids, double total) const {
        std::vector<double> th;
        double sum = 0;
        for (const Node* k : kids) sum += th.emplace_back(std::pow(weight(k), POLY_EXP));
        for (double& t : th) t *= total / sum;
        return th;
    }

    // A subtree as seen by its parent: its bounding circle (centre c on its own
    // x axis, radius R) and the pieces covering it (its node disc plus each
    // child's bounding circle), all in its own frame.
    struct Hull { double c, R; std::vector<std::pair<C, double>> pieces; };

    // Angular range, relative to the child's axis and seen from the parent's
    // fork, covered by a child placed at distance 1 with scale s, every piece
    // grown by `pad` (parent units). Returns a full turn if a piece reaches the
    // fork or the range is too wide to pack.
    static std::pair<double, double> angularSpan(const std::vector<std::pair<C, double>>& pieces, double s, double pad) {
        double lo = 1e300, hi = -1e300;
        for (const auto& [p, r] : pieces) {
            const C q = 1.0 + s * p;
            const double d = std::abs(q), rr = s * r + pad;
            if (rr >= d * 0.999) return {-PI, PI};
            const double a = std::arg(q), h = std::asin(rr / d);
            lo = std::min(lo, a - h);
            hi = std::max(hi, a + h);
        }
        if (hi - lo >= PI) return {-PI, PI};
        return {lo, hi};
    }

    // Disks: children sit on a ring inside the parent disc, clockwise from 12
    // o'clock. Each is the largest disc inscribed in its wedge (wedges capped
    // at a half-turn), so siblings can never overlap.
    void disks(const Node* n) {
        if (hidesKids(n)) return;
        const auto kids = ordered(n);
        if (kids.size() == 1) {  // sit low, leaving room for the parent's label
            const double r = POLY_MARGIN * POLY_ONLY_CHILD;
            loc[kids[0]] = Local{C(0, POLY_MARGIN - r), C(r, 0), PI / 2};
            return disks(kids[0]);
        }
        std::vector<double> th = wedges(kids, 2 * PI);
        const size_t big = std::max_element(th.begin(), th.end()) - th.begin();
        if (th[big] > PI) {  // only one wedge can exceed a half-turn: cap it, rescale the rest
            const double rest = 2 * PI - th[big];
            for (size_t i = 0; i < th.size(); ++i) th[i] = i == big ? PI : th[i] * PI / rest;
        }
        const double rho = POLY_MARGIN / (1 + std::sin(th[big] / 2));  // biggest touches the rim
        double a = -PI / 2;
        for (size_t i = 0; i < kids.size(); ++i) {
            const double mid = a + th[i] / 2;
            loc[kids[i]] = Local{std::polar(rho, mid), C(rho * std::sin(th[i] / 2) * POLY_GAP, 0), mid};
            disks(kids[i]);
            a += th[i];
        }
    }

    // Smallest circle centred on the real axis that encloses all the circles
    // (the radius is convex in the centre, so ternary search).
    static std::pair<double, double> enclose(const std::vector<std::pair<C, double>>& cs) {
        double lo = 1e300, hi = -1e300;
        for (auto& [p, r] : cs) { lo = std::min(lo, p.real() - r); hi = std::max(hi, p.real() + r); }
        auto f = [&](double x) {
            double m = 0;
            for (auto& [p, r] : cs) m = std::max(m, std::abs(p - C(x, 0)) + r);
            return m;
        };
        for (int it = 0; it < 60; ++it) {
            const double a = lo + (hi - lo) / 3, b = hi - (hi - lo) / 3;
            if (f(a) < f(b)) hi = b; else lo = a;
        }
        const double x = (lo + hi) / 2;
        return {x, f(x)};
    }

    // Standard: a node's frame has its origin at its fork, +x along its growth
    // direction, and unit = its outgoing branch length. All children leave the
    // fork together. Two-child forks split the fan by weight and inscribe each
    // child's bounding circle in its wedge. Forks of 3+ children are packed by
    // the angular range each child covers when grown by half the clearance (see
    // angularSpan): neighbours abut, so sibling subtrees stay a clearance apart;
    // slack becomes even gaps, and if the fan is too tight
    // every child shrinks by a common factor. Returns the bounding circle and pieces.
    Hull standard(const Node* n) {
        if (hidesKids(n)) return {0, STD_NODE, {{C(0, 0), STD_NODE}}};
        const auto kids = ordered(n);
        const size_t m = kids.size();
        // Children's hulls don't depend on where they are placed, so compute them first.
        std::vector<Hull> sub(m);
        for (size_t i = 0; i < m; ++i) sub[i] = standard(kids[i]);
        std::vector<std::pair<C, double>> circles{{C(0, 0), STD_WIDTH}};
        auto placeAt = [&](size_t i, double mid, double s) {
            const C u = std::polar(1.0, mid);
            loc[kids[i]] = Local{u, u * s, 0};
            circles.push_back({u * (1 + s * sub[i].c), s * sub[i].R});
        };
        auto place = [&](size_t i, double mid, double halfWedge, double want) {
            const double c = sub[i].c, R = sub[i].R;
            const double sb = std::sin(std::min(halfWedge, PI / 2)) * POLY_GAP;
            placeAt(i, mid, R > c * sb ? std::min(want, sb / (R - c * sb)) : want);  // inscribed in the wedge
        };
        // Rigidly rotating the fan keeps the children disjoint and the enclose()
        // circle valid; it only has to stay clear of the incoming stem at angle pi.
        auto shiftFor = [&](double fan, double imbalance) {
            const double room = std::max(0.0, PI - fan / 2 - STD_BACK_GAP);
            return std::clamp(curl, -1.0, 1.0) * imbalance * room;
        };
        if (m == 1) {
            place(0, shiftFor(PI, 1.0), PI / 2, std::sqrt(shrink));
        } else {
            const double fan = std::min(std::max(spread, STD_MAX_FAN), spread + (m - 2) * STD_FAN_STEP);
            const double wmax = weight(kids.back());
            std::vector<double> want(m);
            double rest = 0;
            for (size_t i = 0; i < m; ++i) {
                want[i] = i == m - 1 ? std::sqrt(shrink)  // main stem
                                     : shrink * std::pow(weight(kids[i]) / wmax, STD_WEIGHT_EXP);
                if (i + 1 < m) rest += weight(kids[i]);
            }
            const double imbalance = std::max(0.0, (wmax - rest) / (wmax + rest));  // 0 = balanced fork
            double a = -fan / 2 + shiftFor(fan, imbalance);
            if (m == 2) {  // bifurcation: unchanged
                const std::vector<double> th = wedges(kids, fan);
                for (size_t i = 0; i < m; ++i) {
                    place(i, a + th[i] / 2, th[i] / 2, want[i]);
                    a += th[i];
                }
            } else {
                // Half the clearance around every child; a fork too crowded to give even
                // its leaves that much (each takes 2·asin(pad)) gets proportionally less.
                const double pad = std::min(STD_CLEARANCE / 2, std::sin(0.9 * fan / (2.0 * double(m))));
                std::vector<std::pair<double, double>> sp(m);
                auto total = [&](double k) {  // fan used when every child is scaled by k
                    double t = 0;
                    for (size_t i = 0; i < m; ++i) {
                        sp[i] = angularSpan(sub[i].pieces, k * want[i], pad);
                        t += sp[i].second - sp[i].first;
                    }
                    return t;
                };
                double k = 1;
                if (total(1) > fan) {
                    double lo = 1e-3, hi = 1;
                    for (int it = 0; it < 40; ++it) {
                        const double mk = (lo + hi) / 2;
                        (total(mk) <= fan ? lo : hi) = mk;
                    }
                    k = lo;
                }
                const double used = total(k);  // also leaves sp[] for this k
                const double gap = std::max(0.0, fan - used) / double(m - 1);
                for (size_t i = 0; i < m; ++i) {
                    placeAt(i, a - sp[i].first, k * want[i]);  // this child's range starts at a
                    a += sp[i].second - sp[i].first + gap;
                }
            }
        }
        const auto [x, r] = enclose(circles);
        return {x, r, std::move(circles)};
    }
};

// ---------------------------------------------------- drawing helpers ---
// Liang–Barsky: clip segment a-b to r; false if it lies fully outside.
static bool clipLine(const QRectF& r, QPointF& a, QPointF& b) {
    double t0 = 0, t1 = 1, dx = b.x() - a.x(), dy = b.y() - a.y();
    const double p[4] = {-dx, dx, -dy, dy};
    const double q[4] = {a.x() - r.left(), r.right() - a.x(), a.y() - r.top(), r.bottom() - a.y()};
    for (int i = 0; i < 4; ++i) {
        if (p[i] == 0) {
            if (q[i] < 0) return false;
            continue;
        }
        const double t = q[i] / p[i];
        if (p[i] < 0) { if (t > t1) return false; t0 = std::max(t0, t); }
        else          { if (t < t0) return false; t1 = std::min(t1, t); }
    }
    const QPointF o = a, d(dx, dy);
    a = o + d * t0;
    b = o + d * t1;
    return true;
}

static double segDist(QPointF p, QPointF a, QPointF b) {
    const QPointF d = b - a;
    const double l2 = QPointF::dotProduct(d, d);
    const double t = l2 > 0 ? std::clamp(QPointF::dotProduct(p - a, d) / l2, 0.0, 1.0) : 0.0;
    return QLineF(p, a + d * t).length();
}

// Tapered quadratic branch a -> b (control point c), widths wa -> wb.
// Returns the sampled centre line (for hit tests and label obstacles).
static std::vector<QPointF> drawTaper(QPainter& p, QPointF a, QPointF c, QPointF b, double wa, double wb,
                                      const QColor& col, const QRectF& view) {
    constexpr int N = 12;
    std::vector<QPointF> mid(N + 1);
    QPolygonF left, right;
    for (int i = 0; i <= N; ++i) {
        const double t = double(i) / N, u = 1 - t;
        mid[i] = a * (u * u) + c * (2 * u * t) + b * (t * t);
        QPointF tan = (c - a) * (2 * u) + (b - c) * (2 * t);
        double len = std::hypot(tan.x(), tan.y());
        if (len < 1e-9) { tan = b - a; len = std::max(1e-9, std::hypot(tan.x(), tan.y())); }
        const QPointF nrm = QPointF(-tan.y(), tan.x()) / len * ((wa + (wb - wa) * t) / 2);
        left << mid[i] + nrm;
        right.prepend(mid[i] - nrm);
    }
    QPolygonF poly = left + right;
    const QRectF br = poly.boundingRect();
    if (br.width() > 1e5 || br.height() > 1e5) poly = poly.intersected(QPolygonF(view));  // keep Qt's rasteriser sane
    p.setPen(Qt::NoPen);
    p.setBrush(col);
    p.drawPolygon(poly);
    return mid;
}

// Box of size w*h whose side facing direction (ux, uy) touches (px, py).
static QRectF anchoredRect(double w, double h, double px, double py, double ux, double uy) {
    const double l = ux > 0.35 ? px : ux < -0.35 ? px - w : px - w / 2;
    const double t = uy > 0.35 ? py : uy < -0.35 ? py - h : py - h / 2;
    return QRectF(std::round(l), std::round(t), w, h);
}

// Ways to lay out a label: for each line count k = 1..MAX_WRAP_LINES, the split
// that minimises the widest line (exact linear-partition DP). An option is kept
// only if it is narrower than every option with fewer lines. Never truncates.
static std::vector<QStringList> wrapOptions(const QStringList& words, const QFontMetricsF& fm) {
    const int n = words.size();
    std::vector<double> pre(n + 1, 0);
    for (int i = 0; i < n; ++i) pre[i + 1] = pre[i] + fm.horizontalAdvance(words[i]);
    const double sp = fm.horizontalAdvance(' ');
    auto W = [&](int a, int b) { return pre[b] - pre[a] + (b - a - 1) * sp; };

    const int K = std::min(n, MAX_WRAP_LINES);
    constexpr double INF = 1e300;
    // best[k][j]: smallest possible widest line when words [0, j) fill k lines; cut = start of the last line
    std::vector<std::vector<double>> best(K + 1, std::vector<double>(n + 1, INF));
    std::vector<std::vector<int>> cut(K + 1, std::vector<int>(n + 1, 0));
    best[0][0] = 0;
    for (int k = 1; k <= K; ++k)
        for (int j = k; j <= n; ++j)
            for (int i = k - 1; i < j; ++i)
                if (const double m = std::max(best[k - 1][i], W(i, j)); m < best[k][j]) {
                    best[k][j] = m;
                    cut[k][j] = i;
                }

    std::vector<QStringList> out;
    double prevW = INF;
    for (int k = 1; k <= K; ++k) {
        if (!(best[k][n] < prevW - 0.5)) continue;  // must be visibly narrower
        QStringList lines;
        for (int kk = k, j = n; kk > 0; j = cut[kk][j], --kk)
            lines.prepend(words.mid(cut[kk][j], j - cut[kk][j]).join(' '));
        out.push_back(lines);
        prevW = best[k][n];
    }
    return out;
}

// ------------------------------------------------------------- canvas ---
class Canvas : public QWidget {
    Q_OBJECT
public:
    enum class LabelMode { Over, Beside };

    Layout lay;  // target layout; what is drawn (shownLoc) animates towards it
    QString selId, cutId;
    LabelMode labelMode = LabelMode::Over;

    explicit Canvas(Model* m) : model(m) {
        setFocusPolicy(Qt::StrongFocus);
        setMinimumSize(300, 200);
        setMouseTracking(true);
        auto setup = [](QVariantAnimation& a, int ms, QEasingCurve::Type curve) {
            a.setDuration(ms);
            a.setStartValue(0.0);
            a.setEndValue(1.0);
            a.setEasingCurve(curve);
        };
        setup(anim, ANIM_MS, QEasingCurve::OutCubic);
        setup(camAnim, 2 * ANIM_MS, QEasingCurve::InOutCubic);
        connect(&anim, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) { applyFrame(v.toDouble()); });
        connect(&anim, &QVariantAnimation::finished, this, [this] {
            applyFrame(1);
            following = false;
            reanchor();
        });
        connect(&camAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
            const double t = v.toDouble();
            cam.f = {camFrom_.o + (camTo_.o - camFrom_.o) * t, lerpZ(camFrom_.z, camTo_.z, t, true)};
            viewUpdated();
        });
        connect(&camAnim, &QVariantAnimation::finished, this, [this] { reanchor(); viewUpdated(); });
        relayout(false);
    }

    bool disks() const { return lay.style == Layout::Disks; }

    // Recompute the layout and animate from what is on screen now. While it
    // animates, the camera keeps the selected node fixed until the user pans or zooms.
    void relayout(bool animate = true) {
        fromLoc_ = animate ? shownLocById_ : QHash<QString, Local>();
        lay.compute(model->root());
        computeTints();
        anim.stop();
        havePrev_ = false;
        shownLoc.clear();  // keys may dangle after a reload
        fixAnchor();
        following = !fromLoc_.isEmpty() && !selId.isEmpty();
        applyFrame(fromLoc_.isEmpty() ? 1 : 0);
        if (!fromLoc_.isEmpty()) anim.start();
    }

    void focusOn(const Node* n) {
        if (!n || !lay.loc.contains(n)) return;
        double mag = std::min(vw(), vh()) * 0.42;
        if (!disks()) {
            const auto f = screenFrame(n);
            mag = std::min(std::max(f ? std::abs(f->z) : 120.0, 120.0), mag / lay.extent(n));
        }
        flyTo(n, {centre(), std::polar(mag, absAngle(n))});
    }

    void ensureVisible(const Node* n) {
        if (!n || !lay.loc.contains(n)) return;
        const auto f = screenFrame(n);
        if (!f) return focusOn(n);
        const double s = std::abs(f->z), lo = std::min(vw(), vh());
        const bool inside = vrect().adjusted(60, 60, -60, -60).contains(qpt(f->o));
        if (!inside || (disks() ? s < 30 || s > lo * 0.6 : s < 15)) focusOn(n);
    }

    void fitView() {
        following = false;
        resetCam();
        viewUpdated();
    }

    // Zoom relative to the fitted view, accumulated in log space to survive extreme depths.
    QString zoomText() const {
        const Node* a = anchorNode();
        if (!a) return "-";
        double z = std::log10(std::abs(cam.f.z) / fitScale());
        for (const Node* x = a; x; x = x->parent) z -= std::log10(std::abs(sloc(x).z));
        return z < 4 ? QString::number(std::pow(10.0, z), 'f', 2) + "x" : QString("10^%1x").arg(z, 0, 'f', 1);
    }

    // Exports: the size in logical px of the view or the whole tree (fitted,
    // with the view's long side), and the pixel factor, capped so the image
    // can be allocated.
    struct ExportSize { int w, h; double factor; int pw, ph; };
    ExportSize exportSize(bool whole, double factor) const {
        double w = width(), h = height();
        if (whole) {
            const double bw = std::max(lay.bbox.width(), 1e-9), bh = std::max(lay.bbox.height(), 1e-9), L = std::max(w, h);
            std::tie(w, h) = bw >= bh ? std::pair(L, std::max(200.0, L * bh / bw)) : std::pair(std::max(200.0, L * bw / bh), L);
        }
        const int iw = int(std::lround(w)), ih = int(std::lround(h));
        const double f = std::min({factor, MAX_EXPORT_SIDE / double(std::max(iw, ih)), std::sqrt(MAX_EXPORT_PIXELS / (double(iw) * ih))});
        return {iw, ih, f, int(std::lround(iw * f)), int(std::lround(ih * f))};
    }
    // Clades an export would draw: every laid-out one for the whole tree, else those on screen.
    int exportCount(bool whole) const { return whole ? lay.count() : int(items_.size()); }

    // Paint at logical size w x h into p (which the caller scales), then
    // restore the on-screen view.
    void renderTo(QPainter& p, int w, int h, bool whole, bool transparent) {
        const Cam savedCam = cam;
        const QString savedHover = std::exchange(hoverId, QString());
        vp_ = QSize(w, h);
        exporting_ = true;
        transparent_ = transparent;
        if (whole) resetCam();
        paint(p);
        vp_ = QSize();
        exporting_ = transparent_ = false;
        cam = savedCam;
        hoverId = savedHover;
        update();  // items_ and hit areas are rebuilt by the next paint
    }

signals:
    void clicked(const QString& id);
    void doubleClicked(const QString& id);
    void contextMenu(const QString& id, const QPoint& global);
    void viewChanged();

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        paint(p);
    }

    // A clade's info appears as a tooltip while the pointer rests on it.
    bool event(QEvent* e) override {
        if (e->type() != QEvent::ToolTip) return QWidget::event(e);
        const auto he = static_cast<QHelpEvent*>(e);
        const Node* n = hitTest(he->pos());
        if (!n || n->info.trimmed().isEmpty()) {
            QToolTip::hideText();
            e->ignore();
            return true;
        }
        QString info = n->info.trimmed();
        if (info.size() > 600) info = info.left(600).trimmed() + "…";
        QToolTip::showText(he->globalPos(), QString("<b>%1</b><br>%2").arg(n->name.toHtmlEscaped(),
                           info.toHtmlEscaped().replace('\n', "<br>")), this);
        return true;
    }

    void mousePressEvent(QMouseEvent* e) override {
        QToolTip::hideText();
        pressPos = lastPos = e->position();
        dragBtn = e->button();
        moved = false;
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (dragBtn == Qt::NoButton) return updateHover(e->position());
        moved = moved || QLineF(e->position(), pressPos).length() > 4;
        if (moved) {
            following = false;
            panBy(e->position() - lastPos);
            viewUpdated();
        }
        lastPos = e->position();
    }
    void mouseReleaseEvent(QMouseEvent* e) override {
        const auto btn = std::exchange(dragBtn, Qt::NoButton);
        if (moved || btn != e->button()) return updateHover(e->position());
        const Node* hit = hitTest(e->position());
        const QString id = hit ? hit->id : QString();
        if (btn == Qt::LeftButton) emit clicked(id);
        else if (btn == Qt::RightButton) emit contextMenu(id, e->globalPosition().toPoint());
    }
    void mouseDoubleClickEvent(QMouseEvent* e) override {
        dragBtn = Qt::NoButton;
        if (e->button() != Qt::LeftButton) return;
        if (const Node* hit = hitTest(e->position())) emit doubleClicked(hit->id);
    }
    void wheelEvent(QWheelEvent* e) override {
        const bool fast = e->modifiers() & Qt::ShiftModifier;
        double notches = e->angleDelta().y() / 120.0;
        if (notches == 0 && fast) notches = e->angleDelta().x() / 120.0;  // some platforms turn Shift+wheel horizontal
        if (notches == 0) return;
        QToolTip::hideText();
        following = false;
        zoomBy(std::pow(fast ? FAST_ZOOM_STEP : ZOOM_STEP, notches), e->position());
        update();
        updateHover(e->position());
        emit viewChanged();
    }
    void leaveEvent(QEvent*) override {
        if (!hoverId.isEmpty()) { hoverId.clear(); update(); }
        unsetCursor();
    }

private:
    // A node as drawn this frame: screen centre, drawn radius, frame scale (px),
    // growth direction on screen, tree depth.
    struct Item { const Node* n; QPointF s; double r, lenPx, angle; int depth; double branchPx; };
    // The branch into `child`, from its parent's frame pf to its own frame cf (depth = parent's).
    struct Branch { const Node* child; Frame pf, cf; int depth; };
    struct HitSeg { QPointF a, b; double hw; const Node* n; };
    struct Seg { QPointF a, b; double pad; QRectF box; const Node *p, *c; };  // branch piece as a label obstacle
    struct Cam { QString anchor; Frame f{C(0, 0), C(300, 0)}; };  // the anchor node's frame on screen

    Model* model;
    Cam cam;
    QSize vp_;  // set while rendering an export: the viewport size used instead of the widget's
    bool exporting_ = false, transparent_ = false;
    QHash<const Node*, std::pair<QColor, int>> tint_;  // chosen colour and the depth of the clade that set it
    QHash<const Node*, Local> shownLoc;  // frames as drawn now
    // animation state is keyed by id: node pointers may have been freed by a reload
    QVariantAnimation anim, camAnim;
    QHash<QString, Local> fromLoc_, shownLocById_;
    Frame camFrom_{}, camTo_{};
    bool following = false, havePrev_ = false;  // pin the selected node at followPrev_ during relayout
    C followPrev_;
    // input and last-paint state
    QPointF pressPos, lastPos;
    Qt::MouseButton dragBtn = Qt::NoButton;
    bool moved = false;
    QString hoverId;
    std::vector<Item> items_;
    std::vector<Branch> branches_;
    std::vector<HitSeg> hitSegs_;
    std::vector<std::pair<QRectF, QString>> labelHits;

    void viewUpdated() { update(); emit viewChanged(); }
    int vw() const { return vp_.isValid() ? vp_.width() : width(); }
    int vh() const { return vp_.isValid() ? vp_.height() : height(); }
    QRectF vrect() const { return QRectF(0, 0, vw(), vh()); }

    void computeTints() {
        tint_.clear();
        if (!model->root()) return;
        struct Step { const Node* n; std::optional<std::pair<QColor, int>> t; int depth; };
        std::vector<Step> stack{{model->root(), std::nullopt, 0}};
        while (!stack.empty()) {
            auto [n, t, d] = stack.back();
            stack.pop_back();
            if (!n->color.isEmpty()) t = std::pair(QColor(n->color), d);
            if (t) tint_.insert(n, *t);
            for (auto& k : n->kids) stack.push_back({k.get(), t, d + 1});
        }
    }

    // A clade's colour: its own or an ancestor's chosen hue at the theme's
    // brightness, every other level a little darker so nesting stays visible.
    // Uncoloured clades keep the depth colours.
    QColor nodeColor(const Node* n, int d, double bright) const {
        const auto it = tint_.constFind(n);
        if (it == tint_.cend()) return depthColor(d, bright);
        const QColor& c = it->first;
        return QColor::fromHsvF(std::max(0.0f, c.hsvHueF()), c.hsvSaturationF(),
                                std::min(1.0, T.val * bright) * ((d - it->second) % 2 ? 0.84 : 1.0));
    }
    C centre() const { return C(vw() / 2.0, vh() / 2.0); }
    const Node* anchorNode() const { return model->find(cam.anchor); }
    bool inCut(const Node* n) const { const Node* c = model->find(cutId); return c && c->contains(n); }

    Local sloc(const Node* n) const {
        if (auto it = shownLoc.constFind(n); it != shownLoc.cend()) return *it;
        return lay.loc.value(n, Local{});
    }

    // rotation of n's frame on screen (fixed by the layout; the view itself never rotates)
    double absAngle(const Node* n) const {
        double a = 0;
        for (const Node* x = n; x; x = x->parent) a += std::arg(lay.loc.value(x, Local{}).z);
        return a;
    }

    bool covers(C c, double r) const {  // the circle contains the whole widget
        for (QPointF q : {QPointF(0, 0), QPointF(vw(), 0), QPointF(0, vh()), QPointF(vw(), vh())})
            if (std::norm(cpx(q) - c) > r * r) return false;
        return true;
    }

    double fitScale() const {
        const double bw = std::max(lay.bbox.width(), 2.0), bh = std::max(lay.bbox.height(), 2.0);
        return std::min(std::max(100.0, vw() - FIT_MARGIN) / bw, std::max(100.0, vh() - FIT_MARGIN) / bh);
    }

    void resetCam() {
        camAnim.stop();
        const Node* r = model->root();
        if (!r || !lay.loc.contains(r)) return;
        const double s = fitScale();
        cam.anchor = r->id;
        cam.f = compose(Frame{centre() - s * cpx(lay.bbox.center()), C(s, 0)}, lay.loc.value(r));
        reanchor();
    }

    // Keep the anchor valid after the tree changed (climb out of collapsed subtrees).
    void fixAnchor() {
        if (!model->root()) return;
        const Node* a = anchorNode();
        if (!a) return resetCam();
        while (!lay.loc.contains(a) && a->parent) {
            cam.f = uncompose(cam.f, fromLoc_.value(a->id, Local{}));
            a = a->parent;
        }
        cam.anchor = a->id;
    }

    // n's frame on screen, reached from the anchor via their common ancestor.
    std::optional<Frame> screenFrame(const Node* n) const {
        const Node* a = anchorNode();
        if (!a || !n || !lay.loc.contains(n)) return std::nullopt;
        const Node* l = a;
        while (!l->contains(n)) l = l->parent;
        Frame d = cam.f;
        for (const Node* x = a; x != l; x = x->parent) d = uncompose(d, sloc(x));
        std::vector<const Node*> path;
        for (const Node* x = n; x != l; x = x->parent) path.push_back(x);
        for (auto it = path.rbegin(); it != path.rend(); ++it) d = compose(d, sloc(*it));
        return finiteFrame(d) ? std::optional(d) : std::nullopt;
    }

    // Where the branch into n starts: its parent's frame (standard: a unit trunk below the root).
    std::optional<Frame> stemFrom(const Node* n, const Frame& f) const {
        if (n->parent) return uncompose(f, sloc(n));
        if (lay.style == Layout::Standard) return Frame{f.o - f.z, f.z};
        return std::nullopt;
    }

    // Move the anchor up while its subtree is small on screen, and down into
    // the child under the view centre while that child is big. The gap between
    // the thresholds prevents ping-pong.
    void reanchor() {
        if (camAnim.state() == QAbstractAnimation::Running) return;
        const Node* a = anchorNode();
        if (!a) return;
        const double lo = std::max(1, std::min(vw(), vh())), hi = std::max(vw(), vh());
        for (int guard = 0; guard < 100000; ++guard) {
            const double R = std::abs(cam.f.z) * lay.extent(a);
            if (a->parent && R < lo * 0.25) {
                cam.f = uncompose(cam.f, sloc(a));
                a = a->parent;
                continue;
            }
            const Node* next = nullptr;
            if (R > hi * 2 && !Layout::hidesKids(a))
                for (auto& k : a->kids) {
                    const Frame cf = compose(cam.f, sloc(k.get()));
                    const double kr = std::abs(cf.z) * lay.extent(k.get());
                    if (kr > lo * 0.5 && std::abs(cf.o - centre()) < kr) {
                        next = k.get();
                        cam.f = cf;
                        break;
                    }
                }
            if (!next) break;
            a = next;
        }
        cam.anchor = a->id;
    }

    void flyTo(const Node* n, Frame target) {
        camAnim.stop();
        following = false;
        const auto d = screenFrame(n);
        cam.anchor = n->id;
        if (d && std::abs(d->z) >= 1e-6 && std::abs(d->z) <= 1e12) {
            cam.f = camFrom_ = *d;
            camTo_ = target;
            camAnim.start();
            return;
        }
        cam.f = target;  // too far off to animate sensibly: jump
        reanchor();
        viewUpdated();
    }

    void panBy(QPointF d) {
        camAnim.stop();
        cam.f.o += cpx(d);
        reanchor();
    }

    void zoomBy(double f, QPointF p) {
        camAnim.stop();
        const double s = std::abs(cam.f.z);
        if (s * f > CAM_MAX_SCALE) f = CAM_MAX_SCALE / s;
        if (const Node* r = model->root())
            if (const auto rf = screenFrame(r)) {
                const double rr = std::abs(rf->z) * lay.extent(r);
                if (rr * f < CAM_MIN_ROOT_PX) f = CAM_MIN_ROOT_PX / rr;
            }
        cam.f.o = cpx(p) + (cam.f.o - cpx(p)) * f;
        cam.f.z *= f;
        reanchor();
    }

    // Interpolate parent-relative frames; new nodes grow out of their parent's origin.
    void applyFrame(double t) {
        shownLoc.clear();
        shownLocById_.clear();
        for (auto it = lay.loc.cbegin(); it != lay.loc.cend(); ++it) {
            Local L = *it;
            if (t < 1 && it.key()->parent)
                L = lerp(fromLoc_.value(it.key()->id, Local{C(0, 0), L.z * 0.02, L.dir}), L, t);
            shownLoc[it.key()] = L;
            shownLocById_[it.key()->id] = L;
        }
        levelCamera();
        if (following && camAnim.state() != QAbstractAnimation::Running)
            if (const auto d = screenFrame(model->find(selId))) {
                if (havePrev_) {
                    cam.f.o += followPrev_ - d->o;
                    emit viewChanged();
                } else {
                    followPrev_ = d->o;
                    havePrev_ = true;
                }
            }
        update();
    }
    // The view never rotates: the anchor's on-screen angle must equal the sum
    // of the shown frame rotations from the anchor up to the root. A relayout
    // changes those rotations, so counter-rotate the camera about a pivot
    // (the selection if visible, else the view centre). Summing angles instead
    // of composing frames stays exact at any zoom depth.
    void levelCamera() {
        if (camAnim.state() == QAbstractAnimation::Running) return;  // flight targets are already level
        const Node* a = anchorNode();
        if (!a) return;
        double want = 0;
        for (const Node* x = a; x; x = x->parent) want += std::arg(sloc(x).z);
        const double err = std::remainder(want - std::arg(cam.f.z), 2 * PI);
        if (std::abs(err) < 1e-12) return;
        C pivot = centre();
        if (const auto s = screenFrame(model->find(selId)); s && vrect().contains(qpt(s->o))) pivot = s->o;
        const C rot = std::polar(1.0, err);
        cam.f = {pivot + rot * (cam.f.o - pivot), rot * cam.f.z};
    }
    Item makeItem(const Node* n, const Frame& f, int depth) const {
        const double s = std::abs(f.z);
        const bool leaf = Layout::hidesKids(n);
        const double r = disks() ? s : leaf ? std::max(3.0, s * STD_NODE) : std::max(3.5, s * STD_WIDTH * 0.65);
        // Standard: a leaf has no outgoing branch, so its size on screen is the branch drawn to it.
        double len = s;
        if (!disks() && leaf && n->parent)
            if (const Local L = sloc(n); std::abs(L.z) > 0) len = s / std::abs(L.z) * std::abs(L.o);
        return {n, qpt(f.o), r, s, std::arg(f.z) + sloc(n).dir, depth, len};
    }

    // Collect visible nodes and branches: the anchor's subtree, then each
    // ancestor with its other subtrees. Disks stop once a disc covers the whole
    // view (nothing outside it can be visible); standard climbs to the root.
    void computeItems() {
        items_.clear();
        branches_.clear();
        const Node* A = anchorNode();
        if (!A || !lay.loc.contains(A)) return;
        const double margin = disks() ? 0 : 100, minPx = disks() ? MIN_PX : 0.0;
        auto visible = [&](const Node* n, const Frame& f) {
            const double R = std::abs(f.z) * lay.extent(n);
            return R >= minPx && f.o.real() + R > -margin && f.o.real() - R < vw() + margin &&
                   f.o.imag() + R > -margin && f.o.imag() - R < vh() + margin;
        };
        // n plus all visible subtrees below it, except the one rooted at `skip`
        std::function<void(const Node*, const Frame&, int, const Node*)> down =
            [&](const Node* n, const Frame& f, int depth, const Node* skip) {
                if (!visible(n, f)) return;
                items_.push_back(makeItem(n, f, depth));
                if (Layout::hidesKids(n)) return;
                for (auto& k : n->kids)
                    if (k.get() != skip) {
                        const Frame cf = compose(f, sloc(k.get()));
                        branches_.push_back({k.get(), f, cf, depth});
                        down(k.get(), cf, depth + 1, nullptr);
                    }
            };
        int depth = A->depth();
        if (const auto pf = stemFrom(A, cam.f)) branches_.push_back({A, *pf, cam.f, depth - 1});
        down(A, cam.f, depth, nullptr);
        Frame d = cam.f;
        for (const Node* c = A; c->parent; c = c->parent, --depth) {
            if (disks() && covers(d.o, std::abs(d.z))) break;
            const Frame pd = uncompose(d, sloc(c));
            if (!finiteFrame(pd) || std::abs(pd.z) > CAM_MAX_UP) break;
            if (const auto ppf = stemFrom(c->parent, pd)) branches_.push_back({c->parent, *ppf, pd, depth - 2});
            down(c->parent, pd, depth - 1, c);
            d = pd;
        }
        std::stable_sort(items_.begin(), items_.end(), [](const Item& a, const Item& b) { return a.depth < b.depth; });
    }

    void updateHover(QPointF pos) {
        const Node* h = hitTest(pos);
        h ? setCursor(Qt::PointingHandCursor) : unsetCursor();
        if (const QString id = h ? h->id : QString(); id != hoverId) {
            hoverId = id;
            QToolTip::hideText();
            update();
        }
    }

    // Disks: labels, then the smallest disc under the cursor. Standard: a node
    // hit exactly, then labels, then a node within 5 px, then a branch (which
    // selects the clade it leads to).
    const Node* hitTest(QPointF p) const {
        auto label = [&]() -> const Node* {
            for (auto it = labelHits.rbegin(); it != labelHits.rend(); ++it)  // topmost first
                if (it->first.contains(p))
                    if (const Node* n = model->find(it->second)) return n;
            return nullptr;
        };
        const Node* best = nullptr;
        if (disks()) {
            if (const Node* n = label()) return n;
            double br = 1e300;
            for (const Item& it : items_)
                if (it.r < br && QLineF(it.s, p).length() <= it.r) { best = it.n; br = it.r; }
            return best;
        }
        double bd = 1e18;
        bool exact = false;
        for (const Item& it : items_)
            if (const double d = QLineF(it.s, p).length(); d <= it.r + 5 && d < bd) {
                best = it.n;
                bd = d;
                exact = d <= it.r;
            }
        if (exact) return best;
        if (const Node* n = label()) return n;
        if (best) return best;
        double bsd = 3;
        for (const HitSeg& s : hitSegs_)
            if (const double d = segDist(p, s.a, s.b) - s.hw; d < bsd) { bsd = d; best = s.n; }
        return best;
    }

    void paint(QPainter& p);
    std::vector<Seg> drawBranches(QPainter& p);
    void drawNodes(QPainter& p);
    void drawLabels(QPainter& p, const std::vector<Seg>& segs);
};

void Canvas::paint(QPainter& p) {
    p.setRenderHint(QPainter::Antialiasing);
    if (!transparent_) p.fillRect(vrect(), T.bg);
    labelHits.clear();
    hitSegs_.clear();
    if (!model->isOpen()) {
        items_.clear();
        branches_.clear();
        p.setPen(T.dim);
        p.drawText(vrect(), Qt::AlignCenter, "No file open — use File ▸ Open or File ▸ New");
        return;
    }
    computeItems();
    const auto segs = drawBranches(p); drawNodes(p); drawLabels(p, segs);
    if (selId.isEmpty() && !exporting_) {
        p.setFont(font());
        p.setPen(T.dim);
        p.drawText(10, 20, "Click a node to select · wheel zoom (Shift = fast) · drag pan · "
                           "double-click to edit · right-click for menu · arrows navigate · Ctrl+F search");
    }
}

// Standard only: tapered branches, culled to the view. Returns them as
// label obstacles.
std::vector<Canvas::Seg> Canvas::drawBranches(QPainter& p) {
    std::vector<Seg> segs;
    if (disks()) return segs;
    const QRectF view = vrect().adjusted(-60, -60, 60, 60);
    for (const Branch& br : branches_) {
        const QPointF a = qpt(br.pf.o), b = qpt(br.cf.o);
        const double ps = std::abs(br.pf.z);
        const double wa = std::max(1.0, ps * STD_WIDTH), wb = std::max(1.0, std::abs(br.cf.z) * STD_WIDTH);
        const double m = std::max(wa, wb);
        const QPointF ctrl = a + qpt(br.pf.z / ps) * (QLineF(a, b).length() * STD_CURVE);
        if (!view.intersects(QPolygonF({a, b, ctrl}).boundingRect().adjusted(-m, -m, m, m))) continue;
        const QColor col = inCut(br.child) ? T.cut.darker(140) : nodeColor(br.child, br.depth, 0.7);
        const auto mid = drawTaper(p, a, ctrl, b, wa, wb, col, view);
        const double n = double(mid.size() - 1);
        for (size_t i = 0; i + 1 < mid.size(); ++i) {
            // the taper is linear in t, so a piece is widest at one of its ends
            const double w = std::max(wa + (wb - wa) * (i / n), wa + (wb - wa) * ((i + 1) / n));
            hitSegs_.push_back({mid[i], mid[i + 1], w / 2, br.child});
            const double pad = w / 2 + 1;
            segs.push_back({mid[i], mid[i + 1], pad,
                            QRectF(mid[i], mid[i + 1]).normalized().adjusted(-pad, -pad, pad, pad),
                            br.child->parent, br.child});
        }
    }
    return segs;
}

// Nodes, parents first (items_ is sorted by depth), then selection / cut rings.
void Canvas::drawNodes(QPainter& p) {
    const bool dk = disks();
    for (const Item& v : items_) {
        const bool cut = inCut(v.n);
        const QColor fill = cut ? T.cut.darker(160) : nodeColor(v.n, v.depth, dk ? 0.55 : 1.0);
        if (covers(cpx(v.s), v.r)) { p.fillRect(vrect(), fill); continue; }  // fills the view: skip the huge ellipse
        p.setPen(QPen(cut ? T.cut : nodeColor(v.n, v.depth, dk ? 1.0 : 0.65), std::clamp(v.r * (dk ? 0.02 : 0.12), 1.0, 3.0)));
        p.setBrush(fill);
        p.drawEllipse(v.s, v.r, v.r);
    }
    p.setBrush(Qt::NoBrush);
    const Node *sel = model->find(selId), *cut = model->find(cutId);
    for (const Item& v : items_) {
        if (v.r > 1e5) continue;
        if (v.n == sel) { p.setPen(QPen(T.accent, 2)); p.drawEllipse(v.s, v.r + 4, v.r + 4); }
        if (v.n == cut) { p.setPen(QPen(T.cut, 2, Qt::DashLine)); p.drawEllipse(v.s, v.r + 8, v.r + 8); }
    }
}

// Greedy placement, selection first, then well-sized clades. Each label tries
// every wrap option at every candidate position (Over: around the node centre /
// disc top; Beside: rings around the node), cheapest first, and takes the first
// one that is not blocked. Hard obstacles: placed labels (+gap), other nodes,
// and branches not incident to the node. Its own branches are a soft cost.
// The selection is forced if nothing is free; the hovered label is drawn last.
void Canvas::drawLabels(QPainter& p, const std::vector<Seg>& segs) {
    const bool dk = disks(), beside = labelMode == LabelMode::Beside;
    const Node *sel = model->find(selId), *cut = model->find(cutId), *hov = model->find(hoverId);
    constexpr double padX = 5, padY = 2;
    static const double OFFS[] = {0, .45, -.45, .9, -.9, 1.4, -1.4, 2.0, -2.0, 2.5, -2.5, PI};
    const QRectF view = vrect(), inner = view.adjusted(4, 4, -4, -4);
    const QRectF nearView = view.adjusted(-120, -120, 120, 120);

    // Uniform grid over branch pieces. Every rect a strict test can see lies in
    // nearView, so clamping to it loses nothing.
    constexpr double CELL = 64;
    auto forCells = [&](const QRectF& r, auto&& f) {
        if (!r.intersects(nearView)) return;
        const QRectF c = r.intersected(nearView);
        for (int x = int(std::floor(c.left() / CELL)); x <= int(std::floor(c.right() / CELL)); ++x)
            for (int y = int(std::floor(c.top() / CELL)); y <= int(std::floor(c.bottom() / CELL)); ++y)
                f(qMakePair(x, y));
    };
    QHash<QPair<int, int>, std::vector<int>> grid;
    for (int i = 0; i < int(segs.size()); ++i) forCells(segs[i].box, [&](QPair<int, int> k) { grid[k].push_back(i); });
    std::vector<int> stamp(segs.size(), -1);
    int query = 0;
    // Does r touch any (padded) branch piece accepted by `want`? Exact clip test.
    auto anySeg = [&](const QRectF& r, auto&& want) {
        ++query;
        bool hit = false;
        forCells(r, [&](QPair<int, int> k) {
            if (hit) return;
            const auto it = grid.constFind(k);
            if (it == grid.cend()) return;
            for (int i : *it) {
                if (stamp[i] == query) continue;
                stamp[i] = query;
                const Seg& s = segs[i];
                if (!s.box.intersects(r) || !want(s)) continue;
                QPointF a = s.a, b = s.b;
                if (clipLine(r.adjusted(-s.pad, -s.pad, s.pad, s.pad), a, b)) { hit = true; return; }
            }
        });
        return hit;
    };

    std::vector<QRectF> nodeRects, placed;
    if (!dk)
        for (const Item& v : items_)
            nodeRects.push_back(QRectF(v.s.x() - v.r - 2, v.s.y() - v.r - 2, 2 * v.r + 4, 2 * v.r + 4));

    auto blocked = [&](const QRectF& r, int own) {
        const Node* me = items_[own].n;
        if (me != sel && !inner.contains(r)) return true;
        const QRectF t = r.adjusted(-LABEL_GAP, -LABEL_GAP, LABEL_GAP, LABEL_GAP);
        if (std::any_of(placed.begin(), placed.end(), [&](const QRectF& q) { return q.intersects(t); })) return true;
        for (size_t i = 0; i < nodeRects.size(); ++i)
            if (int(i) != own && nodeRects[i].intersects(r)) return true;
        return anySeg(r, [me](const Seg& s) { return s.p != me && s.c != me; });
    };
    auto onOwnStem = [&](const QRectF& r, const Node* me) {
        return anySeg(r, [me](const Seg& s) { return s.p == me || s.c == me; });
    };

    struct Lbl { QRectF box; QString text; QFont font; const Node* n; QPointF target; bool forced, leader; };
    auto makeLabel = [&](const Item& v, int own, bool force) -> std::optional<Lbl> {
        QStringList words = v.n->name.split(' ', Qt::SkipEmptyParts);
        if (words.isEmpty()) words << "?";
        if (v.n == cut) words[0] = "✂ " + words[0];
        if (v.n->collapsed && !v.n->kids.empty()) words << QString("[+%1]").arg(v.n->countDesc());

        QFont f = font();
        f.setPixelSize(std::clamp(int(dk ? v.lenPx * 0.11 : v.lenPx * 0.14), 11, 28));
        const QFontMetricsF fm(f);

        QPointF c = v.s;
        double rr = v.r;
        if (!beside) {
            if (dk && !Layout::hidesKids(v.n)) {
                c.ry() -= v.r * 0.82;
                rr = std::min(v.r * 0.1, 12.0) + 2;
                if (v.r > 200 && c.y() < 24 && v.s.y() + v.r > 80) {
                    c.setY(std::clamp(24.0, v.s.y() - v.r + 24.0, v.s.y()));
                    c.setX(std::clamp(c.x(), 80.0, vw() - 80.0));
                }
            }
            if (!force && !nearView.contains(c)) return std::nullopt;
        }

        const std::vector<QStringList> opts = wrapOptions(words, fm);
        struct Cand { double cost; QRectF r; int opt; bool leader; };
        std::vector<Cand> cs;
        for (int k = 0; k < int(opts.size()); ++k) {
            double tw = 0;
            for (const QString& line : opts[k]) tw = std::max(tw, fm.horizontalAdvance(line));
            const double w = std::ceil(tw) + 2 * padX;
            const double h = std::ceil(fm.height() + (opts[k].size() - 1) * fm.lineSpacing()) + 2 * padY;

            if (beside) {
                const int maxRings = (v.lenPx > 80 || force) ? 3 : 1;
                for (int ring = 0; ring < maxRings; ++ring) {
                    const double gap = v.r + 6 + ring * 22;
                    for (double off : OFFS) {
                        const double ux = std::cos(v.angle + off), uy = std::sin(v.angle + off);
                        cs.push_back({ring + std::abs(off) * 0.25 + WRAP_COST * k,
                                      anchoredRect(w, h, v.s.x() + ux * gap, v.s.y() + uy * gap, ux, uy), k, ring > 0});
                    }
                }
                continue;
            }

            if (dk && !force && v.lenPx < 140 && w > v.r * 1.85) continue;
            const double mx = w / 2 + rr - 1, my = h / 2 + rr - 1;
            const int steps = (v.lenPx > 90 || force) ? 3 : 2;
            for (int i = -steps; i <= steps; ++i)
                for (int j = -steps; j <= steps; ++j) {
                    const double dx = mx * i / 2.0, dy = my * j / 2.0;
                    const QRectF r(std::round(c.x() + dx - w / 2), std::round(c.y() + dy - h / 2), w, h);
                    const QPointF q(std::clamp(c.x(), r.left(), r.right()), std::clamp(c.y(), r.top(), r.bottom()));
                    const double dist = QLineF(q, c).length();
                    if (dist <= rr || (v.lenPx > 90 && dist <= rr + 36))
                        cs.push_back({std::abs(dx) / w + std::abs(dy) / h + WRAP_COST * k + (dist > rr ? 1.5 : 0),
                                      r, k, dist > rr + 4});
                }
        }
        if (cs.empty()) return std::nullopt;
        if (!segs.empty())
            for (Cand& cd : cs)
                if (onOwnStem(cd.r, v.n)) cd.cost += STEM_COST;
        std::stable_sort(cs.begin(), cs.end(), [](const Cand& a, const Cand& b) { return a.cost < b.cost; });
        for (const Cand& cd : cs)
            if (!blocked(cd.r, own)) return Lbl{cd.r, opts[cd.opt].join('\n'), f, v.n, c, false, cd.leader};
        if (!force) return std::nullopt;
        return Lbl{cs.front().r, opts[cs.front().opt].join('\n'), f, v.n, c, true, false};
    };

    const double minLen = dk ? LABEL_MIN_SCREEN_LEN : 24.0;
    std::vector<int> cands;
    for (int i = 0; i < int(items_.size()); ++i) {
        const Item& v = items_[i];
        if (v.n == sel || v.branchPx >= minLen) cands.push_back(i);
    }
    auto priority = [&](const Item& v) {
        const bool giantOffCenter = v.lenPx > std::max(vw(), vh()) * 1.2;
        return std::make_tuple(v.n != sel, giantOffCenter, -lay.weight(v.n), v.depth, -v.lenPx);
    };
    std::sort(cands.begin(), cands.end(), [&](int a, int b) { return priority(items_[a]) < priority(items_[b]); });

    std::vector<Lbl> labels;
    bool hovShown = false;
    for (int i : cands) {
        if (int(labels.size()) >= MAX_LABELS) break;
        if (auto l = makeLabel(items_[i], i, items_[i].n == sel)) {
            placed.push_back(l->box);
            labels.push_back(*l);
            hovShown |= l->n == hov;
        }
    }
    if (hov && !hovShown) {
        const auto it = std::find_if(items_.begin(), items_.end(), [&](const Item& v) { return v.n == hov; });
        if (it != items_.end())
            if (auto l = makeLabel(*it, int(it - items_.begin()), true)) labels.push_back(*l);
    }

    for (const Lbl& l : labels) {
        if (l.leader) {
            p.setPen(QPen(T.dim, 1.2, Qt::DotLine));
            p.drawLine(l.target, l.box.center());
        }
        const bool isSel = l.n == sel, isHov = l.n == hov;
        p.setPen(isSel ? QPen(T.accent, 1) : isHov ? QPen(T.dim, 1) : QPen(Qt::NoPen));
        p.setBrush(isHov || l.forced ? T.labelBgHover : T.labelBg);
        p.drawRoundedRect(l.box, 5, 5);
        p.setFont(l.font);
        p.setPen(isSel ? T.accent : inCut(l.n) ? T.cut : T.fg);
        p.drawText(l.box.adjusted(padX, padY, -padX, -padY), Qt::AlignCenter, l.text);
        labelHits.push_back({l.box, l.n->id});
    }
}

// --------------------------------------------------------- info panel ---
// Side panel: clade search plus details and actions for the selected clade.
class InfoPanel : public QWidget {
    Q_OBJECT
public:
    InfoPanel(const Model* m, const QList<QAction*>& acts) : model(m) {
        search->setPlaceholderText("Search clades by name…  (Ctrl+F)");
        search->setClearButtonEnabled(true);
        search->installEventFilter(this);
        results->setMaximumHeight(180);
        results->hide();
        QFont f = title->font();
        f.setPointSize(16);
        f.setBold(true);
        title->setFont(f);
        title->setWordWrap(true);
        crumbs->setWordWrap(true);
        crumbs->setTextFormat(Qt::RichText);
        meta->setWordWrap(true);
        meta->setTextInteractionFlags(Qt::TextSelectableByMouse);
        kids->setMaximumHeight(160);
        up->setFlat(true);
        info->setPlaceholderText("(no info — press E, double-click the node, or click 'Edit')");
        empty->setWordWrap(true);

        auto grid = new QGridLayout;
        for (int i = 0; i < acts.size(); ++i) {
            auto b = new QToolButton;
            b->setDefaultAction(acts[i]);
            b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            b->setMinimumHeight(30);
            grid->addWidget(b, i / 2, i % 2);
        }
        auto nv = new QVBoxLayout(nodeBox);
        nv->setContentsMargins(0, 6, 0, 0);
        nv->addWidget(crumbs);
        nv->addWidget(title);
        nv->addWidget(meta);
        nv->addLayout(grid);
        nv->addWidget(new QLabel("Subclades:"));
        nv->addWidget(kids);
        nv->addWidget(up);
        nv->addWidget(new QLabel("Info:"));
        nv->addWidget(info, 1);
        auto v = new QVBoxLayout(this);
        v->addWidget(search);
        v->addWidget(results);
        v->addWidget(nodeBox, 1);
        v->addWidget(empty, 1, Qt::AlignTop);

        connect(search, &QLineEdit::textChanged, this, [this] { runSearch(); });
        connect(results, &QListWidget::itemClicked, this, [this](QListWidgetItem* it) { choose(it); });
        connect(kids, &QListWidget::itemClicked, this,
                [this](QListWidgetItem* it) { emit selectRequested(it->data(Qt::UserRole).toString()); });
        connect(up, &QPushButton::clicked, this, [this] { emit selectRequested(upId); });
        connect(crumbs, &QLabel::linkActivated, this, [this](const QString& link) {
            if (link != "more") return emit selectRequested(link);
            crumbsOpen = shownId;
            if (const Node* n = model->find(shownId)) setCrumbs(n);
        });
        restyle();
        clearNode();
    }

    void restyle() {
        title->setStyleSheet(colorCss(T.accent));
        meta->setStyleSheet(colorCss(T.dim));
        empty->setStyleSheet(colorCss(T.dim));
        up->setStyleSheet("text-align:left; " + colorCss(T.link));
        if (const Node* n = model->find(shownId); n && nodeBox->isVisible()) setCrumbs(n);
    }

    void focusSearch() { search->setFocus(); search->selectAll(); }
    void clearNode() { nodeBox->hide(); empty->show(); }

    void setNode(const Node* n) {
        empty->hide();
        nodeBox->show();
        shownId = n->id;
        setCrumbs(n);
        title->setText(n->name);
        meta->setText(QString("Depth: %1\nParent: %2\nDirect subclades: %3\nTotal descendants: %4\nID: %5")
                          .arg(n->depth()).arg(n->parent ? n->parent->name : "— (root)")
                          .arg(n->kids.size()).arg(n->countDesc()).arg(n->id));
        kids->clear();
        for (auto& k : n->kids) (new QListWidgetItem("› " + k->name, kids))->setData(Qt::UserRole, k->id);
        upId = n->parent ? n->parent->id : QString();
        up->setVisible(n->parent);
        if (n->parent) up->setText("‹ up to " + n->parent->name);
        info->setPlainText(n->info);
    }

    // The clades above n, root first; a long trail folds its middle into a "…"
    // link that unfolds it.
    void setCrumbs(const Node* n) {
        std::vector<const Node*> path;
        for (const Node* a = n->parent; a; a = a->parent) path.insert(path.begin(), a);
        if (crumbsOpen != n->id) crumbsOpen.clear();
        const bool fold = crumbsOpen.isEmpty() && path.size() > 6;
        const QString link = QString("<a href=\"%1\" style=\"color:%2; text-decoration:none\">%3</a>");
        QStringList parts;
        for (size_t i = 0; i < path.size(); ++i) {
            if (fold && i == 1) {
                parts << link.arg("more", T.link.name(), "…");
                i = path.size() - 5;
                continue;
            }
            parts << link.arg(path[i]->id, T.link.name(), path[i]->name.toHtmlEscaped());
        }
        crumbs->setText(parts.join(QString(" <span style=\"%1\">›</span> ").arg(colorCss(T.dim))));
        crumbs->setVisible(!path.empty());
    }

    // Case-insensitive substring match, ranked exact > prefix > word start > anywhere,
    // then by name length and alphabetically.
    void runSearch() {
        results->clear();
        const QString q = search->text().trimmed();
        if (q.isEmpty() || !model->root()) return results->hide();
        struct Hit { int rank; const Node* n; };
        std::vector<Hit> hits;
        model->root()->walk([&](const Node* n) {
            const int i = n->name.indexOf(q, 0, Qt::CaseInsensitive);
            if (i < 0) return;
            hits.push_back({n->name.compare(q, Qt::CaseInsensitive) == 0 ? 0
                            : i == 0                                     ? 1
                            : !n->name[i - 1].isLetterOrNumber()         ? 2 : 3, n});
        });
        auto key = [](const Hit& h) { return std::make_tuple(h.rank, h.n->name.size(), h.n->name.toLower()); };
        std::stable_sort(hits.begin(), hits.end(), [&](const Hit& a, const Hit& b) { return key(a) < key(b); });

        auto addRow = [&](const QString& text, const QString& id) {  // rows without an id are inert
            auto it = new QListWidgetItem(text, results);
            if (id.isEmpty()) it->setFlags(Qt::NoItemFlags);
            else it->setData(Qt::UserRole, id);
        };
        if (hits.empty()) addRow("(no matches)", {});
        for (size_t i = 0; i < std::min(hits.size(), size_t(MAX_SEARCH)); ++i) {
            const Node* n = hits[i].n;
            addRow(n->name + (n->parent ? "   · in " + n->parent->name : QString()), n->id);
        }
        if (hits.size() > size_t(MAX_SEARCH))
            addRow(QString("… %1 more — refine the search").arg(hits.size() - MAX_SEARCH), {});
        if (!hits.empty()) results->setCurrentRow(0);
        results->show();
    }

signals:
    void selectRequested(const QString& id);
    void searchChosen(const QString& id);

protected:
    // Up/Down/Enter in the search box drive the result list; Esc clears it.
    // Claiming these as ShortcutOverride stops the window's single-key
    // shortcuts (navigation, deselect) from firing while typing.
    bool eventFilter(QObject* o, QEvent* e) override {
        if (o != search || (e->type() != QEvent::ShortcutOverride && e->type() != QEvent::KeyPress))
            return QWidget::eventFilter(o, e);
        const int key = static_cast<QKeyEvent*>(e)->key();
        const bool enter = key == Qt::Key_Return || key == Qt::Key_Enter;
        if (!(key == Qt::Key_Up || key == Qt::Key_Down || enter || (key == Qt::Key_Escape && !search->text().isEmpty())))
            return QWidget::eventFilter(o, e);
        e->accept();
        if (e->type() == QEvent::ShortcutOverride) return true;
        const int n = results->count();
        if (key == Qt::Key_Escape) search->clear();
        else if (key == Qt::Key_Down && n) results->setCurrentRow(std::min(n - 1, results->currentRow() + 1));
        else if (key == Qt::Key_Up && n) results->setCurrentRow(std::max(0, results->currentRow() - 1));
        else if (enter) choose(results->currentItem());
        return true;
    }

private:
    const Model* model;
    QLineEdit* search = new QLineEdit;
    QListWidget* results = new QListWidget;
    QWidget* nodeBox = new QWidget;
    QLabel* crumbs = new QLabel;
    QLabel* title = new QLabel;
    QLabel* meta = new QLabel;
    QLabel* empty = new QLabel("No clade selected — click a node, or search above.");
    QListWidget* kids = new QListWidget;
    QPushButton* up = new QPushButton;
    QTextBrowser* info = new QTextBrowser;
    QString upId, shownId, crumbsOpen;  // crumbsOpen: the clade whose trail is unfolded

    void choose(QListWidgetItem* it) {
        if (!it) return;
        if (const QString id = it->data(Qt::UserRole).toString(); !id.isEmpty()) emit searchChosen(id);
    }
};

// Lets a dialog handle keys in a child widget before the widget does
// (Tab, Enter, arrows). The handler returns true to consume the key.
class KeyHook : public QObject {
public:
    explicit KeyHook(std::function<bool(QKeyEvent*)> f) : f_(std::move(f)) {}
protected:
    bool eventFilter(QObject* o, QEvent* e) override {
        if (e->type() == QEvent::KeyPress && f_(static_cast<QKeyEvent*>(e))) return true;
        return QObject::eventFilter(o, e);
    }
private:
    std::function<bool(QKeyEvent*)> f_;
};

// -------------------------------------------------------- main window ---
static const char* FILE_FILTER = "Cladogram JSON (*.json);;All files (*)";

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow() {
        resize(1280, 800);
        setAcceptDrops(true);
        setCentralWidget(canvas);

        auto mk = [this](const QString& text, QList<QKeySequence> keys, auto fn) {
            auto a = new QAction(text, this);
            a->setShortcuts(keys);
            a->setShortcutContext(Qt::WindowShortcut);
            connect(a, &QAction::triggered, this, fn);
            addAction(a);  // shortcuts work even for actions not in any menu
            return a;
        };

        QMenu* fileMenu = menuBar()->addMenu("&File");
        fileMenu->addAction(mk("&New…", {QKeySequence::New}, [this] { fileNew(); }));
        fileMenu->addAction(mk("&Open…", {QKeySequence::Open}, [this] { fileOpen(); }));
        recentMenu = fileMenu->addMenu("Open &Recent");
        connect(recentMenu, &QMenu::aboutToShow, this, &MainWindow::rebuildRecentMenu);
        fileMenu->addAction(aSaveAs = mk("Save &As…", {QKeySequence("Ctrl+Shift+S")}, [this] { fileSaveAs(); }));
        fileMenu->addAction(aExport = mk("&Export image…", {QKeySequence("Ctrl+E")}, [this] { fileExport(); }));
        fileMenu->addSeparator();
        fileMenu->addAction(mk("&Quit", {QKeySequence::Quit}, [this] { close(); }));

        aAdd      = mk("Add child (A)", {Qt::Key_A, Qt::Key_D}, [this] { actAdd(); });
        aEdit     = mk("Edit (E)", {Qt::Key_E, Qt::Key_R}, [this] { actEdit(); });
        aCollapse = mk("Collapse (C)", {Qt::Key_C}, [this] { actCollapse(); });
        aColor    = mk("Colour… (K)", {Qt::Key_K}, [this] { actColor(); });
        aFocus    = mk("Focus (F)", {Qt::Key_F}, [this] { if (auto n = sel()) canvas->focusOn(n); });
        aDelete   = mk("Delete (Del)", {Qt::Key_Delete, Qt::Key_Backspace}, [this] { actDelete(); });
        aCut      = mk("Cut subtree (X)", {Qt::Key_X, QKeySequence::Cut}, [this] { actCut(); });
        aPaste    = mk("Paste here (V)", {Qt::Key_V, QKeySequence::Paste}, [this] { actPaste(); });
        QAction* aFind = mk("Find clade…", {QKeySequence::Find}, [this] { panel->focusSearch(); });
        mk("Escape", {Qt::Key_Escape}, [this] {
            if (!canvas->cutId.isEmpty()) { canvas->cutId.clear(); model.status = "Cut cancelled"; }
            else canvas->selId.clear();
            refresh();
        });
        // Spread / shrink / curl only affect the standard layout, so only it is recomputed.
        auto layoutKey = [this, &mk](QList<QKeySequence> keys, auto fn) {
            mk("", keys, [this, fn] {
                fn(canvas->lay);
                if (!canvas->disks()) canvas->relayout();
                updateHud();
            });
        };
        layoutKey({Qt::Key_Plus, Qt::Key_Equal}, [](Layout& l) { l.spread = std::min(340 * DEG, l.spread + 10 * DEG); });
        layoutKey({Qt::Key_Minus}, [](Layout& l) { l.spread = std::max(20 * DEG, l.spread - 10 * DEG); });
        layoutKey({Qt::Key_BracketLeft}, [](Layout& l) { l.shrink = std::max(0.4, l.shrink - 0.02); });
        layoutKey({Qt::Key_BracketRight}, [](Layout& l) { l.shrink = std::min(0.9, l.shrink + 0.02); });
        // curl: ',' = less (toward -1, counter-clockwise), '.' = more (toward +1, clockwise)
        layoutKey({Qt::Key_Comma}, [](Layout& l) { l.curl = std::max(-1.0, l.curl - 0.05); });
        layoutKey({Qt::Key_Period}, [](Layout& l) { l.curl = std::min(1.0, l.curl + 0.05); });

        QMenu* editMenu = menuBar()->addMenu("&Edit");
        editMenu->addAction(mk("&Undo", {QKeySequence::Undo}, [this] { undoRedo(model.undo()); }));
        editMenu->addAction(mk("&Redo", {QKeySequence::Redo, QKeySequence("Ctrl+Shift+Z")}, [this] { undoRedo(model.redo()); }));
        editMenu->addSeparator();
        editMenu->addAction(aPalette = mk("Command &palette…", {QKeySequence("Ctrl+K")}, [this] { openPalette(); }));

        QMenu* cladeMenu = menuBar()->addMenu("&Clade");
        fillCladeMenu(cladeMenu, aFind);
        cladeMenu->addSeparator();
        QMenu* goMenu = cladeMenu->addMenu("&Go to");
        struct NavKey { const char* text; Qt::Key key; Nav dir; };
        for (const NavKey& k : {NavKey{"Heaviest daughter", Qt::Key_Up, Nav::Daughter},
                                NavKey{"Sister to the right", Qt::Key_Right, Nav::Right},
                                NavKey{"Sister to the left", Qt::Key_Left, Nav::Left},
                                NavKey{"Mother", Qt::Key_Down, Nav::Mother}})
            goMenu->addAction(mk(k.text, {k.key}, [this, d = k.dir] { navigate(d); }));

        QMenu* viewMenu = menuBar()->addMenu("&View");
        auto radio = [viewMenu](QActionGroup* g, QAction* a) {
            a->setCheckable(true);
            g->addAction(a);
            viewMenu->addAction(a);
            return a;
        };
        viewMenu->addSection("Style");
        auto styleGroup = new QActionGroup(this);
        for (Layout::Style st : {Layout::Standard, Layout::Disks})
            styleActs.push_back(radio(styleGroup, mk(Layout::styleName(st), {QKeySequence(QString("Ctrl+%1").arg(st + 1))},
                                                     [this, st] { setStyle(st); })));
        viewMenu->addSection("Labels");
        auto labelGroup = new QActionGroup(this);
        aLabelsOver = radio(labelGroup, mk("Labels over nodes", {}, [this] { setLabelsBeside(false); }));
        aLabelsBeside = radio(labelGroup, mk("Labels beside nodes", {}, [this] { setLabelsBeside(true); }));
        viewMenu->addAction(mk("Toggle label placement", {Qt::Key_L},
                               [this] { setLabelsBeside(canvas->labelMode == Canvas::LabelMode::Over); }));
        viewMenu->addSection("Theme");
        viewMenu->addAction(aDark = mk("Dark mode", {QKeySequence("Ctrl+Shift+D")}, [this](bool on) { applyTheme(on); }));
        aDark->setCheckable(true);
        viewMenu->addSeparator();
        viewMenu->addAction(mk("Fit view", {Qt::Key_Home}, [this] { canvas->fitView(); }));

        panel = new InfoPanel(&model, {aAdd, aEdit, aCollapse, aColor, aFocus, aCut, aPaste, aDelete});
        auto dock = new QDockWidget("Clade", this);
        dock->setWidget(panel);
        dock->setFeatures(QDockWidget::NoDockWidgetFeatures);
        dock->setMinimumWidth(340);
        addDockWidget(Qt::RightDockWidgetArea, dock);
        statusBar()->addWidget(hud, 1);

        connect(&model, &Model::treeChanged, this, [this] { canvas->relayout(); panel->runSearch(); refresh(); });
        connect(&model, &Model::statusChanged, this, &MainWindow::updateHud);
        connect(canvas, &Canvas::viewChanged, this, &MainWindow::updateHud);
        connect(canvas, &Canvas::clicked, this, &MainWindow::select);
        connect(panel, &InfoPanel::selectRequested, this, &MainWindow::select);
        connect(panel, &InfoPanel::searchChosen, this, &MainWindow::jumpTo);
        connect(canvas, &Canvas::doubleClicked, this, [this](const QString& id) { select(id); actEdit(); });
        connect(canvas, &Canvas::contextMenu, this, [this](const QString& id, const QPoint& g) {
            if (!id.isEmpty()) select(id);
            if (!sel()) return;
            QMenu m;
            fillCladeMenu(&m);
            m.exec(g);
        });

        QSettings s;  // restore view preferences
        const int st = std::clamp(s.value("view/style", int(Layout::Standard)).toInt(), 0, 1);
        canvas->lay.style = Layout::Style(st);
        styleActs[st]->setChecked(true);
        setLabelsBeside(s.value("view/labelsBeside", false).toBool());
        applyTheme(s.value("view/dark", true).toBool());
        canvas->relayout(false);
        refresh();
    }

    bool openPath(const QString& p) { return switched(model.open(p), "Cannot open file"); }

    // Open p if it exists, otherwise create it containing the sample tree.
    bool openOrCreate(const QString& p) {
        return QFile::exists(p) ? openPath(p) : switched(model.create(p, sampleTree()), "Cannot create file");
    }

    // Returns whether a file is open afterwards.
    bool startupPrompt() {
        QMessageBox box(this);
        box.setWindowTitle("TreeFern");
        box.setText("Open an existing cladogram or create a new one?");
        auto bOpen = box.addButton("Open…", QMessageBox::AcceptRole);
        auto bNew = box.addButton("New…", QMessageBox::AcceptRole);
        auto bSample = box.addButton("New from sample…", QMessageBox::AcceptRole);
        box.addButton(QMessageBox::Cancel);
        box.exec();
        if (box.clickedButton() == bOpen) fileOpen();
        else if (box.clickedButton() == bNew) fileNew();
        else if (box.clickedButton() == bSample) fileNew(true);
        return model.isOpen();
    }

protected:
    void dragEnterEvent(QDragEnterEvent* e) override {
        if (e->mimeData()->hasUrls()) e->acceptProposedAction();
    }
    void dropEvent(QDropEvent* e) override {
        const auto urls = e->mimeData()->urls();
        if (!urls.isEmpty() && urls.first().isLocalFile()) openPath(urls.first().toLocalFile());
    }

private:
    enum class Nav { Daughter, Right, Left, Mother };

    Model model;
    Canvas* canvas = new Canvas(&model);
    InfoPanel* panel = nullptr;
    QLabel* hud = new QLabel;
    QMenu* recentMenu = nullptr;
    QAction *aAdd, *aEdit, *aCollapse, *aColor, *aFocus, *aDelete, *aCut, *aPaste, *aSaveAs, *aExport;
    QAction *aLabelsOver, *aLabelsBeside, *aDark, *aPalette = nullptr;
    std::vector<QAction*> styleActs;  // indexed by Layout::Style

    Node* sel() const { return model.find(canvas->selId); }
    void select(const QString& id) { canvas->selId = id; refresh(); }

    void fillCladeMenu(QMenu* m, QAction* extra = nullptr) {
        m->addActions({aAdd, aEdit, aCollapse, aColor, aFocus});
        if (extra) m->addAction(extra);
        m->addSeparator();
        m->addActions({aCut, aPaste});
        m->addSeparator();
        m->addAction(aDelete);
    }

    // Expand collapsed ancestors so the node is laid out, then select it and fly there.
    void jumpTo(const QString& id) {
        Node* n = model.find(id);
        if (!n) return;
        bool expanded = false;
        for (Node* a = n->parent; a; a = a->parent)
            if (a->collapsed) { a->collapsed = false; expanded = true; }
        canvas->selId = id;
        if (expanded) model.commit();
        if ((n = model.find(id))) canvas->focusOn(n);
        refresh();
        canvas->setFocus();
    }

    // ---- view settings
    void setStyle(Layout::Style st) {
        canvas->lay.style = st;
        styleActs[st]->setChecked(true);
        canvas->relayout(false);  // frames mean different things per style: don't morph
        canvas->fitView();
        QSettings().setValue("view/style", int(st));
        updateHud();
    }

    void setLabelsBeside(bool beside) {
        canvas->labelMode = beside ? Canvas::LabelMode::Beside : Canvas::LabelMode::Over;
        (beside ? aLabelsBeside : aLabelsOver)->setChecked(true);
        QSettings().setValue("view/labelsBeside", beside);
        canvas->update();
    }

    void applyTheme(bool dark) {
        T = makeTheme(dark);
        applyPalette(T);
        panel->restyle();
        aDark->setChecked(dark);
        QSettings().setValue("view/dark", dark);
        canvas->update();
    }

    // ---- navigation, in on-screen order (Layout::ordered)
    void goTo(const Node* n) {
        canvas->selId = n->id;
        canvas->ensureVisible(n);
        refresh();
    }

    void navigate(Nav d) {
        Node* n = sel();
        if (!n) {
            if (Node* r = model.root()) goTo(r);
            return;
        }
        const Node* target = nullptr;
        if (d == Nav::Mother) {
            if (!(target = n->parent)) model.status = "Already at the root clade";
        } else if (d == Nav::Daughter) {
            if (n->kids.empty()) model.status = QString("'%1' has no daughters").arg(n->name);
            else if (n->collapsed) model.status = QString("'%1' is collapsed — press C to expand").arg(n->name);
            else target = canvas->lay.ordered(n).back();
        } else if (!n->parent) {
            model.status = "The root clade has no sisters";
        } else if (const auto sis = canvas->lay.ordered(n->parent); sis.size() < 2) {
            model.status = QString("'%1' has no sisters").arg(n->name);
        } else {  // wraps around at either end
            const int count = int(sis.size()), i = int(std::find(sis.begin(), sis.end(), n) - sis.begin());
            target = sis[(i + (d == Nav::Right ? 1 : -1) + count) % count];
        }
        if (target) goTo(target);
        else refresh();
    }

    // ---- file handling
    // Show `err` if there is one; otherwise finish switching to the model's new file.
    bool switched(const QString& err, const char* title, bool resetView = true) {
        if (!err.isEmpty()) {
            QMessageBox::warning(this, title, err);
            return false;
        }
        if (resetView) {
            canvas->selId.clear();
            canvas->cutId.clear();
        }
        setWindowTitle("TreeFern — " + model.path);
        addRecent(model.path);
        canvas->relayout(!resetView);
        if (resetView) canvas->fitView();
        panel->runSearch();
        refresh();
        return true;
    }

    QString startDir() const {
        return model.path.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
                                    : QFileInfo(model.path).absolutePath();
    }

    QString askSavePath(const char* title, const QString& start) {
        QString p = QFileDialog::getSaveFileName(this, title, start, FILE_FILTER);
        if (!p.isEmpty() && QFileInfo(p).suffix().isEmpty()) p += ".json";
        return p;
    }

    void fileOpen() {
        const QString p = QFileDialog::getOpenFileName(this, "Open cladogram", startDir(), FILE_FILTER);
        if (!p.isEmpty()) openPath(p);
    }

    void fileNew(bool sample = false) {
        const QString p = askSavePath("New cladogram", startDir() + "/cladogram.json");
        if (p.isEmpty()) return;
        std::unique_ptr<Node> tree = sample ? sampleTree() : nullptr;
        if (!tree) {
            bool ok;
            const QString name = QInputDialog::getText(this, "New cladogram", "Name of the root clade:",
                                                       QLineEdit::Normal, "Root", &ok).trimmed();
            if (!ok) return;
            tree = Node::make(name.isEmpty() ? "Root" : name);
        }
        switched(model.create(p, std::move(tree)), "Cannot create file");
    }

    // Export the view or the whole tree as PNG (saved or copied) or SVG. Image
    // sizes are capped by Canvas::exportSize; SVGs too large to open are refused.
    void fileExport() {
        if (!model.isOpen()) return;
        QSettings st;
        QDialog dlg(this);
        dlg.setWindowTitle("Export image");
        auto group = [&](const char* title, QStringList labels, int checked) {
            auto box = new QGroupBox(title);
            auto row = new QHBoxLayout(box);
            auto g = new QButtonGroup(box);
            for (int i = 0; i < labels.size(); ++i) {
                auto b = new QRadioButton(labels[i]);
                g->addButton(b, i);
                row->addWidget(b);
            }
            row->addStretch();
            g->button(std::clamp(checked, 0, int(labels.size()) - 1))->setChecked(true);
            return std::pair(box, g);
        };
        const auto [areaBox, area] = group("Area", {"Current view", "Whole tree"}, st.value("export/whole", false).toBool());
        const auto [formatBox, format] = group("Format", {"PNG", "SVG"}, st.value("export/svg", false).toBool());
        const auto [scaleBox, scale] = group("Resolution", {"1x", "2x", "4x"}, st.value("export/scale", 1).toInt());
        auto transparent = new QCheckBox("Transparent background");
        transparent->setChecked(st.value("export/transparent", false).toBool());
        auto info = new QLabel;
        info->setWordWrap(true);
        info->setStyleSheet(colorCss(T.dim));
        auto buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
        QPushButton* save = buttons->addButton("Save…", QDialogButtonBox::AcceptRole);
        QPushButton* copy = buttons->addButton("Copy to clipboard", QDialogButtonBox::ActionRole);
        save->setDefault(true);
        auto v = new QVBoxLayout(&dlg);
        for (QWidget* w : std::initializer_list<QWidget*>{areaBox, formatBox, scaleBox, transparent, info, buttons}) v->addWidget(w);

        const double factors[] = {1, 2, 4};
        auto showInfo = [&] {
            const bool whole = area->checkedId() == 1, svg = format->checkedId() == 1;
            const int n = canvas->exportCount(whole);
            scaleBox->setEnabled(!svg);
            copy->setEnabled(!svg);
            QString text;
            bool ok = true;
            if (svg && n > MAX_SVG_CLADES) {
                text = QString("This would put %L1 clades in one SVG; most programs cannot open SVGs that large (the limit "
                               "is %L2). Collapse some clades, zoom in, or export a PNG.").arg(n).arg(MAX_SVG_CLADES);
                ok = false;
            } else if (svg) {
                const auto e = canvas->exportSize(whole, 1);
                text = QString("%1 × %2, %L3 clades.").arg(e.w).arg(e.h).arg(n);
            } else {
                const double want = factors[scale->checkedId()];
                const auto e = canvas->exportSize(whole, want);
                text = QString("%L1 × %L2 pixels%3").arg(e.pw).arg(e.ph)
                           .arg(e.factor < want - 1e-9 ? ", reduced to keep the image within memory." : ".");
                if (n > SLOW_EXPORT_CLADES) text += QString(" Drawing %L1 clades may take a few seconds.").arg(n);
            }
            info->setText(text);
            save->setEnabled(ok);
        };
        for (QButtonGroup* g : {area, format, scale}) connect(g, &QButtonGroup::idClicked, &dlg, showInfo);
        connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
        bool toClipboard = false;
        connect(copy, &QPushButton::clicked, &dlg, [&] { toClipboard = true; dlg.accept(); });
        showInfo();
        if (dlg.exec() != QDialog::Accepted || !model.isOpen()) return;

        const bool whole = area->checkedId() == 1, svg = format->checkedId() == 1 && !toClipboard;
        const double factor = factors[scale->checkedId()];
        st.setValue("export/whole", whole);
        st.setValue("export/svg", format->checkedId() == 1);
        st.setValue("export/scale", scale->checkedId());
        st.setValue("export/transparent", transparent->isChecked());

        const QString base = QFileInfo(model.path).completeBaseName();
        QString p;
        if (!toClipboard) {
            p = QFileDialog::getSaveFileName(this, "Export image", startDir() + "/" + base + (svg ? ".svg" : ".png"),
                                             svg ? "SVG image (*.svg)" : "PNG image (*.png)");
            if (p.isEmpty()) return;
            if (QFileInfo(p).suffix().isEmpty()) p += svg ? ".svg" : ".png";
        }
        QApplication::setOverrideCursor(Qt::WaitCursor);
        bool ok = false;
        if (svg) {
            const auto e = canvas->exportSize(whole, 1);
            QSvgGenerator gen;
            gen.setFileName(p);
            gen.setSize(QSize(e.w, e.h));
            gen.setViewBox(QRect(0, 0, e.w, e.h));
            gen.setTitle(base);
            QPainter painter;
            if ((ok = painter.begin(&gen))) canvas->renderTo(painter, e.w, e.h, whole, transparent->isChecked());
            ok = painter.end() && ok;
        } else {
            const auto e = canvas->exportSize(whole, factor);
            QImage img(e.pw, e.ph, QImage::Format_ARGB32_Premultiplied);
            if (!img.isNull()) {
                img.fill(Qt::transparent);
                img.setDevicePixelRatio(e.factor);
                QPainter painter(&img);
                canvas->renderTo(painter, e.w, e.h, whole, transparent->isChecked());
                painter.end();
                if (toClipboard) QGuiApplication::clipboard()->setImage(img);
                ok = toClipboard || img.save(p);
            }
        }
        QApplication::restoreOverrideCursor();
        model.status = !ok ? (p.isEmpty() ? QString("Cannot make the image") : "Cannot write " + p)
                     : toClipboard ? QString("Copied the image to the clipboard") : "Exported " + p;
        updateHud();
    }

    void fileSaveAs() {
        if (!model.isOpen()) return;
        const QString p = askSavePath("Save cladogram as", model.path);
        if (!p.isEmpty()) switched(model.saveAs(p), "Cannot save file", false);
    }

    void addRecent(const QString& p) {
        QSettings s;
        QStringList r = s.value("recent").toStringList();
        r.removeAll(p);
        r.prepend(p);
        s.setValue("recent", r.mid(0, MAX_RECENT));
        s.setValue("lastFile", p);
    }

    void rebuildRecentMenu() {
        recentMenu->clear();
        QStringList r = QSettings().value("recent").toStringList();
        r.erase(std::remove_if(r.begin(), r.end(), [](const QString& p) { return !QFile::exists(p); }), r.end());
        if (r.isEmpty()) {
            recentMenu->addAction("(none)")->setEnabled(false);
            return;
        }
        for (const QString& p : r) recentMenu->addAction(p, this, [this, p] { openPath(p); });
        recentMenu->addSeparator();
        recentMenu->addAction("Clear list", this, [] { QSettings().remove("recent"); });
    }

    // ---- UI state: drop stale ids, then sync panel, actions and HUD
    void refresh() {
        if (!model.find(canvas->cutId)) canvas->cutId.clear();
        Node* s = sel();
        if (!s) canvas->selId.clear();
        Node* cut = model.find(canvas->cutId);
        s ? panel->setNode(s) : panel->clearNode();
        for (auto a : {aAdd, aEdit, aColor, aFocus}) a->setEnabled(s);
        aCollapse->setEnabled(s && !s->kids.empty());
        aCollapse->setText(s && s->collapsed ? "Expand (C)" : "Collapse (C)");
        aDelete->setEnabled(s && s->parent);
        aCut->setEnabled(s && s->parent);
        aPaste->setEnabled(s && cut && !cut->contains(s));
        aSaveAs->setEnabled(model.isOpen());
        aExport->setEnabled(model.isOpen());
        canvas->update();
        updateHud();
    }

    void updateHud() {
        QString cut, params;
        if (Node* c = model.find(canvas->cutId)) cut = QString("   ✂ cut: %1 (select target, V)").arg(c->name);
        if (!canvas->disks())
            params = QString("   spread %1°   shrink %2   curl %3")
                         .arg(qRound(canvas->lay.spread / DEG))
                         .arg(canvas->lay.shrink, 0, 'f', 2)
                         .arg(canvas->lay.curl, 0, 'f', 2);
        hud->setText(QString("%1   zoom %2%3   nodes %4%5   |   %6")
                         .arg(Layout::styleName(canvas->lay.style), canvas->zoomText(), params)
                         .arg(canvas->lay.count())
                         .arg(cut, model.status));
    }

    void undoRedo(const QString& err) {
        if (err.isEmpty()) return;  // treeChanged already refreshed
        model.status = err;
        updateHud();
    }

    // ---- command palette: every menu command, found by typing any part of its name
    void openPalette() {
        rebuildRecentMenu();
        struct Cmd { QAction* a; QString label, group; };
        std::vector<Cmd> cmds;
        QSet<QAction*> seen;
        std::function<void(QMenu*, const QString&)> collect = [&](QMenu* m, const QString& group) {
            QString sub = group;
            for (QAction* a : m->actions()) {
                if (a->isSeparator()) {
                    if (!a->text().isEmpty()) sub = group + " › " + a->text();  // a section
                    continue;
                }
                const QString text = QString(a->text()).remove('&');
                if (a->menu()) collect(a->menu(), group + " › " + text);
                else if (!text.isEmpty() && a != aPalette && !seen.contains(a)) {
                    seen.insert(a);
                    cmds.push_back({a, text, sub});
                }
            }
        };
        for (QAction* top : menuBar()->actions())
            if (top->menu()) collect(top->menu(), QString(top->text()).remove('&'));

        QDialog dlg(this);
        dlg.setWindowTitle("Command palette");
        dlg.resize(560, 420);
        auto input = new QLineEdit;
        input->setPlaceholderText("Type a command…");
        auto list = new QTreeWidget;
        list->setColumnCount(3);
        list->setHeaderHidden(true);
        list->setRootIsDecorated(false);
        list->setFocusPolicy(Qt::NoFocus);
        auto v = new QVBoxLayout(&dlg);
        v->addWidget(input);
        v->addWidget(list, 1);

        QAction* chosen = nullptr;
        auto fill = [&] {
            list->clear();
            const QStringList words = input->text().trimmed().toLower().split(' ', Qt::SkipEmptyParts);
            const QString q = words.join(' ');
            struct Hit { std::tuple<bool, int, size_t> key; const Cmd* c; };
            std::vector<Hit> hits;
            for (size_t i = 0; i < cmds.size(); ++i) {
                const QString label = cmds[i].label.toLower(), hay = label + ' ' + cmds[i].group.toLower();
                if (!std::all_of(words.begin(), words.end(), [&](const QString& w) { return hay.contains(w); })) continue;
                const int at = q.isEmpty() ? 0 : int(label.indexOf(q));
                const int rank = at == 0 ? 0 : at > 0 && !label[at - 1].isLetterOrNumber() ? 1 : at > 0 ? 2 : 3;
                hits.push_back({{!cmds[i].a->isEnabled(), rank, i}, &cmds[i]});
            }
            std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.key < b.key; });
            for (const Hit& h : hits) {
                auto it = new QTreeWidgetItem(list, {(h.c->a->isChecked() ? "✓ " : "") + h.c->label, h.c->group,
                                                     h.c->a->shortcut().toString(QKeySequence::NativeText)});
                it->setData(0, Qt::UserRole, QVariant::fromValue(static_cast<QObject*>(h.c->a)));
                it->setForeground(1, T.dim);
                it->setForeground(2, T.dim);
                if (!h.c->a->isEnabled()) it->setDisabled(true);
            }
            for (int c = 0; c < 3; ++c) list->resizeColumnToContents(c);
            for (int i = 0; i < list->topLevelItemCount(); ++i)
                if (!list->topLevelItem(i)->isDisabled()) { list->setCurrentItem(list->topLevelItem(i)); break; }
        };
        auto choose = [&](QTreeWidgetItem* it) {
            if (!it || it->isDisabled()) return;
            chosen = qobject_cast<QAction*>(it->data(0, Qt::UserRole).value<QObject*>());
            dlg.accept();
        };
        // Up/Down move through the list while typing; Enter runs the command.
        KeyHook hook([&](QKeyEvent* e) {
            const int k = e->key();
            if (k == Qt::Key_Return || k == Qt::Key_Enter) { choose(list->currentItem()); return true; }
            if (k != Qt::Key_Up && k != Qt::Key_Down) return false;
            QTreeWidgetItem* it = list->currentItem();
            do it = it ? (k == Qt::Key_Down ? list->itemBelow(it) : list->itemAbove(it)) : list->topLevelItem(0);
            while (it && it->isDisabled());
            if (it) list->setCurrentItem(it);
            return true;
        });
        input->installEventFilter(&hook);
        connect(input, &QLineEdit::textChanged, &dlg, fill);
        connect(list, &QTreeWidget::itemActivated, &dlg, [&](QTreeWidgetItem* it) { choose(it); });
        connect(list, &QTreeWidget::itemClicked, &dlg, [&](QTreeWidgetItem* it) { choose(it); });
        fill();
        input->setFocus();
        if (dlg.exec() == QDialog::Accepted && chosen) chosen->trigger();
    }

    // ---- clade actions
    // Dialogs run a nested event loop during which an external reload may
    // replace the tree, so nodes are re-looked-up by id afterwards.

    enum class Next { Done, Daughter, Sister };
    struct NewClade { QString name, info; Next next; };

    // Name (and info) of one new clade. Enter in the name: add it, then a
    // daughter of it; Tab: add it, then a sister; Ctrl+Enter or Add: add and stop.
    // An empty name, Esc or Cancel stops.
    std::optional<NewClade> askNewClade(const QString& parentName) {
        QDialog dlg(this);
        dlg.setWindowTitle("New clade");
        dlg.resize(560, 380);
        auto nameEdit = new QLineEdit;
        auto infoEdit = new QPlainTextEdit;
        infoEdit->setPlaceholderText("(no info)");
        auto hint = new QLabel("Enter: add it, then a daughter of it   ·   Tab: add it, then a sister   ·   "
                               "Ctrl+Enter or Add: add it and stop   ·   Esc: stop");
        hint->setWordWrap(true);
        hint->setStyleSheet(colorCss(T.dim));
        auto buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
        QPushButton* sister = buttons->addButton("Add, then a sister", QDialogButtonBox::ActionRole);
        QPushButton* daughter = buttons->addButton("Add, then a daughter", QDialogButtonBox::ActionRole);
        QPushButton* add = buttons->addButton("Add", QDialogButtonBox::AcceptRole);
        for (QPushButton* b : {sister, daughter, add}) b->setAutoDefault(false);
        auto form = new QVBoxLayout(&dlg);
        form->addWidget(new QLabel(QString("New clade under '%1':").arg(parentName)));
        form->addWidget(nameEdit);
        form->addSpacing(6);
        form->addWidget(new QLabel("Info:"));
        form->addWidget(infoEdit, 1);
        form->addWidget(hint);
        form->addWidget(buttons);

        Next next = Next::Done;
        auto finish = [&](Next n) {
            next = n;
            nameEdit->text().trimmed().isEmpty() ? dlg.reject() : dlg.accept();
        };
        KeyHook hook([&](QKeyEvent* e) {
            if (e->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) return false;
            if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) { finish(Next::Daughter); return true; }
            if (e->key() == Qt::Key_Tab) { finish(Next::Sister); return true; }
            return false;
        });
        nameEdit->installEventFilter(&hook);
        connect(sister, &QPushButton::clicked, &dlg, [&] { finish(Next::Sister); });
        connect(daughter, &QPushButton::clicked, &dlg, [&] { finish(Next::Daughter); });
        connect(add, &QPushButton::clicked, &dlg, [&] { finish(Next::Done); });
        connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
        for (auto seq : {QKeySequence("Ctrl+Return"), QKeySequence("Ctrl+Enter")})
            connect(new QShortcut(seq, &dlg), &QShortcut::activated, &dlg, [&] { finish(Next::Done); });
        nameEdit->setFocus();
        if (dlg.exec() != QDialog::Accepted) return std::nullopt;
        return NewClade{nameEdit->text().trimmed(), infoEdit->toPlainText(), next};
    }

    // Adds clades until the user stops; each is its own undo step.
    void actAdd() {
        Node* n = sel();
        if (!n) return;
        QString parentId = n->id;
        for (;;) {
            Node* p = model.find(parentId);
            if (!p) return;
            const auto r = askNewClade(p->name);
            if (!r || !(p = model.find(parentId))) return;
            Node* c = p->addChild(Node::make(r->name, r->info));
            p->collapsed = false;
            canvas->selId = c->id;
            model.commit();
            if (r->next == Next::Done) return;
            if (r->next == Next::Daughter) parentId = c->id;
        }
    }

    // A swatch menu by the cursor (over the tree) or the panel's Colour button.
    void actColor() {
        Node* n = sel();
        if (!n) return;
        const QString id = n->id;
        auto set = [this, id](const QString& c) {
            Node* m = model.find(id);
            if (!m || m->color == c) return;
            m->color = c;
            model.status = c.isEmpty() ? QString("'%1' uses %2 colours").arg(m->name, m->parent ? "its mother clade's" : "the default")
                                       : QString("Coloured '%1'").arg(m->name);
            model.commit();
        };
        static const std::pair<const char*, const char*> SWATCHES[] = {
            {"Red", "#e5534b"}, {"Orange", "#e8913a"}, {"Yellow", "#d9c13b"}, {"Green", "#57ab5a"}, {"Teal", "#39b5a5"},
            {"Blue", "#4f8fe0"}, {"Purple", "#9a6fe0"}, {"Pink", "#e06fb4"}, {"Brown", "#a0704a"}, {"Grey", "#8a8f99"}};
        QMenu m;
        for (const auto& [name, hex] : SWATCHES) {
            QPixmap px(14, 14);
            px.fill(Qt::transparent);
            QPainter pp(&px);
            pp.setRenderHint(QPainter::Antialiasing);
            pp.setPen(T.dim);
            pp.setBrush(QColor(hex));
            pp.drawEllipse(QRectF(0.5, 0.5, 13, 13));
            pp.end();
            QAction* a = m.addAction(QIcon(px), name, this, [set, h = QString(hex)] { set(h); });
            a->setCheckable(true);
            a->setChecked(n->color == hex);
        }
        m.addSeparator();
        m.addAction("Custom colour…", this, [this, set, id] {
            const Node* cur = model.find(id);
            const QColor c = QColorDialog::getColor(QColor(cur && !cur->color.isEmpty() ? cur->color : "#4f8fe0"), this, "Clade colour");
            if (c.isValid()) set(c.name());
        });
        QAction* inherit = m.addAction(n->parent ? "Same as mother clade" : "Default colours", this, [set] { set({}); });
        inherit->setCheckable(true);
        inherit->setChecked(n->color.isEmpty());
        QPoint at = QCursor::pos();
        if (!canvas->underMouse())
            for (auto b : panel->findChildren<QToolButton*>())
                if (b->defaultAction() == aColor && b->isVisible()) at = b->mapToGlobal(b->rect().bottomLeft());
        m.exec(at);
    }

    void actEdit() {
        Node* n = sel();
        if (!n) return;
        const QString id = n->id;
        QDialog dlg(this);
        dlg.setWindowTitle("Edit clade");
        dlg.resize(560, 420);
        auto nameEdit = new QLineEdit(n->name);
        auto infoEdit = new QPlainTextEdit(n->info);
        auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        auto hint = new QLabel("Enter in name or Ctrl+Enter anywhere: OK   ·   Esc: cancel");
        nameEdit->selectAll();
        infoEdit->setPlaceholderText("(no info)");
        hint->setStyleSheet(colorCss(T.dim));
        auto form = new QVBoxLayout(&dlg);
        form->addWidget(new QLabel("Name:"));
        form->addWidget(nameEdit);
        form->addSpacing(6);
        form->addWidget(new QLabel("Info:"));
        form->addWidget(infoEdit, 1);
        form->addWidget(hint);
        form->addWidget(buttons);

        QPushButton* ok = buttons->button(QDialogButtonBox::Ok);
        auto acceptIfValid = [&] { if (ok->isEnabled()) dlg.accept(); };
        connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
        connect(nameEdit, &QLineEdit::textChanged, &dlg, [ok](const QString& t) { ok->setEnabled(!t.trimmed().isEmpty()); });
        connect(nameEdit, &QLineEdit::returnPressed, &dlg, acceptIfValid);
        for (auto seq : {QKeySequence("Ctrl+Return"), QKeySequence("Ctrl+Enter")})
            connect(new QShortcut(seq, &dlg), &QShortcut::activated, &dlg, acceptIfValid);
        nameEdit->setFocus();
        if (dlg.exec() != QDialog::Accepted || !(n = model.find(id))) return;

        const QString newName = nameEdit->text().trimmed(), newInfo = infoEdit->toPlainText();
        if (newName.isEmpty() || (newName == n->name && newInfo == n->info)) return;
        n->name = newName;
        n->info = newInfo;
        model.commit();
    }

    void actCollapse() {
        Node* n = sel();
        if (!n || n->kids.empty()) return;
        n->collapsed = !n->collapsed;
        model.commit();
    }

    void actDelete() {
        Node* n = sel();
        if (!n) return;
        if (const int inside = n->countDesc(); n->parent && inside + 1 > CONFIRM_DELETE_OVER) {
            const QString id = n->id;
            QMessageBox box(QMessageBox::Warning, "Delete clades", QString("Delete '%1'?").arg(n->name),
                            QMessageBox::Cancel, this);
            box.setInformativeText(QString("This deletes %1 clades: '%2' and the %3 inside it. You can undo it with Ctrl+Z.")
                                       .arg(inside + 1).arg(n->name).arg(inside));
            QPushButton* del = box.addButton("Delete", QMessageBox::DestructiveRole);
            box.setDefaultButton(QMessageBox::Cancel);
            box.exec();
            if (box.clickedButton() != del || !(n = model.find(id))) return;
        }
        const QString parentId = n->parent ? n->parent->id : QString();
        const QString err = model.removeSubtree(n);
        if (err.isEmpty()) canvas->selId = parentId;
        else model.status = err;
        refresh();
    }

    void actCut() {
        Node* n = sel();
        if (!n) return;
        if (!n->parent) {
            model.status = "Cannot cut the root clade";
        } else {
            canvas->cutId = n->id;
            model.status = QString("Cut '%1' (+%2 descendants) — select a new parent and press V")
                               .arg(n->name).arg(n->countDesc());
        }
        refresh();
    }

    void actPaste() {
        Node* target = sel();
        Node* cut = model.find(canvas->cutId);
        if (!target || !cut) return;
        const QString name = cut->name, tname = target->name;
        if (const QString err = model.moveSubtree(cut, target); !err.isEmpty()) {
            model.status = err;
        } else {
            canvas->selId = std::exchange(canvas->cutId, QString());
            model.status = QString("Moved '%1' under '%2'").arg(name, tname);
        }
        refresh();
    }
};

// Layout and painting recurse once per tree level, so an 8 MB stack overflows
// around depth 10-30k. Re-exec once with a larger limit (reserved lazily).
static void ensureBigStack(char** argv) {
#ifdef Q_OS_UNIX
    const rlim_t want = rlim_t(1) << 30;  // 1 GiB
    rlimit rl;
    if (getenv("TREEFERN_STACK") || getrlimit(RLIMIT_STACK, &rl) || rl.rlim_cur == RLIM_INFINITY || rl.rlim_cur >= want)
        return;
    rl.rlim_cur = rl.rlim_max == RLIM_INFINITY ? want : std::min(want, rl.rlim_max);
    if (setrlimit(RLIMIT_STACK, &rl)) return;
    setenv("TREEFERN_STACK", "1", 1);
    execv("/proc/self/exe", argv);  // Linux; elsewhere fall back to argv[0]
    execvp(argv[0], argv);          // on failure, carry on with the old limit
#endif
}

int main(int argc, char** argv) {
    ensureBigStack(argv);
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("TreeFern");
    QCoreApplication::setApplicationName("TreeFern");
    app.setStyle("Fusion");  // MainWindow applies the dark / light palette on top

    MainWindow w;
    w.setWindowTitle("TreeFern");
    w.show();

    const QStringList args = app.arguments();
    if (args.size() > 1) w.openOrCreate(args[1]);
    else if (const QString last = QSettings().value("lastFile").toString();
             (last.isEmpty() || !QFile::exists(last) || !w.openPath(last)) && !w.startupPrompt())
        return 0;
    return app.exec();
}

#include "main.moc"
