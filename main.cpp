// TreeFern — JSON-based live GUI cladogram editor
//
// File:   Ctrl+N new · Ctrl+O open · Ctrl+Shift+S save as · drop a .json file
// Mouse:  left click node/label = select · drag = pan · wheel = zoom (Shift = fast)
//         double-click = edit name & info · right-click = context menu
//         hover a node to see its label even when there is no room for it
// Keys:   A add child  E (or R) edit name & info  C collapse/expand  F focus
//         Del/Backspace delete  X / Ctrl+X cut subtree  V / Ctrl+V paste (graft)
//         Home reset view  Esc cancel cut / deselect
//         + / - fan spread   [ / ] branch shrink ratio
// Layout: at every fork children are ordered lightest -> heaviest, so the
//         heaviest clade is always the rightmost (clockwise) branch.
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
static const QColor BG(14, 17, 26), FG(225, 230, 240), DIM(130, 140, 160),
    ACCENT(255, 200, 80), CUT_COL(255, 90, 90);
static const QColor LABEL_BG(20, 24, 36, 190);        // translucent label boxes
static const QColor LABEL_BG_HOVER(28, 34, 50, 240);  // hovered / forced labels
constexpr double ROOT_LEN = 220.0;
constexpr double MIN_ZOOM = 1e-3, MAX_ZOOM = 1e12;
constexpr double ZOOM_STEP = 1.15, FAST_ZOOM_STEP = 2.0;
constexpr double LABEL_MIN_SCREEN_LEN = 35;
constexpr double LABEL_GAP = 2;    // minimum spacing between label boxes
constexpr double WRAP_COST = 0.45; // placement penalty per extra wrapped line
constexpr int MAX_LABELS = 400;
constexpr int MAX_RECENT = 10;
constexpr double PI = 3.14159265358979323846;

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

// ------------------------------------------------------- fractal layout ---
struct Geom { double x, y, len, angle; int depth; };
struct Edge { QPointF a, b; int depth; const Node* child; };

struct Layout {
    double spread = 120 * PI / 180;  // fan angle for children
    double shrink = 0.68;            // branch length ratio per generation
    QHash<const Node*, Geom> pos;
    std::vector<Edge> edges;

    void compute(const Node* root) {
        pos.clear();
        edges.clear();
        size_.clear();
        leaves_.clear();
        if (!root) return;
        weigh(root);
        pos[root] = {0, 0, ROOT_LEN, -PI / 2, 0};
        rec(root, 0, 0, -PI / 2, ROOT_LEN, 0);
    }

    // clade size: the node plus all of its descendants (collapsed or not)
    int weight(const Node* n) const { return size_.value(n, 1); }

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
        if (n->collapsed || n->kids.empty()) l = 1;
        size_[n] = s;
        leaves_[n] = l;
    }

    void rec(const Node* n, double x, double y, double ang, double len, int depth) {
        if (n->collapsed || n->kids.empty()) return;

        // Lightest first, heaviest last. Angles increase clockwise on screen,
        // i.e. towards the right-hand side of the branch direction, so the
        // heaviest clade always ends up as the rightmost branch.
        std::vector<const Node*> kids;
        for (auto& k : n->kids) kids.push_back(k.get());
        std::stable_sort(kids.begin(), kids.end(), [this](const Node* a, const Node* b) {
            return std::make_pair(size_.value(a), leaves_.value(a)) <
                   std::make_pair(size_.value(b), leaves_.value(b));
        });

        int total = 0;
        for (auto k : kids) total += leaves_.value(k);
        const bool many = kids.size() > 1;
        // wider subtrees get a bigger share of the fan; fan narrows with depth
        const double sp = many ? spread * std::pow(0.92, depth) : 0;
        double a = ang - sp / 2;
        for (const Node* c : kids) {
            double share = sp * leaves_.value(c) / total;
            double ca = many ? a + share / 2 : ang;
            a += share;
            double cx = x + std::cos(ca) * len, cy = y + std::sin(ca) * len;
            edges.push_back({{x, y}, {cx, cy}, depth, c});
            pos[c] = {cx, cy, len * shrink, ca, depth + 1};
            rec(c, cx, cy, ca, len * shrink, depth + 1);
        }
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
    return QColor::fromHsvF(((200 + d * 37) % 360) / 360.0, 0.55, std::min(1.0, 0.95 * bright));
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
    Layout lay;
    Camera cam;
    QString selId, cutId;

    explicit Canvas(Model* m) : model(m) {
        setFocusPolicy(Qt::StrongFocus);
        setMinimumSize(300, 200);
        setMouseTracking(true);  // for hover labels
        relayout();
    }

    void relayout() { lay.compute(model->root()); update(); }

    void focusOn(const Node* n) {
        if (!lay.pos.contains(n)) return;
        const Geom& g = lay.pos[n];
        cam.x = g.x;
        cam.y = g.y;
        cam.zoom = std::max(cam.zoom, std::min(MAX_ZOOM, 120 / g.len));
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
        dragBtn = Qt::NoButton;  // swallow the following release
        if (e->button() != Qt::LeftButton) return;
        if (const Node* hit = hitTest(e->position())) emit doubleClicked(hit->id);
    }
    void wheelEvent(QWheelEvent* e) override {
        bool fast = e->modifiers() & Qt::ShiftModifier;
        double notches = e->angleDelta().y() / 120.0;
        if (notches == 0 && fast) notches = e->angleDelta().x() / 120.0;  // Shift+wheel quirk
        if (notches != 0) {
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
    std::vector<std::pair<QRectF, QString>> labelHits;  // label boxes from the last paint

    double nodeRadius(double len) const { return std::clamp(len * 0.06 * cam.zoom + 2, 3.0, 18.0); }

    void updateHover(QPointF pos) {
        const Node* h = hitTest(pos);
        QString id = h ? h->id : QString();
        if (h) setCursor(Qt::PointingHandCursor);
        else unsetCursor();
        if (id != hoverId) { hoverId = id; update(); }
    }

    // Priority: a node disc under the cursor, then a label box (topmost
    // first), then the nearest node within a small tolerance.
    const Node* hitTest(QPointF p) const {
        const Node* best = nullptr;
        double bd = 1e18;
        bool exact = false;
        for (auto it = lay.pos.cbegin(); it != lay.pos.cend(); ++it) {
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
    p.fillRect(rect(), BG);
    labelHits.clear();
    if (!model->isOpen()) {
        p.setPen(DIM);
        p.drawText(rect(), Qt::AlignCenter, "No file open — use File ▸ Open or File ▸ New");
        return;
    }
    const QSizeF vs = size();
    const Node* sel = model->find(selId);
    const Node* cut = model->find(cutId);
    const Node* hov = model->find(hoverId);
    auto inCut = [&](const Node* n) { return cut && cut->contains(n); };

    // ---- edges (clipped to view)
    const QRectF view(-60, -60, vs.width() + 120, vs.height() + 120);
    for (const Edge& e : lay.edges) {
        QPointF a = cam.toScreen(e.a.x(), e.a.y(), vs), b = cam.toScreen(e.b.x(), e.b.y(), vs);
        if (!clipLine(view, a, b)) continue;
        int w = std::clamp(int(ROOT_LEN * 0.04 * std::pow(lay.shrink, e.depth) * cam.zoom), 1, 30);
        QColor c = inCut(e.child) ? CUT_COL.darker(140) : depthColor(e.depth, 0.7);
        p.setPen(QPen(c, w, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(a, b);
    }

    // ---- visible nodes
    struct Vis { const Node* n; QPointF s; Geom g; double r; };
    std::vector<Vis> nodes;
    for (auto it = lay.pos.cbegin(); it != lay.pos.cend(); ++it) {
        QPointF s = cam.toScreen(it->x, it->y, vs);
        if (s.x() < -100 || s.x() > vs.width() + 100 || s.y() < -100 || s.y() > vs.height() + 100)
            continue;
        nodes.push_back({it.key(), s, *it, nodeRadius(it->len)});
    }
    p.setPen(Qt::NoPen);
    for (auto& v : nodes) {
        p.setBrush(inCut(v.n) ? CUT_COL : depthColor(v.g.depth));
        p.drawEllipse(v.s, v.r, v.r);
    }
    p.setBrush(Qt::NoBrush);
    for (auto& v : nodes) {
        if (v.n == sel) { p.setPen(QPen(ACCENT, 2)); p.drawEllipse(v.s, v.r + 4, v.r + 4); }
        if (v.n == cut) { p.setPen(QPen(CUT_COL, 2, Qt::DashLine)); p.drawEllipse(v.s, v.r + 8, v.r + 8); }
    }

    // ---- labels: centred over their node; if that spot is taken, nudge
    //      (staying on top of the node) and/or wrap, never overlapping
    //      another label. Biggest clades claim space first.
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

    auto blocked = [&](const QRectF& r) {
        QRectF t = r.adjusted(-LABEL_GAP, -LABEL_GAP, LABEL_GAP, LABEL_GAP);
        for (const QRectF& q : placed)
            if (q.intersects(t)) return true;
        return false;
    };

    auto makeLabel = [&](const Vis& v, bool force) -> std::optional<Lbl> {
        QStringList words = v.n->name.split(' ', Qt::SkipEmptyParts);
        if (words.isEmpty()) words << "?";
        if (v.n == cut) words[0] = "✂ " + words[0];
        if (v.n->collapsed && !v.n->kids.empty()) words << QString("[+%1]").arg(v.n->countDesc());

        QFont f = font();
        f.setPixelSize(std::clamp(int(v.g.len * cam.zoom * 0.09), 11, 40));
        QFontMetricsF fm(f);
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
            if (!blocked(c.r)) return Lbl{c.r, opts[c.opt].join('\n'), f, v.n, false};
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
        p.setPen(isSel ? QPen(ACCENT, 1) : isHov ? QPen(DIM, 1) : QPen(Qt::NoPen));
        p.setBrush(isHov || l.forced ? LABEL_BG_HOVER : LABEL_BG);
        p.drawRoundedRect(l.box, 5, 5);
        p.setFont(l.font);
        p.setPen(isSel ? ACCENT : inCut(l.n) ? CUT_COL : FG);
        p.drawText(l.box.adjusted(padX, padY, -padX, -padY), Qt::AlignCenter, l.text);
        labelHits.push_back({l.box, l.n->id});
    }

    if (selId.isEmpty()) {
        p.setFont(font());
        p.setPen(DIM);
        p.drawText(10, 20, "Click a node to select · wheel zoom (Shift = fast) · drag pan · "
                           "double-click to edit · right-click for menu · X/V cut & graft");
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
        title->setStyleSheet("color:#ffc850");
        meta = new QLabel;
        meta->setWordWrap(true);
        meta->setTextInteractionFlags(Qt::TextSelectableByMouse);
        meta->setStyleSheet("color:#828ca0");
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
        up->setStyleSheet("text-align:left; color:#aac8ff");
        v->addWidget(up);
        v->addWidget(new QLabel("Info:"));
        info = new QTextBrowser;
        info->setPlaceholderText("(no info — press E, double-click the node, or click 'Edit')");
        v->addWidget(info, 1);

        connect(kids, &QListWidget::itemClicked, this,
                [this](QListWidgetItem* it) { emit selectRequested(it->data(Qt::UserRole).toString()); });
        connect(up, &QPushButton::clicked, this, [this] { emit selectRequested(upId); });
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
        mk("Reset view", {Qt::Key_Home}, [this] { canvas->cam.reset(); canvas->update(); updateHud(); });
        auto layoutKey = [this, &mk](QList<QKeySequence> k, auto fn) {
            mk("", k, [this, fn] { fn(canvas->lay); canvas->relayout(); updateHud(); });
        };
        layoutKey({Qt::Key_Plus, Qt::Key_Equal},
                  [](Layout& l) { l.spread = std::min(340 * PI / 180, l.spread + 10 * PI / 180); });
        layoutKey({Qt::Key_Minus},
                  [](Layout& l) { l.spread = std::max(20 * PI / 180, l.spread - 10 * PI / 180); });
        layoutKey({Qt::Key_BracketLeft}, [](Layout& l) { l.shrink = std::max(0.4, l.shrink - 0.02); });
        layoutKey({Qt::Key_BracketRight}, [](Layout& l) { l.shrink = std::min(0.9, l.shrink + 0.02); });

        QMenu* editMenu = menuBar()->addMenu("&Clade");
        editMenu->addActions({aAdd, aEdit, aCollapse, aFocus});
        editMenu->addSeparator();
        editMenu->addActions({aCut, aPaste});
        editMenu->addSeparator();
        editMenu->addAction(aDelete);

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
    Model model;
    Canvas* canvas;
    InfoPanel* panel;
    QDockWidget* dock;
    QLabel* hud;
    QMenu* recentMenu;
    QAction *aAdd, *aEdit, *aCollapse, *aFocus, *aDelete, *aCut, *aPaste, *aSaveAs;

    Node* sel() const { return model.find(canvas->selId); }

    void select(const QString& id) { canvas->selId = id; refresh(); }

    QString startDir() const {
        if (!model.path.isEmpty()) return QFileInfo(model.path).absolutePath();
        return QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
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
            canvas->cam.reset();
        }
        setWindowTitle("TreeFern — " + model.path);
        addRecent(model.path);
        canvas->relayout();
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
        hud->setText(QString("zoom %1   spread %2°   shrink %3   nodes %4%5   |   %6")
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
        hint->setStyleSheet("color:#828ca0");

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
    app.setStyle("Fusion");
    QPalette pal;
    pal.setColor(QPalette::Window, QColor(26, 31, 46));
    pal.setColor(QPalette::WindowText, FG);
    pal.setColor(QPalette::Base, QColor(16, 19, 30));
    pal.setColor(QPalette::Text, FG);
    pal.setColor(QPalette::Button, QColor(45, 54, 78));
    pal.setColor(QPalette::ButtonText, FG);
    pal.setColor(QPalette::Highlight, QColor(58, 92, 160));
    pal.setColor(QPalette::PlaceholderText, DIM);
    app.setPalette(pal);

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
