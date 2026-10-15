// Claditor — JSON-based fractal cladogram editor (Qt rewrite)
//
// File:   Ctrl+N new · Ctrl+O open · Ctrl+Shift+S save as · drop a .json file
// Mouse:  left click node = select · drag = pan · wheel = zoom (Shift = fast)
//         double-click node = edit name & info · right-click node = context menu
// Keys:   A add child  E (or R) edit name & info  C collapse/expand  F focus
//         Del/Backspace delete  X / Ctrl+X cut subtree  V / Ctrl+V paste (graft)
//         Home reset view  Esc cancel cut / deselect
//         + / - fan spread   [ / ] branch shrink ratio
// The open file is autosaved on every change and watched for external edits.

#include <QtWidgets>
#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

// ------------------------------------------------------------- config ---
static const QColor BG(14, 17, 26), FG(225, 230, 240), DIM(130, 140, 160),
    ACCENT(255, 200, 80), LABEL_BG(20, 24, 36), CUT_COL(255, 90, 90);
constexpr double ROOT_LEN = 220.0;
constexpr double MIN_ZOOM = 1e-3, MAX_ZOOM = 1e12;
constexpr double ZOOM_STEP = 1.15, FAST_ZOOM_STEP = 2.0;
constexpr double LABEL_MIN_SCREEN_LEN = 35;
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
    int leafWeight() const {
        if (collapsed || kids.empty()) return 1;
        int w = 0;
        for (auto& k : kids) w += k->leafWeight();
        return w;
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
    "children": [
        {
            "children": [
                {
                    "children": [
                    ],
                    "collapsed": false,
                    "id": "0476bdad33",
                    "info": "Turtles are a clade of reptile distinguished by a bony composite shell over their bodies. As with all reptiles, turtles descend from swimming, underwater vertebrates - but in the case of modern survivng turtles, their ancestors actually went back into the water, and the modern land-dwelling turtles emerged again. ",
                    "name": "Turtles"
                },
                {
                    "children": [
                        {
                            "children": [
                                {
                                    "children": [
                                        {
                                            "children": [
                                            ],
                                            "collapsed": false,
                                            "id": "4d317bf30e",
                                            "info": "Distinct for their bony septum and cold-temperature tolerance.",
                                            "name": "Alligators"
                                        },
                                        {
                                            "children": [
                                            ],
                                            "collapsed": false,
                                            "id": "f22d978662",
                                            "info": "A clade unique for the bony ridge between the eyes and needle-like nose.",
                                            "name": "Caimans"
                                        }
                                    ],
                                    "collapsed": false,
                                    "id": "d2f8aaf3ed",
                                    "info": "A clade distinct for its broad snout, hidden lower teeth, and freshwater environment.",
                                    "name": "Alligatoridae"
                                },
                                {
                                    "children": [
                                    ],
                                    "collapsed": false,
                                    "id": "4b200b4698",
                                    "info": "The clade of crocodilian most amenable to salt water thanks to specialized exchange glands as well as an interlocking and exposed fourth-tooth.",
                                    "name": "Crocodiles"
                                }
                            ],
                            "collapsed": false,
                            "id": "793b7b06c4",
                            "info": "A clade of large, predatory, semi-aquatic reptiles. These large species are covered in bony armor-plates and have long, flat tails and webbed feet.",
                            "name": "Crocodilians"
                        },
                        {
                            "children": [
                                {
                                    "children": [
                                    ],
                                    "collapsed": false,
                                    "id": "1b06f85448",
                                    "info": "The only living clade of flyuing reptiles, birds posses a characteristic wing with an elongated upper arm - even in flightless species. They also lack the tails of other theropods.",
                                    "name": "Birds"
                                }
                            ],
                            "collapsed": false,
                            "id": "159a566c07",
                            "info": "The clade containing the modern birds, and also containing the most famous extinct animals in the world thanks to books, television, and movies. Unique features of this clade include an open hip socket and fused vertebrae in the lower back to accomodate an upright posture.",
                            "name": "Dinosaurs"
                        }
                    ],
                    "collapsed": false,
                    "id": "c720abd7ba",
                    "info": "The clade which contains modern birds and crocodilians - along with all extinct dinosaurs and pterosaurs. Diagnostic of this clade are the fenestrae behind the eyes, as well as the unique four-chambered heart design.",
                    "name": "Archosauria"
                }
            ],
            "collapsed": false,
            "id": "6f3c4b3457",
            "info": "This is the least diverse of the two basal clades of reptile, and was discovered through molecular evidence from genome sequencing, challenging the traditional belief that turtles were more closely-related to snakes than to birds. Subsequent anatomical studies have identified the presence of a saggital crest on the supraoccipital bone and a lack of the entepicondylar foramen on the humerus as diagnostic of this clade.",
            "name": "Archelosauria"
        },
        {
            "children": [
                {
                    "children": [
                        {
                            "children": [
                            ],
                            "collapsed": false,
                            "id": "4ab9c4c819",
                            "info": "Snakes are a legless clade of lizard. They possess a highly mobile, kinetic skull; they lack moveable eyelids; and, they accomidate their paired organs sequentially in their long, slender bodies.",
                            "name": "Snakes"
                        }
                    ],
                    "collapsed": false,
                    "id": "afd5b9d7b4",
                    "info": "The most diverse clade of lepidosaur, featuring a common \"lizard-like\" body plan and its derivatives. Unique for their paired hemipenes in male reproduction, as well as skull kinesis and skin shedding.",
                    "name": "Lizards (Squamata)"
                },
                {
                    "children": [
                    ],
                    "collapsed": false,
                    "id": "14513769c6",
                    "info": "The sole surviving species of the ancient clade Rhynchocephalia, unique for their posesesion of the ancestral form of the diapsid skull, keeping intact both the upper and lower temporal bars.",
                    "name": "Tuatara"
                }
            ],
            "collapsed": false,
            "id": "0567bf4d80",
            "info": "The most diverse clade of reptiles, containing the tuatara, and all lizards.",
            "name": "Lepidosaurs"
        }
    ],
    "collapsed": false,
    "id": "1bc47f540e",
    "info": "Reptiles are an important clade of land animal, sister to modern mammals. Prior to the K-Pg Boundary Event 65 million years ago, the clade had significantly more diversity.",
    "name": "Reptiles"
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
        if (!root) return;
        pos[root] = {0, 0, ROOT_LEN, -PI / 2, 0};
        rec(root, 0, 0, -PI / 2, ROOT_LEN, 0);
    }

private:
    void rec(const Node* n, double x, double y, double ang, double len, int depth) {
        if (n->collapsed || n->kids.empty()) return;
        std::vector<int> w;
        int total = 0;
        for (auto& k : n->kids) { w.push_back(k->leafWeight()); total += w.back(); }
        bool many = n->kids.size() > 1;
        // wider subtrees get a bigger share of the fan; fan narrows with depth
        double sp = many ? spread * std::pow(0.92, depth) : 0;
        double a = ang - sp / 2;
        for (size_t i = 0; i < n->kids.size(); ++i) {
            double share = sp * w[i] / total;
            double ca = many ? a + share / 2 : ang;
            a += share;
            double cx = x + std::cos(ca) * len, cy = y + std::sin(ca) * len;
            const Node* c = n->kids[i].get();
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

// Box of size w*h whose node-facing side touches (px, py).
static QRectF anchoredRect(double w, double h, double px, double py, double ux, double uy) {
    double l = ux > 0.35 ? px : ux < -0.35 ? px - w : px - w / 2;
    double t = uy > 0.35 ? py : uy < -0.35 ? py - h : py - h / 2;
    return QRectF(std::round(l), std::round(t), w, h);
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
        if (dragBtn == Qt::NoButton) return;
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
        if (moved || btn != e->button()) return;
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
            emit viewChanged();
        }
    }

private:
    Model* model;
    QPointF pressPos, lastPos;
    Qt::MouseButton dragBtn = Qt::NoButton;
    bool moved = false;

    double nodeRadius(double len) const { return std::clamp(len * 0.06 * cam.zoom + 2, 3.0, 18.0); }

    const Node* hitTest(QPointF p) const {
        const Node* best = nullptr;
        double bd = 1e18;
        for (auto it = lay.pos.cbegin(); it != lay.pos.cend(); ++it) {
            QPointF s = cam.toScreen(it->x, it->y, size());
            double d = QLineF(s, p).length();
            if (d <= nodeRadius(it->len) + 5 && d < bd) { best = it.key(); bd = d; }
        }
        return best;
    }
};

void Canvas::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), BG);
    if (!model->isOpen()) {
        p.setPen(DIM);
        p.drawText(rect(), Qt::AlignCenter, "No file open — use File ▸ Open or File ▸ New");
        return;
    }
    const QSizeF vs = size();
    const Node* sel = model->find(selId);
    const Node* cut = model->find(cutId);
    auto inCut = [&](const Node* n) { return cut && cut->contains(n); };

    // ---- edges (clipped to view, kept as label obstacles)
    struct Seg { QPointF a, b; double pad; QRectF box; };
    std::vector<Seg> segs;
    const QRectF view(-60, -60, vs.width() + 120, vs.height() + 120);
    for (const Edge& e : lay.edges) {
        QPointF a = cam.toScreen(e.a.x(), e.a.y(), vs), b = cam.toScreen(e.b.x(), e.b.y(), vs);
        if (!clipLine(view, a, b)) continue;
        int w = std::clamp(int(ROOT_LEN * 0.04 * std::pow(lay.shrink, e.depth) * cam.zoom), 1, 30);
        QColor c = inCut(e.child) ? CUT_COL.darker(140) : depthColor(e.depth, 0.7);
        p.setPen(QPen(c, w, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(a, b);
        double pad = w / 2 + 1;
        segs.push_back({a, b, pad, QRectF(a, b).normalized().adjusted(-pad, -pad, pad, pad)});
    }

    // ---- visible nodes (also obstacles)
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
        p.setBrush(inCut(v.n) ? CUT_COL : depthColor(v.g.depth));
        p.drawEllipse(v.s, v.r, v.r);
    }
    p.setBrush(Qt::NoBrush);
    for (auto& v : nodes) {
        if (v.n == sel) { p.setPen(QPen(ACCENT, 2)); p.drawEllipse(v.s, v.r + 4, v.r + 4); }
        if (v.n == cut) { p.setPen(QPen(CUT_COL, 2, Qt::DashLine)); p.drawEllipse(v.s, v.r + 8, v.r + 8); }
    }

    // ---- labels: greedy placement with collision avoidance
    std::vector<Vis> cands;
    for (auto& v : nodes)
        if (v.g.len * cam.zoom > LABEL_MIN_SCREEN_LEN || v.n == sel) cands.push_back(v);
    std::sort(cands.begin(), cands.end(), [&](const Vis& a, const Vis& b) {
        return std::make_tuple(a.n != sel, a.g.depth, -a.g.len) <
               std::make_tuple(b.n != sel, b.g.depth, -b.g.len);
    });

    static const double OFFS[] = {0, .45, -.45, .9, -.9, 1.4, -1.4, 2.0, -2.0, PI};
    std::vector<QRectF> placed;
    const double padX = 5, padY = 2;

    auto blocked = [&](const QRectF& t, int own) {
        for (auto& r : placed) if (r.intersects(t)) return true;
        for (size_t i = 0; i < nodeRects.size(); ++i)
            if (int(i) != own && nodeRects[i].intersects(t)) return true;
        for (auto& s : segs) {
            if (!s.box.intersects(t)) continue;
            QPointF a = s.a, b = s.b;
            if (clipLine(t.adjusted(-s.pad, -s.pad, s.pad, s.pad), a, b)) return true;
        }
        return false;
    };

    for (auto& v : cands) {
        if (int(placed.size()) >= MAX_LABELS) break;
        QString text = v.n->name;
        if (v.n->collapsed && !v.n->kids.empty())
            text += QString("  [+%1]").arg(v.n->countDesc());
        if (v.n == cut) text = "✂ " + text;
        QFont f = font();
        f.setPixelSize(std::clamp(int(v.g.len * cam.zoom * 0.09), 11, 40));
        QFontMetricsF fm(f);
        double w = fm.horizontalAdvance(text) + 2 * padX, h = fm.height() + 2 * padY;
        double gap = v.r + 6;
        bool isSel = v.n == sel;

        std::optional<QRectF> chosen;
        for (double off : OFFS) {
            double ang = v.g.angle + off, ux = std::cos(ang), uy = std::sin(ang);
            QRectF r = anchoredRect(w, h, v.s.x() + ux * gap, v.s.y() + uy * gap, ux, uy);
            if (!blocked(r.adjusted(-2, -1, 2, 1), v.own)) { chosen = r; break; }
        }
        if (!chosen) {
            if (!isSel) continue;  // no room: hidden until zoomed in further
            double ux = std::cos(v.g.angle), uy = std::sin(v.g.angle);
            chosen = anchoredRect(w, h, v.s.x() + ux * gap, v.s.y() + uy * gap, ux, uy);
        }
        placed.push_back(chosen->adjusted(-2, -1, 2, 1));

        p.setPen(isSel ? QPen(ACCENT, 1) : Qt::NoPen);
        p.setBrush(LABEL_BG);
        p.drawRoundedRect(*chosen, 5, 5);
        p.setFont(f);
        p.setPen(isSel ? ACCENT : inCut(v.n) ? CUT_COL : FG);
        p.drawText(chosen->adjusted(padX, padY, -padX, -padY), Qt::AlignLeft | Qt::AlignVCenter, text);
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
        box.setWindowTitle("Claditor");
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
        setWindowTitle("Claditor — " + model.path);
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
    QCoreApplication::setOrganizationName("Claditor");
    QCoreApplication::setApplicationName("Claditor");
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
    w.setWindowTitle("Claditor");
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
