// TreeFern — JSON-based live GUI cladogram editor
//
// File:   Ctrl+N new · Ctrl+O open · Ctrl+Shift+S save as · drop a .json file
// View:   Ctrl+1..3 fractal style · L toggle label placement · Ctrl+Shift+D dark mode
//         Home fit view
// Mouse:  left click node/label = select · drag = pan · wheel = zoom (Shift = fast)
//         double-click = edit name & info · right-click = context menu
//         hover a node to see its label even when there is no room for it
// Keys:   A add child  E (or R) edit name & info  C collapse/expand  F focus
//         Del/Backspace delete  X / Ctrl+X cut subtree  V / Ctrl+V paste (graft)
//         Up heaviest daughter · Down mother · Left/Right sister (wraps around)
//         Esc cancel cut / deselect   + / - spread   [ / ] shrink / spacing
// Layout: children are always ordered lightest -> heaviest, so the heaviest
//         clade is the rightmost (clockwise) branch in every style.
// The open file is autosaved on every change and watched for external edits.

#include <QtWidgets>
#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <optional>
#include <tuple>
#include <vector>

// ------------------------------------------------------------- config ---
constexpr double PI = 3.14159265358979323846;
constexpr double ROOT_LEN = 220.0;
constexpr double DEFAULT_SPREAD = 120 * PI / 180, DEFAULT_SHRINK = 0.68;
constexpr double LEAF_GAP = 70.0;             // leaf spacing (cladogram)
constexpr double FERN_CURL = 12 * PI / 180;   // clockwise curl of a fern's main stem
constexpr double MIN_ZOOM = 1e-3, MAX_ZOOM = 1e12;
constexpr double ZOOM_STEP = 1.15, FAST_ZOOM_STEP = 2.0;
constexpr double LABEL_MIN_SCREEN_LEN = 35;
constexpr double LABEL_GAP = 2;     // minimum spacing between label boxes
constexpr double WRAP_COST = 0.45;  // placement penalty per extra wrapped line
constexpr int MAX_LABELS = 400;
constexpr int MAX_RECENT = 10;
constexpr double LEVEL = 120.0;               // generation spacing (cladogram)
// collision nudging (fan / fern)
constexpr double COLLIDE = 0.32;              // node footprint radius, as a fraction of its len
constexpr double NUDGE_STEP = 1.5 * PI / 180; // rotation per relaxation pass
constexpr double MAX_BEND = 45 * PI / 180;    // max extra rotation of any one clade
constexpr int RELAX_ITERS = 60;
constexpr int ANIM_MS = 380;                  // rebalance animation length

// -------------------------------------------------------------- theme ---
struct Theme {
    bool dark;
    QColor bg, fg, dim, accent, cut, labelBg, labelBgHover, link;
    double sat, val;  // node colour saturation / value
};

static Theme makeTheme(bool dark) {
    if (dark)
        return {true, {14, 17, 26}, {225, 230, 240}, {130, 140, 160}, {255, 200, 80}, {255, 90, 90},
                {20, 24, 36, 190}, {28, 34, 50, 240}, {170, 200, 255}, 0.55, 0.95};
    return {false, {246, 247, 250}, {28, 32, 42}, {105, 113, 130}, {200, 120, 0}, {215, 45, 45},
            {255, 255, 255, 205}, {255, 255, 255, 245}, {40, 90, 190}, 0.70, 0.72};
}

static Theme T = makeTheme(true);

static void applyPalette(const Theme& t) {
    QPalette pal = qApp->style()->standardPalette();
    const QColor window = t.dark ? QColor(26, 31, 46) : QColor(234, 236, 242);
    pal.setColor(QPalette::Window, window);
    pal.setColor(QPalette::WindowText, t.fg);
    pal.setColor(QPalette::Base, t.dark ? QColor(16, 19, 30) : QColor(255, 255, 255));
    pal.setColor(QPalette::AlternateBase, t.dark ? QColor(22, 26, 40) : QColor(244, 245, 248));
    pal.setColor(QPalette::Text, t.fg);
    pal.setColor(QPalette::Button, t.dark ? QColor(45, 54, 78) : QColor(218, 222, 231));
    pal.setColor(QPalette::ButtonText, t.fg);
    pal.setColor(QPalette::Highlight, t.dark ? QColor(58, 92, 160) : QColor(80, 120, 200));
    pal.setColor(QPalette::HighlightedText, Qt::white);
    pal.setColor(QPalette::PlaceholderText, t.dim);
    pal.setColor(QPalette::ToolTipBase, window);
    pal.setColor(QPalette::ToolTipText, t.fg);
    for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        pal.setColor(QPalette::Disabled, role, t.dim);
    qApp->setPalette(pal);
}

static QString newId() { return QUuid::createUuid().toString(QUuid::Id128).left(10); }

// --------------------------------------------------------- data model ---
struct Node {
    QString id, name, info;
    bool collapsed = false;
    std::vector<std::unique_ptr<Node>> kids;
    Node* parent = nullptr;

    Node* addChild(std::unique_ptr<Node> c) {
        c->parent = this;
        kids.push_back(std::move(c));
        return kids.back().get();
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
    // true if n == this or n is a descendant of this
    bool contains(const Node* n) const {
        for (; n; n = n->parent)
            if (n == this) return true;
        return false;
    }
};

static std::unique_ptr<Node> makeNode(const QString& name, const QString& info = {}) {
    auto n = std::make_unique<Node>();
    n->id = newId();
    n->name = name;
    n->info = info;
    return n;
}

static std::unique_ptr<Node> sampleTree() {
    auto root = makeNode("Reptiles",
        "Reptiles are an important clade of land animal, sister to modern mammals. Prior to the "
        "K-Pg Boundary Event 65 million years ago, the clade had significantly more diversity.");

    // ---- Archelosauria
    Node* archelo = root->addChild(makeNode("Archelosauria",
        "This is the least diverse of the two basal clades of reptile, and was discovered through "
        "molecular evidence from genome sequencing, challenging the traditional belief that turtles "
        "were more closely-related to snakes than to birds. Subsequent anatomical studies have "
        "identified the presence of a sagittal crest on the supraoccipital bone and a lack of the "
        "entepicondylar foramen on the humerus as diagnostic of this clade."));

    archelo->addChild(makeNode("Turtles",
        "Turtles are a clade of reptile distinguished by a bony composite shell over their bodies. "
        "As with all reptiles, turtles descend from swimming, underwater vertebrates - but in the "
        "case of modern surviving turtles, their ancestors actually went back into the water, and "
        "the modern land-dwelling turtles emerged again. "));

    Node* archo = archelo->addChild(makeNode("Archosauria",
        "The clade which contains modern birds and crocodilians - along with all extinct dinosaurs "
        "and pterosaurs. Diagnostic of this clade are the fenestrae behind the eyes, as well as the "
        "unique four-chambered heart design."));

    Node* croc = archo->addChild(makeNode("Crocodilians",
        "A clade of large, predatory, semi-aquatic reptiles. These large species are covered in "
        "bony armor-plates and have long, flat tails and webbed feet."));

    Node* allig = croc->addChild(makeNode("Alligatoridae",
        "A clade distinct for its broad snout, hidden lower teeth, and freshwater environment."));
    allig->addChild(makeNode("Alligators",
        "Distinct for their bony septum and cold-temperature tolerance."));
    allig->addChild(makeNode("Caimans",
        "A clade unique for the bony ridge between the eyes and needle-like nose."));

    croc->addChild(makeNode("Crocodiles",
        "The clade of crocodilian most amenable to salt water thanks to specialized exchange "
        "glands as well as an interlocking and exposed fourth-tooth."));

    Node* dino = archo->addChild(makeNode("Dinosaurs",
        "The clade containing the modern birds, and also containing the most famous extinct "
        "animals in the world thanks to books, television, and movies. Unique features of this "
        "clade include an open hip socket and fused vertebrae in the lower back to accommodate an "
        "upright posture."));
    dino->addChild(makeNode("Birds",
        "The only living clade of flying reptiles, birds possess a characteristic wing with an "
        "elongated upper arm - even in flightless species. They also lack the tails of other "
        "theropods."));

    // ---- Lepidosaurs
    Node* lepido = root->addChild(makeNode("Lepidosaurs",
        "The most diverse clade of reptiles, containing the tuatara, and all lizards."));

    Node* squam = lepido->addChild(makeNode("Lizards (Squamata)",
        "The most diverse clade of lepidosaur, featuring a common \"lizard-like\" body plan and its "
        "derivatives. Unique for their paired hemipenes in male reproduction, as well as skull "
        "kinesis and skin shedding."));
    squam->addChild(makeNode("Snakes",
        "Snakes are a legless clade of lizard. They possess a highly mobile, kinetic skull; they "
        "lack movable eyelids; and, they accommodate their paired organs sequentially in their "
        "long, slender bodies."));

    lepido->addChild(makeNode("Tuatara",
        "The sole surviving species of the ancient clade Rhynchocephalia, unique for their "
        "possession of the ancestral form of the diapsid skull, keeping intact both the upper and "
        "lower temporal bars."));

    return root;
}

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

    // Open an existing file. On failure the current tree stays open and an
    // error message is returned.
    QString open(const QString& p) {
        QString err;
        auto r = parseFile(p, err);
        if (!r) return err;
        setTree(p, std::move(r));
        status = "Opened " + path;
        emit treeChanged();
        return {};
    }

    // Create a new file containing the given tree (overwrites if it exists).
    QString create(const QString& p, std::unique_ptr<Node> tree) {
        setTree(p, std::move(tree));
        if (!save()) return "Cannot write " + path;
        emit treeChanged();
        return {};
    }

    // Write the current tree to a new file and continue editing there.
    QString saveAs(const QString& p) {
        if (!root_) return "Nothing to save";
        path = QFileInfo(p).absoluteFilePath();
        watch();
        if (!save()) return "Cannot write " + path;
        emit treeChanged();
        return {};
    }

    // Call after every mutation.
    void commit() {
        reindex();
        save();
        emit treeChanged();
    }

    QString removeSubtree(Node* n) {
        if (!n->parent) return "Cannot delete the root clade";
        detach(n);
        commit();
        return {};
    }

    // Cut `n` (with all descendants) and graft it under `target`.
    QString moveSubtree(Node* n, Node* target) {
        if (!n->parent) return "Cannot move the root clade";
        if (n->contains(target)) return "Cannot paste a clade into itself or its descendants";
        target->addChild(detach(n));
        target->collapsed = false;
        commit();
        return {};
    }

signals:
    void treeChanged();
    void statusChanged();

private:
    std::unique_ptr<Node> root_;
    QHash<QString, Node*> index_;
    QDateTime lastMtime_;
    QFileSystemWatcher watcher_;

    void setTree(const QString& p, std::unique_ptr<Node> tree) {
        path = QFileInfo(p).absoluteFilePath();
        root_ = std::move(tree);
        reindex();
        lastMtime_ = QFileInfo(path).lastModified();
        watch();
    }

    void watch() {
        if (!watcher_.files().isEmpty()) watcher_.removePaths(watcher_.files());
        if (!watcher_.directories().isEmpty()) watcher_.removePaths(watcher_.directories());
        if (QFile::exists(path)) watcher_.addPath(path);
        watcher_.addPath(QFileInfo(path).absolutePath());  // catch delete+recreate
    }

    static std::unique_ptr<Node> detach(Node* n) {
        auto& v = n->parent->kids;
        auto it = std::find_if(v.begin(), v.end(), [n](auto& p) { return p.get() == n; });
        std::unique_ptr<Node> up = std::move(*it);
        v.erase(it);
        up->parent = nullptr;
        return up;
    }

    void reindex() {
        index_.clear();
        if (!root_) return;
        std::function<void(Node*)> rec = [&](Node* n) {
            index_[n->id] = n;
            for (auto& k : n->kids) rec(k.get());
        };
        rec(root_.get());
    }

    static std::unique_ptr<Node> fromJson(const QJsonObject& o, QSet<QString>& seen) {
        auto n = std::make_unique<Node>();
        n->id = o.value("id").toString();
        if (n->id.isEmpty() || seen.contains(n->id)) n->id = newId();  // fix dup/missing
        seen.insert(n->id);
        n->name = o.value("name").toString("Unnamed");
        n->info = o.value("info").toString();
        n->collapsed = o.value("collapsed").toBool(false);
        for (const auto& v : o.value("children").toArray())
            if (v.isObject()) n->addChild(fromJson(v.toObject(), seen));
        return n;
    }

    static QJsonObject toJson(const Node* n) {
        QJsonArray kids;
        for (auto& k : n->kids) kids.append(toJson(k.get()));
        return {{"id", n->id}, {"name", n->name}, {"info", n->info},
                {"collapsed", n->collapsed}, {"children", kids}};
    }

    static std::unique_ptr<Node> parseFile(const QString& p, QString& err) {
        QFile f(p);
        if (!f.open(QIODevice::ReadOnly)) {
            err = "Cannot open " + p + ": " + f.errorString();
            return nullptr;
        }
        QJsonParseError pe;
        auto doc = QJsonDocument::fromJson(f.readAll(), &pe);
        if (pe.error != QJsonParseError::NoError) {
            err = QString("JSON error in %1 (offset %2): %3").arg(p).arg(pe.offset).arg(pe.errorString());
            return nullptr;
        }
        if (!doc.isObject()) {
            err = "JSON error in " + p + ": top level must be an object";
            return nullptr;
        }
        QSet<QString> seen;
        return fromJson(doc.object(), seen);
    }

    bool save() {
        if (!root_ || path.isEmpty()) return false;
        QSaveFile f(path);  // atomic write-and-rename
        if (!f.open(QIODevice::WriteOnly)) {
            status = "Cannot write " + path;
            return false;
        }
        f.write(QJsonDocument(toJson(root_.get())).toJson(QJsonDocument::Indented));
        if (!f.commit()) {
            status = "Cannot write " + path;
            return false;
        }
        lastMtime_ = QFileInfo(path).lastModified();
        if (!watcher_.files().contains(path)) watcher_.addPath(path);
        status = "Saved " + QTime::currentTime().toString("HH:mm:ss");
        return true;
    }

    void checkExternal() {
        if (path.isEmpty()) return;
        if (QFile::exists(path) && !watcher_.files().contains(path)) watcher_.addPath(path);
        QDateTime m = QFileInfo(path).lastModified();
        if (!m.isValid() || m == lastMtime_) return;
        lastMtime_ = m;
        QString err;
        auto r = parseFile(path, err);
        if (r) {
            root_ = std::move(r);
            reindex();
            status = "Reloaded external change " + QTime::currentTime().toString("HH:mm:ss");
            emit treeChanged();
        } else {
            status = err + " (kept old tree)";
            emit statusChanged();
        }
    }
};

// ------------------------------------------------------------- layout ---
// len = size scale of the node (its outgoing branch length in the fractal
// styles); angle = direction the node grows in (used for beside-labels).
struct Geom { double x, y, len, angle; int depth; };
// scale = width scale of the stroke; pa / pb = nodes whose motion the two
// endpoints follow while a rebalance is animated
struct Edge { QPointF a, b; int depth; const Node* child; double scale; const Node *pa, *pb; };

struct Layout {
    enum Style { Fan, Fern, Cladogram };
    Style style = Fan;
    double spread = DEFAULT_SPREAD;  // fan angle (cladogram: leaf spacing)
    double shrink = DEFAULT_SHRINK;  // branch ratio (cladogram: generation spacing)
    QHash<const Node*, Geom> pos;
    std::vector<Edge> edges;

    static QString styleName(Style s) {
        static const char* names[] = {"Fractal fan", "Fern frond", "Cladogram"};
        return names[s];
    }

    void compute(const Node* root) {
        pos.clear();
        edges.clear();
        size_.clear();
        leaves_.clear();
        bend_.clear();
        if (!root) return;
        weigh(root);
        if (style == Cladogram) { cladogram(root); return; }
        // place, look for collisions, nudge the clashing clades apart, repeat
        for (int it = 0;; ++it) {
            pos.clear();
            edges.clear();
            pos[root] = {0, 0, ROOT_LEN, -PI / 2, 0};
            branch(root, 0, 0, -PI / 2, ROOT_LEN, 0);
            if (it >= RELAX_ITERS || !relax()) break;
        }
    }

    // clade size: the node plus all of its descendants (collapsed or not)
    int weight(const Node* n) const { return size_.value(n, 1); }

    // children in display order: lightest first, heaviest last (= rightmost)
    std::vector<const Node*> ordered(const Node* n) const {
        std::vector<const Node*> v;
        for (auto& k : n->kids) v.push_back(k.get());
        std::stable_sort(v.begin(), v.end(), [this](const Node* a, const Node* b) {
            return std::make_pair(size_.value(a), leaves_.value(a)) <
                   std::make_pair(size_.value(b), leaves_.value(b));
        });
        return v;
    }

private:
    QHash<const Node*, int> size_;     // node + all descendants (ignores collapse)
    QHash<const Node*, int> leaves_;   // visible leaves (respects collapse)
    QHash<const Node*, double> bend_;  // collision nudge added to a clade's branch angle

    static bool hidesKids(const Node* n) { return n->collapsed || n->kids.empty(); }

    void weigh(const Node* n) {
        int s = 1, l = 0;
        for (auto& k : n->kids) {
            weigh(k.get());
            s += size_[k.get()];
            l += leaves_[k.get()];
        }
        if (hidesKids(n)) l = 1;
        size_[n] = s;
        leaves_[n] = l;
    }

    void fan(const std::vector<const Node*>& kids, size_t b, size_t e, double a0, double a1,
             std::vector<double>& angs) const {
        int total = 0;
        for (size_t i = b; i < e; ++i) total += leaves_.value(kids[i]);
        double a = a0;
        for (size_t i = b; i < e; ++i) {
            double share = (a1 - a0) * leaves_.value(kids[i]) / total;
            angs[i] = a + share / 2;
            a += share;
        }
    }

    void branch(const Node* n, double x, double y, double ang, double len, int depth) {
        if (hidesKids(n)) return;
        const std::vector<const Node*> kids = ordered(n);
        const size_t m = kids.size();
        const double sp = spread * std::pow(0.92, depth);
        std::vector<double> angs(m, ang), lens(m, len * shrink);
        if (style == Fan) {
            if (m > 1) fan(kids, 0, m, ang - sp / 2, ang + sp / 2, angs);
        } else {
            angs[m - 1] = ang + FERN_CURL;
            lens[m - 1] = len * std::sqrt(shrink);
            if (m > 1) {
                fan(kids, 0, m - 1, ang - sp * 0.55, ang - sp * 0.12, angs);
                for (size_t i = 0; i + 1 < m; ++i) lens[i] = len * shrink * 0.85;
            }
        }
        for (size_t i = 0; i < m; ++i) {
            const Node* c = kids[i];
            angs[i] += bend_.value(c);  // rotates c's whole subtree about n
            double cx = x + std::cos(angs[i]) * len, cy = y + std::sin(angs[i]) * len;
            edges.push_back({{x, y}, {cx, cy}, depth, c, len, n, c});
            pos[c] = {cx, cy, lens[i], angs[i], depth + 1};
            branch(c, cx, cy, angs[i], lens[i], depth + 1);
        }
    }

    // The two sister clades under the common ancestor of a and b
    // (null if one contains the other).
    static std::pair<const Node*, const Node*> divergent(const Node* a, const Node* b) {
        if (a->contains(b) || b->contains(a)) return {nullptr, nullptr};
        for (const Node* ca = a; ca->parent; ca = ca->parent)
            if (ca->parent->contains(b)) {
                const Node* cb = b;
                while (cb->parent != ca->parent) cb = cb->parent;
                return {ca, cb};
            }
        return {nullptr, nullptr};
    }

    // proper crossing (shared endpoints don't count)
    static bool crosses(QPointF p, QPointF p2, QPointF q, QPointF q2) {
        const QPointF r = p2 - p, s = q2 - q, qp = q - p;
        const double den = r.x() * s.y() - r.y() * s.x();
        if (std::abs(den) < 1e-12) return false;
        const double t = (qp.x() * s.y() - qp.y() * s.x()) / den;
        const double u = (qp.x() * r.y() - qp.y() * r.x()) / den;
        const double e = 1e-6;
        return t > e && t < 1 - e && u > e && u < 1 - e;
    }

    // One relaxation pass: for every crash, rotate the two sister clades
    // involved a little apart. Returns false when nothing changed.
    bool relax() {
        QHash<const Node*, double> push;
        auto nudge = [&](const Node* a, const Node* b) {
            auto [ca, cb] = divergent(a, b);
            if (!ca) return;
            const double d = std::remainder(pos.value(cb).angle - pos.value(ca).angle, 2 * PI);
            const double s = d >= 0 ? 1 : -1;  // cb is clockwise of ca: push it further
            push[ca] -= s;
            push[cb] += s;
        };

        std::vector<std::pair<const Node*, Geom>> ns;
        for (auto it = pos.cbegin(); it != pos.cend(); ++it) ns.push_back({it.key(), *it});
        for (size_t i = 0; i < ns.size(); ++i)
            for (size_t j = i + 1; j < ns.size(); ++j) {
                const Geom &a = ns[i].second, &b = ns[j].second;
                const double r = COLLIDE * (a.len + b.len);
                const double dx = a.x - b.x, dy = a.y - b.y;
                if (std::abs(dx) < r && std::abs(dy) < r && dx * dx + dy * dy < r * r)
                    nudge(ns[i].first, ns[j].first);
            }

        std::vector<QRectF> boxes;
        for (const Edge& e : edges) boxes.push_back(QRectF(e.a, e.b).normalized());
        for (size_t i = 0; i < edges.size(); ++i)
            for (size_t j = i + 1; j < edges.size(); ++j)
                if (boxes[i].intersects(boxes[j]) &&
                    crosses(edges[i].a, edges[i].b, edges[j].a, edges[j].b))
                    nudge(edges[i].child, edges[j].child);

        bool changed = false;
        for (auto it = push.cbegin(); it != push.cend(); ++it) {
            const double old = bend_.value(it.key());
            const double nb = std::clamp(old + std::clamp(it.value(), -1.0, 1.0) * NUDGE_STEP,
                                         -MAX_BEND, MAX_BEND);
            if (nb != old) { bend_[it.key()] = nb; changed = true; }
        }
        return changed;
    }

    void cladogram(const Node* root) {
        const double gap = LEAF_GAP * spread / DEFAULT_SPREAD;
        const double level = LEVEL * shrink / DEFAULT_SHRINK;
        const double len = level * 0.7;
        double next = 0;
        std::function<double(const Node*, int)> place = [&](const Node* n, int depth) -> double {
            double x;
            if (hidesKids(n)) {
                x = next;
                next += gap;
            } else {
                double first = 0, last = 0;
                bool any = false;
                for (const Node* c : ordered(n)) {
                    double cx = place(c, depth + 1);
                    if (!any) { first = cx; any = true; }
                    last = cx;
                }
                x = (first + last) / 2;
            }
            pos[n] = {x, -depth * level, len, -PI / 2, depth};
            return x;
        };
        const double rx = place(root, 0);
        for (auto it = pos.begin(); it != pos.end(); ++it) it->x -= rx;

        std::function<void(const Node*)> link = [&](const Node* n) {
            if (hidesKids(n)) return;
            const Geom p = pos.value(n);
            for (const Node* c : ordered(n)) {
                const Geom g = pos.value(c);
                if (std::abs(g.x - p.x) > 1e-9)
                    edges.push_back({{p.x, p.y}, {g.x, p.y}, p.depth, c, len, n, c});
                edges.push_back({{g.x, p.y}, {g.x, g.y}, p.depth, c, len, c, c});
                link(c);
            }
        };
        link(root);
    }
};

// ------------------------------------------------------------- camera ---
struct Camera {
    double x = 0, y = -180, zoom = 1;
    void reset() { x = 0; y = -180; zoom = 1; }
    QPointF toScreen(double wx, double wy, QSizeF v) const {
        return {(wx - x) * zoom + v.width() / 2, (wy - y) * zoom + v.height() / 2};
    }
    QPointF toWorld(QPointF s, QSizeF v) const {
        return {(s.x() - v.width() / 2) / zoom + x, (s.y() - v.height() / 2) / zoom + y};
    }
    void zoomAt(double f, QPointF s, QSizeF v) {
        QPointF w = toWorld(s, v);
        zoom = std::clamp(zoom * f, MIN_ZOOM, MAX_ZOOM);
        QPointF n = toWorld(s, v);
        x += w.x() - n.x();
        y += w.y() - n.y();
    }
};

// ---------------------------------------------------------- utilities ---
static QColor depthColor(int d, double bright = 1.0) {
    return QColor::fromHsvF(((200 + d * 37) % 360) / 360.0, T.sat, std::min(1.0, T.val * bright));
}

// Liang–Barsky: clip segment a-b to rect; returns false if fully outside.
static bool clipLine(const QRectF& r, QPointF& a, QPointF& b) {
    double t0 = 0, t1 = 1, dx = b.x() - a.x(), dy = b.y() - a.y();
    const double p[4] = {-dx, dx, -dy, dy};
    const double q[4] = {a.x() - r.left(), r.right() - a.x(), a.y() - r.top(), r.bottom() - a.y()};
    for (int i = 0; i < 4; ++i) {
        if (p[i] == 0) {
            if (q[i] < 0) return false;
        } else {
            double t = q[i] / p[i];
            if (p[i] < 0) { if (t > t1) return false; t0 = std::max(t0, t); }
            else          { if (t < t0) return false; t1 = std::min(t1, t); }
        }
    }
    QPointF o = a, d(dx, dy);
    a = o + d * t0;
    b = o + d * t1;
    return true;
}

// Box of size w*h whose node-facing side touches (px, py).
static QRectF anchoredRect(double w, double h, double px, double py, double ux, double uy) {
    double l = ux > 0.35 ? px : ux < -0.35 ? px - w : px - w / 2;
    double t = uy > 0.35 ? py : uy < -0.35 ? py - h : py - h / 2;
    return QRectF(std::round(l), std::round(t), w, h);
}

// Ways to lay out a label: one line, plus balanced 2- and 3-line wraps when
// they make the box noticeably narrower. Never truncates.
static std::vector<QStringList> wrapOptions(const QStringList& words, const QFontMetricsF& fm) {
    const int n = words.size();
    std::vector<double> pre(n + 1, 0);
    for (int i = 0; i < n; ++i) pre[i + 1] = pre[i] + fm.horizontalAdvance(words[i]);
    const double sp = fm.horizontalAdvance(' ');
    auto W = [&](int a, int b) { return pre[b] - pre[a] + (b - a - 1) * sp; };
    auto seg = [&](int a, int b) { return words.mid(a, b - a).join(' '); };

    std::vector<QStringList> out{{words.join(' ')}};
    double prevW = W(0, n);
    if (n >= 2) {
        int bi = 1;
        double bw = 1e300;
        for (int i = 1; i < n; ++i) {
            double m = std::max(W(0, i), W(i, n));
            if (m < bw) { bw = m; bi = i; }
        }
        if (bw < prevW * 0.9) { out.push_back({seg(0, bi), seg(bi, n)}); prevW = bw; }
    }
    if (n >= 3) {
        int bi = 1, bj = 2;
        double bw = 1e300;
        for (int i = 1; i < n - 1; ++i)
            for (int j = i + 1; j < n; ++j) {
                double m = std::max({W(0, i), W(i, j), W(j, n)});
                if (m < bw) { bw = m; bi = i; bj = j; }
            }
        if (bw < prevW * 0.9) out.push_back({seg(0, bi), seg(bi, bj), seg(bj, n)});
    }
    return out;
}

static QString fmtZoom(double z) {
    return z < 1e4 ? QString::number(z, 'f', 2) + "x" : QString::number(z, 'e', 2) + "x";
}

// ------------------------------------------------------------- canvas ---
class Canvas : public QWidget {
    Q_OBJECT
public:
    enum class LabelMode { Over, Beside };

    Layout lay;                          // target layout
    QHash<const Node*, Geom> shownPos;   // what is drawn (animated towards lay)
    std::vector<Edge> shownEdges;
    Camera cam;
    QString selId, cutId;
    LabelMode labelMode = LabelMode::Over;

    explicit Canvas(Model* m) : model(m) {
        setFocusPolicy(Qt::StrongFocus);
        setMinimumSize(300, 200);
        setMouseTracking(true);
        anim.setDuration(ANIM_MS);
        anim.setStartValue(0.0);
        anim.setEndValue(1.0);
        anim.setEasingCurve(QEasingCurve::OutCubic);
        connect(&anim, &QVariantAnimation::valueChanged, this,
                [this](const QVariant& v) { applyFrame(v.toDouble()); });
        connect(&anim, &QVariantAnimation::finished, this, [this] { applyFrame(1); following = false; });
        relayout(false);
    }

    // Recompute the layout and animate from what is on screen now. While it
    // animates, the camera sticks to the selected node until the user pans
    // or zooms.
    void relayout(bool animate = true) {
        from_ = animate ? shownById_ : QHash<QString, QPointF>();
        lay.compute(model->root());
        anim.stop();
        havePrev_ = false;
        if (!from_.isEmpty()) {
            following = !selId.isEmpty();
            applyFrame(0);
            anim.start();
        } else {
            following = false;
            applyFrame(1);
        }
    }

    void stopFollowing() { following = false; }

    void focusOn(const Node* n) {
        if (!lay.pos.contains(n)) return;
        following = false;
        const Geom& g = lay.pos[n];
        cam.x = g.x;
        cam.y = g.y;
        cam.zoom = std::max(cam.zoom, std::min(MAX_ZOOM, 120 / g.len));
        update();
        emit viewChanged();
    }

    void ensureVisible(const Node* n) {
        if (!lay.pos.contains(n)) return;
        const Geom& g = lay.pos[n];
        QRectF safe = QRectF(rect()).adjusted(60, 60, -60, -60);
        if (safe.contains(cam.toScreen(g.x, g.y, size()))) return;
        following = false;
        cam.x = g.x;
        cam.y = g.y;
        update();
        emit viewChanged();
    }

    void fitView() {
        following = false;
        if (lay.pos.isEmpty()) { cam.reset(); update(); emit viewChanged(); return; }
        double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
        for (const Geom& g : lay.pos) {
            x0 = std::min(x0, g.x); x1 = std::max(x1, g.x);
            y0 = std::min(y0, g.y); y1 = std::max(y1, g.y);
        }
        const double bw = std::max(x1 - x0, 1.0), bh = std::max(y1 - y0, 1.0);
        cam.x = (x0 + x1) / 2;
        cam.y = (y0 + y1) / 2;
        cam.zoom = std::clamp(std::min(std::max(100, width() - 120) / bw,
                                       std::max(100, height() - 120) / bh), MIN_ZOOM, 2.0);
        update();
        emit viewChanged();
    }

signals:
    void clicked(const QString& id);
    void doubleClicked(const QString& id);
    void contextMenu(const QString& id, const QPoint& global);
    void viewChanged();

protected:
    void paintEvent(QPaintEvent*) override;

    void mousePressEvent(QMouseEvent* e) override {
        pressPos = lastPos = e->position();
        dragBtn = e->button();
        moved = false;
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (dragBtn == Qt::NoButton) { updateHover(e->position()); return; }
        QPointF d = e->position() - lastPos;
        if (QLineF(e->position(), pressPos).length() > 4) moved = true;
        if (moved) {
            following = false;  // user took over the camera
            cam.x -= d.x() / cam.zoom;
            cam.y -= d.y() / cam.zoom;
            update();
            emit viewChanged();
        }
        lastPos = e->position();
    }
    void mouseReleaseEvent(QMouseEvent* e) override {
        auto btn = dragBtn;
        dragBtn = Qt::NoButton;
        if (moved || btn != e->button()) { updateHover(e->position()); return; }
        const Node* hit = hitTest(e->position());
        QString id = hit ? hit->id : QString();
        if (btn == Qt::LeftButton) emit clicked(id);
        else if (btn == Qt::RightButton) emit contextMenu(id, e->globalPosition().toPoint());
    }
    void mouseDoubleClickEvent(QMouseEvent* e) override {
        dragBtn = Qt::NoButton;
        if (e->button() != Qt::LeftButton) return;
        if (const Node* hit = hitTest(e->position())) emit doubleClicked(hit->id);
    }
    void wheelEvent(QWheelEvent* e) override {
        bool fast = e->modifiers() & Qt::ShiftModifier;
        double notches = e->angleDelta().y() / 120.0;
        if (notches == 0 && fast) notches = e->angleDelta().x() / 120.0;
        if (notches != 0) {
            following = false;  // user took over the camera
            cam.zoomAt(std::pow(fast ? FAST_ZOOM_STEP : ZOOM_STEP, notches), e->position(), size());
            update();
            updateHover(e->position());
            emit viewChanged();
        }
    }
    void leaveEvent(QEvent*) override {
        if (!hoverId.isEmpty()) { hoverId.clear(); update(); }
        unsetCursor();
    }

private:
    Model* model;
    QPointF pressPos, lastPos;
    Qt::MouseButton dragBtn = Qt::NoButton;
    bool moved = false;
    QString hoverId;
    std::vector<std::pair<QRectF, QString>> labelHits;

    // animation state (keyed by id: node pointers may have been freed)
    QVariantAnimation anim;
    QHash<QString, QPointF> from_, shownById_;
    bool following = false, havePrev_ = false;
    QPointF followPrev_;

    // Where n starts the animation: its old position, or (for new nodes)
    // that of its nearest ancestor that already existed, so it grows out.
    QPointF startOf(const Node* n) const {
        for (const Node* p = n; p; p = p->parent)
            if (auto it = from_.constFind(p->id); it != from_.cend()) return *it;
        const Geom& g = lay.pos[n];
        return {g.x, g.y};
    }

    void applyFrame(double t) {
        shownPos.clear();
        shownById_.clear();
        QHash<const Node*, QPointF> off;
        for (auto it = lay.pos.cbegin(); it != lay.pos.cend(); ++it) {
            Geom g = *it;
            const QPointF o = t >= 1 ? QPointF() : (startOf(it.key()) - QPointF(g.x, g.y)) * (1 - t);
            off[it.key()] = o;
            g.x += o.x();
            g.y += o.y();
            shownPos[it.key()] = g;
            shownById_[it.key()->id] = {g.x, g.y};
        }
        shownEdges = lay.edges;
        for (Edge& e : shownEdges) {
            e.a += off.value(e.pa);
            e.b += off.value(e.pb);
        }
        // keep the followed node fixed on screen
        if (following) {
            if (const Node* f = model->find(selId); f && shownPos.contains(f)) {
                const QPointF cur(shownPos[f].x, shownPos[f].y);
                if (havePrev_) {
                    cam.x += cur.x() - followPrev_.x();
                    cam.y += cur.y() - followPrev_.y();
                    emit viewChanged();
                }
                followPrev_ = cur;
                havePrev_ = true;
            }
        }
        update();
    }

    double nodeRadius(double len) const { return std::clamp(len * 0.06 * cam.zoom + 2, 3.0, 18.0); }

    void updateHover(QPointF pos) {
        const Node* h = hitTest(pos);
        QString id = h ? h->id : QString();
        if (h) setCursor(Qt::PointingHandCursor);
        else unsetCursor();
        if (id != hoverId) { hoverId = id; update(); }
    }

    const Node* hitTest(QPointF p) const {
        const Node* best = nullptr;
        double bd = 1e18;
        bool exact = false;
        for (auto it = shownPos.cbegin(); it != shownPos.cend(); ++it) {
            QPointF s = cam.toScreen(it->x, it->y, size());
            double d = QLineF(s, p).length(), r = nodeRadius(it->len);
            if (d <= r + 5 && d < bd) { best = it.key(); bd = d; exact = d <= r; }
        }
        if (exact) return best;
        for (auto it = labelHits.rbegin(); it != labelHits.rend(); ++it)
            if (it->first.contains(p))
                if (const Node* n = model->find(it->second)) return n;
        return best;
    }
};

void Canvas::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), T.bg);
    labelHits.clear();
    if (!model->isOpen()) {
        p.setPen(T.dim);
        p.drawText(rect(), Qt::AlignCenter, "No file open — use File ▸ Open or File ▸ New");
        return;
    }
    const QSizeF vs = size();
    const Node* sel = model->find(selId);
    const Node* cut = model->find(cutId);
    const Node* hov = model->find(hoverId);
    auto inCut = [&](const Node* n) { return cut && cut->contains(n); };
    const bool beside = labelMode == LabelMode::Beside;

    // ---- edges (clipped to view; kept as label obstacles in beside mode)
    struct Seg { QPointF a, b; double pad; QRectF box; };
    std::vector<Seg> segs;
    const QRectF view(-60, -60, vs.width() + 120, vs.height() + 120);
    for (const Edge& e : shownEdges) {
        QPointF a = cam.toScreen(e.a.x(), e.a.y(), vs), b = cam.toScreen(e.b.x(), e.b.y(), vs);
        if (!clipLine(view, a, b)) continue;
        int w = std::clamp(int(e.scale * 0.04 * cam.zoom), 1, 30);
        QColor c = inCut(e.child) ? T.cut.darker(140) : depthColor(e.depth, 0.7);
        p.setPen(QPen(c, w, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(a, b);
        if (beside) {
            double pad = w / 2.0 + 1;
            segs.push_back({a, b, pad, QRectF(a, b).normalized().adjusted(-pad, -pad, pad, pad)});
        }
    }

    // ---- visible nodes
    struct Vis { const Node* n; QPointF s; Geom g; double r; int own; };
    std::vector<Vis> nodes;
    std::vector<QRectF> nodeRects;
    for (auto it = lay.pos.cbegin(); it != lay.pos.cend(); ++it) {
        QPointF s = cam.toScreen(it->x, it->y, vs);
        if (s.x() < -100 || s.x() > vs.width() + 100 || s.y() < -100 || s.y() > vs.height() + 100)
            continue;
        double r = nodeRadius(it->len);
        nodes.push_back({it.key(), s, *it, r, int(nodeRects.size())});
        nodeRects.emplace_back(s.x() - r - 2, s.y() - r - 2, 2 * r + 4, 2 * r + 4);
    }
    p.setPen(Qt::NoPen);
    for (auto& v : nodes) {
        p.setBrush(inCut(v.n) ? T.cut : depthColor(v.g.depth));
        p.drawEllipse(v.s, v.r, v.r);
    }
    p.setBrush(Qt::NoBrush);
    for (auto& v : nodes) {
        if (v.n == sel) { p.setPen(QPen(T.accent, 2)); p.drawEllipse(v.s, v.r + 4, v.r + 4); }
        if (v.n == cut) { p.setPen(QPen(T.cut, 2, Qt::DashLine)); p.drawEllipse(v.s, v.r + 8, v.r + 8); }
    }

    // ---- labels. Over mode: centred on the node, nudged (staying on top of
    //      it) and/or wrapped to avoid other labels. Beside mode: next to the
    //      node, avoiding labels, nodes and branches. Biggest clades first.
    std::vector<Vis> cands;
    for (auto& v : nodes)
        if (v.g.len * cam.zoom > LABEL_MIN_SCREEN_LEN || v.n == sel) cands.push_back(v);
    std::sort(cands.begin(), cands.end(), [&](const Vis& a, const Vis& b) {
        return std::make_tuple(a.n != sel, -lay.weight(a.n), a.g.depth, -a.g.len) <
               std::make_tuple(b.n != sel, -lay.weight(b.n), b.g.depth, -b.g.len);
    });

    struct Lbl { QRectF box; QString text; QFont font; const Node* n; bool forced; };
    std::vector<Lbl> labels;
    std::vector<QRectF> placed;
    const double padX = 5, padY = 2;

    auto blocked = [&](const QRectF& r, int own) {
        QRectF t = r.adjusted(-LABEL_GAP, -LABEL_GAP, LABEL_GAP, LABEL_GAP);
        for (const QRectF& q : placed)
            if (q.intersects(t)) return true;
        if (!beside) return false;
        for (size_t i = 0; i < nodeRects.size(); ++i)
            if (int(i) != own && nodeRects[i].intersects(t)) return true;
        for (const Seg& s : segs) {
            if (!s.box.intersects(t)) continue;
            QPointF a = s.a, b = s.b;
            if (clipLine(t.adjusted(-s.pad, -s.pad, s.pad, s.pad), a, b)) return true;
        }
        return false;
    };

    static const double OFFS[] = {0, .45, -.45, .9, -.9, 1.4, -1.4, 2.0, -2.0, PI};

    auto makeLabel = [&](const Vis& v, bool force) -> std::optional<Lbl> {
        QStringList words = v.n->name.split(' ', Qt::SkipEmptyParts);
        if (words.isEmpty()) words << "?";
        if (v.n == cut) words[0] = "✂ " + words[0];
        if (v.n->collapsed && !v.n->kids.empty()) words << QString("[+%1]").arg(v.n->countDesc());

        QFont f = font();
        f.setPixelSize(std::clamp(int(v.g.len * cam.zoom * 0.09), 11, 40));
        QFontMetricsF fm(f);

        if (beside) {  // single line, beside the node, as before
            const QString text = words.join(' ');
            const double w = std::ceil(fm.horizontalAdvance(text)) + 2 * padX;
            const double h = std::ceil(fm.height()) + 2 * padY;
            const double gap = v.r + 6;
            auto at = [&](double off) {
                double ux = std::cos(v.g.angle + off), uy = std::sin(v.g.angle + off);
                return anchoredRect(w, h, v.s.x() + ux * gap, v.s.y() + uy * gap, ux, uy);
            };
            for (double off : OFFS) {
                QRectF r = at(off);
                if (!blocked(r, v.own)) return Lbl{r, text, f, v.n, false};
            }
            if (!force) return std::nullopt;
            return Lbl{at(0), text, f, v.n, true};
        }

        const std::vector<QStringList> opts = wrapOptions(words, fm);
        struct Cand { double cost; QRectF r; int opt; };
        std::vector<Cand> cs;
        for (int k = 0; k < int(opts.size()); ++k) {
            double tw = 0;
            for (const QString& line : opts[k]) tw = std::max(tw, fm.horizontalAdvance(line));
            const double w = std::ceil(tw) + 2 * padX;
            const double h = std::ceil(fm.height() + (opts[k].size() - 1) * fm.lineSpacing()) + 2 * padY;
            // largest shifts that still leave the box touching the node disc
            const double mx = w / 2 + v.r - 1, my = h / 2 + v.r - 1;
            for (int i = -2; i <= 2; ++i)
                for (int j = -2; j <= 2; ++j) {
                    double dx = mx * i / 2, dy = my * j / 2;
                    QRectF r(std::round(v.s.x() + dx - w / 2), std::round(v.s.y() + dy - h / 2), w, h);
                    double qx = std::clamp(v.s.x(), r.left(), r.right());
                    double qy = std::clamp(v.s.y(), r.top(), r.bottom());
                    if (std::hypot(qx - v.s.x(), qy - v.s.y()) > v.r) continue;  // must stay over node
                    cs.push_back({std::abs(dx) / w + std::abs(dy) / h + WRAP_COST * k, r, k});
                }
        }
        std::stable_sort(cs.begin(), cs.end(), [](const Cand& a, const Cand& b) { return a.cost < b.cost; });
        for (const Cand& c : cs)
            if (!blocked(c.r, v.own)) return Lbl{c.r, opts[c.opt].join('\n'), f, v.n, false};
        if (!force) return std::nullopt;  // no room: hidden until zoomed in (or hovered)
        return Lbl{cs.front().r, opts[cs.front().opt].join('\n'), f, v.n, true};
    };

    bool hovShown = false;
    for (const Vis& v : cands) {
        if (int(labels.size()) >= MAX_LABELS) break;
        if (auto l = makeLabel(v, v.n == sel)) {
            placed.push_back(l->box);
            labels.push_back(*l);
            if (v.n == hov) hovShown = true;
        }
    }
    // hovered node without a label: draw one on top of everything, without
    // disturbing the placement of the others
    if (hov && !hovShown)
        for (const Vis& v : nodes)
            if (v.n == hov) {
                if (auto l = makeLabel(v, true)) labels.push_back(*l);
                break;
            }

    for (const Lbl& l : labels) {
        const bool isSel = l.n == sel, isHov = l.n == hov;
        p.setPen(isSel ? QPen(T.accent, 1) : isHov ? QPen(T.dim, 1) : QPen(Qt::NoPen));
        p.setBrush(isHov || l.forced ? T.labelBgHover : T.labelBg);
        p.drawRoundedRect(l.box, 5, 5);
        p.setFont(l.font);
        p.setPen(isSel ? T.accent : inCut(l.n) ? T.cut : T.fg);
        p.drawText(l.box.adjusted(padX, padY, -padX, -padY), Qt::AlignCenter, l.text);
        labelHits.push_back({l.box, l.n->id});
    }

    if (selId.isEmpty()) {
        p.setFont(font());
        p.setPen(T.dim);
        p.drawText(10, 20, "Click a node to select · wheel zoom (Shift = fast) · drag pan · "
                           "double-click to edit · right-click for menu · arrows navigate · X/V cut & graft");
    }
}

// --------------------------------------------------------- info panel ---
class InfoPanel : public QWidget {
    Q_OBJECT
public:
    explicit InfoPanel(const QList<QAction*>& acts) {
        auto v = new QVBoxLayout(this);
        title = new QLabel;
        QFont f = title->font();
        f.setPointSize(16);
        f.setBold(true);
        title->setFont(f);
        title->setWordWrap(true);
        meta = new QLabel;
        meta->setWordWrap(true);
        meta->setTextInteractionFlags(Qt::TextSelectableByMouse);
        v->addWidget(title);
        v->addWidget(meta);

        auto grid = new QGridLayout;
        for (int i = 0; i < acts.size(); ++i) {
            auto b = new QToolButton;
            b->setDefaultAction(acts[i]);
            b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            b->setMinimumHeight(30);
            grid->addWidget(b, i / 2, i % 2);
        }
        v->addLayout(grid);

        v->addWidget(new QLabel("Subclades:"));
        kids = new QListWidget;
        kids->setMaximumHeight(160);
        v->addWidget(kids);
        up = new QPushButton;
        up->setFlat(true);
        v->addWidget(up);
        v->addWidget(new QLabel("Info:"));
        info = new QTextBrowser;
        info->setPlaceholderText("(no info — press E, double-click the node, or click 'Edit')");
        v->addWidget(info, 1);

        connect(kids, &QListWidget::itemClicked, this,
                [this](QListWidgetItem* it) { emit selectRequested(it->data(Qt::UserRole).toString()); });
        connect(up, &QPushButton::clicked, this, [this] { emit selectRequested(upId); });
        restyle();
    }

    void restyle() {
        title->setStyleSheet(QString("color:%1").arg(T.accent.name()));
        meta->setStyleSheet(QString("color:%1").arg(T.dim.name()));
        up->setStyleSheet(QString("text-align:left; color:%1").arg(T.link.name()));
    }

    void setNode(const Node* n) {
        title->setText(n->name);
        meta->setText(QString("Depth: %1\nParent: %2\nDirect subclades: %3\n"
                              "Total descendants: %4\nID: %5")
                          .arg(n->depth())
                          .arg(n->parent ? n->parent->name : "— (root)")
                          .arg(n->kids.size())
                          .arg(n->countDesc())
                          .arg(n->id));
        kids->clear();
        for (auto& k : n->kids) {
            auto it = new QListWidgetItem("› " + k->name, kids);
            it->setData(Qt::UserRole, k->id);
        }
        upId = n->parent ? n->parent->id : QString();
        up->setVisible(n->parent);
        if (n->parent) up->setText("‹ up to " + n->parent->name);
        info->setPlainText(n->info);
    }

signals:
    void selectRequested(const QString& id);

private:
    QLabel *title, *meta;
    QListWidget* kids;
    QPushButton* up;
    QTextBrowser* info;
    QString upId;
};

// -------------------------------------------------------- main window ---
static const char* FILE_FILTER = "Cladogram JSON (*.json);;All files (*)";

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow() {
        resize(1280, 800);
        setAcceptDrops(true);
        canvas = new Canvas(&model);
        setCentralWidget(canvas);

        auto mk = [this](const QString& text, QList<QKeySequence> keys, auto fn) {
            auto a = new QAction(text, this);
            a->setShortcuts(keys);
            a->setShortcutContext(Qt::WindowShortcut);
            connect(a, &QAction::triggered, this, fn);
            addAction(a);
            return a;
        };

        // ---- File menu
        QMenu* fileMenu = menuBar()->addMenu("&File");
        fileMenu->addAction(mk("&New…", {QKeySequence::New}, [this] { fileNew(); }));
        fileMenu->addAction(mk("&Open…", {QKeySequence::Open}, [this] { fileOpen(); }));
        recentMenu = fileMenu->addMenu("Open &Recent");
        connect(recentMenu, &QMenu::aboutToShow, this, &MainWindow::rebuildRecentMenu);
        aSaveAs = mk("Save &As…", {QKeySequence("Ctrl+Shift+S")}, [this] { fileSaveAs(); });
        fileMenu->addAction(aSaveAs);
        fileMenu->addSeparator();
        fileMenu->addAction(mk("&Quit", {QKeySequence::Quit}, [this] { close(); }));

        // ---- clade actions
        aAdd      = mk("Add child (A)", {Qt::Key_A}, [this] { actAdd(); });
        aEdit     = mk("Edit (E)", {Qt::Key_E, Qt::Key_R}, [this] { actEdit(); });
        aCollapse = mk("Collapse (C)", {Qt::Key_C}, [this] { actCollapse(); });
        aFocus    = mk("Focus (F)", {Qt::Key_F}, [this] { if (auto n = sel()) canvas->focusOn(n); });
        aDelete   = mk("Delete (Del)", {Qt::Key_Delete, Qt::Key_Backspace}, [this] { actDelete(); });
        aCut      = mk("Cut subtree (X)", {Qt::Key_X, QKeySequence::Cut}, [this] { actCut(); });
        aPaste    = mk("Paste here (V)", {Qt::Key_V, QKeySequence::Paste}, [this] { actPaste(); });
        mk("Escape", {Qt::Key_Escape}, [this] {
            if (!canvas->cutId.isEmpty()) { canvas->cutId.clear(); model.status = "Cut cancelled"; }
            else canvas->selId.clear();
            refresh();
        });
        auto layoutKey = [this, &mk](QList<QKeySequence> k, auto fn) {
            mk("", k, [this, fn] { fn(canvas->lay); canvas->relayout(); updateHud(); });
        };
        layoutKey({Qt::Key_Plus, Qt::Key_Equal},
                  [](Layout& l) { l.spread = std::min(340 * PI / 180, l.spread + 10 * PI / 180); });
        layoutKey({Qt::Key_Minus},
                  [](Layout& l) { l.spread = std::max(20 * PI / 180, l.spread - 10 * PI / 180); });
        layoutKey({Qt::Key_BracketLeft}, [](Layout& l) { l.shrink = std::max(0.4, l.shrink - 0.02); });
        layoutKey({Qt::Key_BracketRight}, [](Layout& l) { l.shrink = std::min(0.9, l.shrink + 0.02); });

        // ---- navigation
        QAction* aDaughter = mk("Heaviest daughter", {Qt::Key_Up}, [this] { navigate(Nav::Daughter); });
        QAction* aRight    = mk("Sister to the right", {Qt::Key_Right}, [this] { navigate(Nav::Right); });
        QAction* aLeft     = mk("Sister to the left", {Qt::Key_Left}, [this] { navigate(Nav::Left); });
        QAction* aMother   = mk("Mother", {Qt::Key_Down}, [this] { navigate(Nav::Mother); });

        QMenu* editMenu = menuBar()->addMenu("&Clade");
        editMenu->addActions({aAdd, aEdit, aCollapse, aFocus});
        editMenu->addSeparator();
        editMenu->addActions({aCut, aPaste});
        editMenu->addSeparator();
        editMenu->addAction(aDelete);
        editMenu->addSeparator();
        QMenu* goMenu = editMenu->addMenu("&Go to");
        goMenu->addActions({aDaughter, aRight, aLeft, aMother});

        // ---- View menu
        QMenu* viewMenu = menuBar()->addMenu("&View");
        viewMenu->addSection("Fractal");
        auto fractalGroup = new QActionGroup(this);
        const Layout::Style styles[] = {Layout::Fan, Layout::Fern, Layout::Cladogram};
        for (int i = 0; i < 3; ++i) {
            const Layout::Style st = styles[i];
            QAction* a = mk(Layout::styleName(st), {QKeySequence(QString("Ctrl+%1").arg(i + 1))},
                            [this, st] { setFractal(st); });
            a->setCheckable(true);
            fractalGroup->addAction(a);
            viewMenu->addAction(a);
            styleActs.push_back(a);
        }

        viewMenu->addSection("Labels");
        auto labelGroup = new QActionGroup(this);
        aLabelsOver = mk("Labels over nodes", {}, [this] { setLabelsBeside(false); });
        aLabelsBeside = mk("Labels beside nodes", {}, [this] { setLabelsBeside(true); });
        for (QAction* a : {aLabelsOver, aLabelsBeside}) {
            a->setCheckable(true);
            labelGroup->addAction(a);
            viewMenu->addAction(a);
        }
        viewMenu->addAction(mk("Toggle label placement", {Qt::Key_L}, [this] {
            setLabelsBeside(canvas->labelMode == Canvas::LabelMode::Over);
        }));

        viewMenu->addSection("Theme");
        aDark = mk("Dark mode", {QKeySequence("Ctrl+Shift+D")}, [this](bool on) { applyTheme(on); });
        aDark->setCheckable(true);
        viewMenu->addAction(aDark);
        viewMenu->addSeparator();
        viewMenu->addAction(mk("Fit view", {Qt::Key_Home}, [this] { canvas->fitView(); }));

        panel = new InfoPanel({aAdd, aEdit, aCollapse, aFocus, aCut, aPaste, aDelete});
        dock = new QDockWidget("Clade", this);
        dock->setWidget(panel);
        dock->setFeatures(QDockWidget::NoDockWidgetFeatures);
        dock->setMinimumWidth(340);
        addDockWidget(Qt::RightDockWidgetArea, dock);

        hud = new QLabel;
        statusBar()->addWidget(hud, 1);

        connect(&model, &Model::treeChanged, this, [this] { canvas->relayout(); refresh(); });
        connect(&model, &Model::statusChanged, this, &MainWindow::updateHud);
        connect(canvas, &Canvas::viewChanged, this, &MainWindow::updateHud);
        connect(canvas, &Canvas::clicked, this, &MainWindow::select);
        connect(panel, &InfoPanel::selectRequested, this, &MainWindow::select);
        connect(canvas, &Canvas::doubleClicked, this, [this](const QString& id) { select(id); actEdit(); });
        connect(canvas, &Canvas::contextMenu, this, [this](const QString& id, const QPoint& g) {
            if (!id.isEmpty()) select(id);
            if (!sel()) return;
            QMenu m;
            m.addActions({aAdd, aEdit, aCollapse, aFocus});
            m.addSeparator();
            m.addActions({aCut, aPaste});
            m.addSeparator();
            m.addAction(aDelete);
            m.exec(g);
        });

        // ---- restore view preferences
        QSettings s;
        const int st = std::clamp(s.value("view/fractal", 0).toInt(), 0, 2);
        canvas->lay.style = Layout::Style(st);
        styleActs[st]->setChecked(true);
        const bool besideLabels = s.value("view/labelsBeside", false).toBool();
        canvas->labelMode = besideLabels ? Canvas::LabelMode::Beside : Canvas::LabelMode::Over;
        (besideLabels ? aLabelsBeside : aLabelsOver)->setChecked(true);
        applyTheme(s.value("view/dark", true).toBool());
        canvas->relayout(false);   // was: canvas->relayout();
        refresh();
    }

    bool isOpen() const { return model.isOpen(); }

    // Open a file; on failure show an error and keep the current tree.
    bool openPath(const QString& p) {
        QString err = model.open(p);
        if (!err.isEmpty()) {
            QMessageBox::warning(this, "Cannot open file", err);
            return false;
        }
        afterSwitch();
        return true;
    }

    // Command-line behaviour: open if it exists, otherwise create with sample tree.
    bool openOrCreate(const QString& p) {
        if (QFile::exists(p)) return openPath(p);
        QString err = model.create(p, sampleTree());
        if (!err.isEmpty()) {
            QMessageBox::warning(this, "Cannot create file", err);
            return false;
        }
        afterSwitch();
        return true;
    }

    // Shown at startup when there is no file to open.
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
    Canvas* canvas;
    InfoPanel* panel;
    QDockWidget* dock;
    QLabel* hud;
    QMenu* recentMenu;
    QAction *aAdd, *aEdit, *aCollapse, *aFocus, *aDelete, *aCut, *aPaste, *aSaveAs;
    QAction *aLabelsOver, *aLabelsBeside, *aDark;
    std::vector<QAction*> styleActs;

    Node* sel() const { return model.find(canvas->selId); }

    void select(const QString& id) { canvas->selId = id; refresh(); }

    QString startDir() const {
        if (!model.path.isEmpty()) return QFileInfo(model.path).absolutePath();
        return QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    }

    // ---- view settings
    void setFractal(Layout::Style st) {
        canvas->lay.style = st;
        styleActs[st]->setChecked(true);
        canvas->relayout();
        canvas->fitView();
        QSettings().setValue("view/fractal", int(st));
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

    // ---- navigation (follows on-screen order: lightest left, heaviest right)
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
        switch (d) {
        case Nav::Daughter:
            if (n->kids.empty()) model.status = QString("'%1' has no daughters").arg(n->name);
            else if (n->collapsed) model.status = QString("'%1' is collapsed — press C to expand").arg(n->name);
            else target = canvas->lay.ordered(n).back();
            break;
        case Nav::Right:
        case Nav::Left: {
            if (!n->parent) { model.status = "The root clade has no sisters"; break; }
            const auto sis = canvas->lay.ordered(n->parent);
            if (sis.size() < 2) { model.status = QString("'%1' has no sisters").arg(n->name); break; }
            const int count = int(sis.size());
            const int i = int(std::find(sis.begin(), sis.end(), n) - sis.begin());
            const int step = d == Nav::Right ? 1 : -1;
            target = sis[(i + step + count) % count];  // wraps around at either end
            break;
        }
        case Nav::Mother:
            if (n->parent) target = n->parent;
            else model.status = "Already at the root clade";
            break;
        }
        if (target) goTo(target);
        else refresh();
    }

    // ---- file handling
    void fileOpen() {
        QString p = QFileDialog::getOpenFileName(this, "Open cladogram", startDir(), FILE_FILTER);
        if (!p.isEmpty()) openPath(p);
    }

    void fileNew(bool sample = false) {
        QString p = QFileDialog::getSaveFileName(this, "New cladogram",
                                                 startDir() + "/cladogram.json", FILE_FILTER);
        if (p.isEmpty()) return;
        if (QFileInfo(p).suffix().isEmpty()) p += ".json";
        std::unique_ptr<Node> tree;
        if (sample) {
            tree = sampleTree();
        } else {
            bool ok;
            QString name = QInputDialog::getText(this, "New cladogram", "Name of the root clade:",
                                                 QLineEdit::Normal, "Root", &ok).trimmed();
            if (!ok) return;
            tree = makeNode(name.isEmpty() ? "Root" : name);
        }
        QString err = model.create(p, std::move(tree));
        if (!err.isEmpty()) {
            QMessageBox::warning(this, "Cannot create file", err);
            return;
        }
        afterSwitch();
    }

    void fileSaveAs() {
        if (!model.isOpen()) return;
        QString p = QFileDialog::getSaveFileName(this, "Save cladogram as", model.path, FILE_FILTER);
        if (p.isEmpty()) return;
        if (QFileInfo(p).suffix().isEmpty()) p += ".json";
        QString err = model.saveAs(p);
        if (!err.isEmpty()) {
            QMessageBox::warning(this, "Cannot save file", err);
            return;
        }
        afterSwitch(false);
    }

    // Called whenever a different file becomes the open one.
    void afterSwitch(bool resetView = true) {
        if (resetView) {
            canvas->selId.clear();
            canvas->cutId.clear();
        }
        setWindowTitle("TreeFern — " + model.path);
        addRecent(model.path);
        canvas->relayout(!resetView);
        if (resetView) canvas->fitView();
        refresh();
    }

    void addRecent(const QString& p) {
        QSettings s;
        QStringList r = s.value("recent").toStringList();
        r.removeAll(p);
        r.prepend(p);
        while (r.size() > MAX_RECENT) r.removeLast();
        s.setValue("recent", r);
        s.setValue("lastFile", p);
    }

    void rebuildRecentMenu() {
        recentMenu->clear();
        QStringList r = QSettings().value("recent").toStringList();
        r.erase(std::remove_if(r.begin(), r.end(), [](const QString& p) { return !QFile::exists(p); }),
                r.end());
        if (r.isEmpty()) {
            recentMenu->addAction("(none)")->setEnabled(false);
            return;
        }
        for (const QString& p : r)
            recentMenu->addAction(p, this, [this, p] { openPath(p); });
        recentMenu->addSeparator();
        recentMenu->addAction("Clear list", this, [] { QSettings().remove("recent"); });
    }

    // ---- UI state
    void refresh() {
        if (!model.find(canvas->cutId)) canvas->cutId.clear();
        Node* s = sel();
        if (!s) canvas->selId.clear();
        Node* cut = model.find(canvas->cutId);
        dock->setVisible(s);
        if (s) panel->setNode(s);
        for (auto a : {aAdd, aEdit, aFocus}) a->setEnabled(s);
        aCollapse->setEnabled(s && !s->kids.empty());
        aCollapse->setText(s && s->collapsed ? "Expand (C)" : "Collapse (C)");
        aDelete->setEnabled(s && s->parent);
        aCut->setEnabled(s && s->parent);
        aPaste->setEnabled(s && cut && !cut->contains(s));
        aSaveAs->setEnabled(model.isOpen());
        canvas->update();
        updateHud();
    }

    void updateHud() {
        QString cut;
        if (Node* c = model.find(canvas->cutId)) cut = QString("   ✂ cut: %1 (select target, V)").arg(c->name);
        hud->setText(QString("%1   zoom %2   spread %3°   shrink %4   nodes %5%6   |   %7")
                         .arg(Layout::styleName(canvas->lay.style))
                         .arg(fmtZoom(canvas->cam.zoom))
                         .arg(qRound(canvas->lay.spread * 180 / PI))
                         .arg(canvas->lay.shrink, 0, 'f', 2)
                         .arg(canvas->lay.pos.size())
                         .arg(cut, model.status));
    }

    // ---- clade actions
    // Dialogs run a nested event loop; an external reload may replace the
    // tree meanwhile, so always re-look-up nodes by id afterwards.
    void actAdd() {
        Node* n = sel();
        if (!n) return;
        QString id = n->id;
        bool ok;
        QString name = QInputDialog::getText(this, "New clade",
                                             QString("New clade under '%1':").arg(n->name),
                                             QLineEdit::Normal, {}, &ok).trimmed();
        if (!ok || name.isEmpty() || !(n = model.find(id))) return;
        Node* c = n->addChild(makeNode(name));
        n->collapsed = false;
        canvas->selId = c->id;
        model.commit();
    }

    // One dialog for both name (top) and info (bottom).
    void actEdit() {
        Node* n = sel();
        if (!n) return;
        const QString id = n->id;

        QDialog dlg(this);
        dlg.setWindowTitle("Edit clade");
        dlg.resize(560, 420);

        auto nameEdit = new QLineEdit(n->name);
        nameEdit->selectAll();
        auto infoEdit = new QPlainTextEdit(n->info);
        infoEdit->setPlaceholderText("(no info)");
        auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        auto hint = new QLabel("Enter in name or Ctrl+Enter anywhere: OK   ·   Esc: cancel");
        hint->setStyleSheet(QString("color:%1").arg(T.dim.name()));

        auto lay = new QVBoxLayout(&dlg);
        lay->addWidget(new QLabel("Name:"));
        lay->addWidget(nameEdit);
        lay->addSpacing(6);
        lay->addWidget(new QLabel("Info:"));
        lay->addWidget(infoEdit, 1);
        lay->addWidget(hint);
        lay->addWidget(buttons);

        QPushButton* ok = buttons->button(QDialogButtonBox::Ok);
        connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
        connect(nameEdit, &QLineEdit::textChanged, &dlg,
                [ok](const QString& t) { ok->setEnabled(!t.trimmed().isEmpty()); });
        connect(nameEdit, &QLineEdit::returnPressed, &dlg, [&] { if (ok->isEnabled()) dlg.accept(); });
        for (auto seq : {QKeySequence("Ctrl+Return"), QKeySequence("Ctrl+Enter")}) {
            auto sc = new QShortcut(seq, &dlg);
            connect(sc, &QShortcut::activated, &dlg, [&] { if (ok->isEnabled()) dlg.accept(); });
        }
        nameEdit->setFocus();

        if (dlg.exec() != QDialog::Accepted || !(n = model.find(id))) return;

        const QString newName = nameEdit->text().trimmed();
        const QString newInfo = infoEdit->toPlainText();
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
        QString parentId = n->parent ? n->parent->id : QString();
        QString err = model.removeSubtree(n);
        if (!err.isEmpty()) model.status = err;
        else canvas->selId = parentId;
        refresh();
    }
    void actCut() {
        Node* n = sel();
        if (!n) return;
        if (!n->parent) { model.status = "Cannot cut the root clade"; refresh(); return; }
        canvas->cutId = n->id;
        model.status = QString("Cut '%1' (+%2 descendants) — select a new parent and press V")
                           .arg(n->name).arg(n->countDesc());
        refresh();
    }
    void actPaste() {
        Node* target = sel();
        Node* cut = model.find(canvas->cutId);
        if (!target || !cut) return;
        QString name = cut->name, tname = target->name;
        QString err = model.moveSubtree(cut, target);
        if (err.isEmpty()) {
            canvas->selId = canvas->cutId;
            canvas->cutId.clear();
            model.status = QString("Moved '%1' under '%2'").arg(name, tname);
        } else {
            model.status = err;
        }
        refresh();
    }
};

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("TreeFern");
    QCoreApplication::setApplicationName("TreeFern");
    app.setStyle("Fusion");  // palette is applied by MainWindow (dark / light)

    MainWindow w;
    w.setWindowTitle("TreeFern");
    w.show();

    const QStringList args = app.arguments();
    if (args.size() > 1) {
        w.openOrCreate(args[1]);              // explicit file on the command line
    } else {
        QString last = QSettings().value("lastFile").toString();
        if (last.isEmpty() || !QFile::exists(last) || !w.openPath(last))
            if (!w.startupPrompt()) return 0;  // user cancelled: nothing to edit
    }
    return app.exec();
}

#include "main.moc"
