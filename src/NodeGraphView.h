#pragma once
// ─── NodeGraphView ────────────────────────────────────────────────────────────
// Grafische Darstellung des TaskTree als gerichteter Graph.
//
// Darstellung:
//   Nodes     — Rechtecke mit Titel + Level + Status-Farbe
//   Vertikal  — Eltern→Kind Verbindungen (schwarz, oben/unten)
//   Horizontal— dependsOn Verbindungen (blau, seitlich)
//
// Struktur:
//   QDockWidget
//     └── QGraphicsView (zoombar, scrollbar)
//           └── QGraphicsScene
//                 ├── NodeItem (QGraphicsRectItem + QGraphicsTextItem)
//                 └── EdgeItem (QGraphicsLineItem mit Pfeilspitze)
//
// Layout-Algorithmus:
//   Vereinfachtes hierarchisches Layout:
//   - X-Position: nach Ebene (level) → Spalten
//   - Y-Position: nach insertionIdx innerhalb der Ebene → Zeilen
//   Kein vollständiger Graphen-Layout-Algorithmus (zu komplex für MVP).
//   Der User kann Nodes per Drag&Drop verschieben.
//
// Interaktion:
//   - Mausrad: Zoom
//   - Linksklick + Drag: Szene verschieben
//   - Doppelklick auf Node: selektiert Node (Signal nodeSelected)
//   - Drag Node: Node verschieben (Position wird nicht gespeichert)

#include "TaskTree.h"
#include <QDockWidget>
#include <QGraphicsView>
#include <QGraphicsScene>
#include <QGraphicsRectItem>
#include <QGraphicsLineItem>
#include <QGraphicsTextItem>
#include <QGraphicsItemGroup>
#include <QWheelEvent>
#include <QHash>
#include <QColor>
#include <QPen>
#include <QBrush>
#include <QFont>
#include <QPointF>
#include <QRectF>
#include <cmath>

// ─── NodeItem ─────────────────────────────────────────────────────────────────
// Ein Node als QGraphicsItem — Rechteck + Titeltext + Level-Badge.
// Moveable: User kann ihn per Drag verschieben.
class NodeItem : public QGraphicsItem
{
public:
    static constexpr qreal W = 160;   // Breite
    static constexpr qreal H = 60;    // Höhe
    static constexpr qreal R = 6;     // Ecken-Radius

    explicit NodeItem(const TaskNode *node, QGraphicsItem *parent = nullptr)
        : QGraphicsItem(parent), m_node(node)
    {
        setFlag(QGraphicsItem::ItemIsMovable);
        setFlag(QGraphicsItem::ItemIsSelectable);
        setFlag(QGraphicsItem::ItemSendsGeometryChanges);
        setToolTip(node->description.left(200));
    }

    const TaskNode *node() const { return m_node; }

    // Verbindungspunkte für Kanten
    QPointF topCenter()    const { return mapToScene(QPointF(W/2, 0));    }
    QPointF bottomCenter() const { return mapToScene(QPointF(W/2, H));    }
    QPointF leftCenter()   const { return mapToScene(QPointF(0,   H/2));  }
    QPointF rightCenter()  const { return mapToScene(QPointF(W,   H/2)); }

    QRectF boundingRect() const override {
        return QRectF(-1, -1, W + 2, H + 2);
    }

    void paint(QPainter *painter, const QStyleOptionGraphicsItem *,
               QWidget *) override
    {
        painter->setRenderHint(QPainter::Antialiasing);

        // Hintergrundfarbe nach Status
        QColor bg = statusColor();
        if (isSelected()) bg = bg.lighter(130);

        painter->setBrush(QBrush(bg));
        painter->setPen(QPen(bg.darker(150), 1.5));
        painter->drawRoundedRect(QRectF(0, 0, W, H), R, R);

        // Level-Badge (oben links)
        QString levelBadge = TaskNode::levelName(m_node->level);
        painter->setFont(QFont("monospace", 7));
        painter->setPen(Qt::white);
        painter->drawText(QRectF(4, 2, W - 8, 14),
                          Qt::AlignLeft | Qt::AlignTop,
                          levelBadge);

        // Titel (zentriert, fett)
        painter->setFont(QFont("sans-serif", 8, QFont::Bold));
        painter->setPen(Qt::white);
        // Titel kürzen wenn zu lang
        QString title = m_node->title;
        if (title.length() > 22) title = title.left(19) + "...";
        painter->drawText(QRectF(4, 16, W - 8, H - 20),
                          Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap,
                          title);

        // Status-Punkt (unten rechts)
        painter->setBrush(QBrush(statusDotColor()));
        painter->setPen(Qt::NoPen);
        painter->drawEllipse(QPointF(W - 8, H - 8), 4, 4);
    }

private:
    const TaskNode *m_node;

    QColor statusColor() const {
        switch (m_node->status) {
            case TaskStatus::Pending: return QColor(0x5c, 0x6b, 0x7a);  // grau-blau
            case TaskStatus::Running: return QColor(0xe3, 0x74, 0x00);  // orange
            case TaskStatus::Done:    return QColor(0x18, 0x80, 0x38);  // grün
            case TaskStatus::Failed:  return QColor(0xc5, 0x22, 0x1f);  // rot
            case TaskStatus::Blocked: return QColor(0x8b, 0x00, 0x8b);  // lila
        }
        return QColor(0x5c, 0x6b, 0x7a);
    }

    QColor statusDotColor() const {
        switch (m_node->status) {
            case TaskStatus::Pending: return Qt::lightGray;
            case TaskStatus::Running: return Qt::yellow;
            case TaskStatus::Done:    return Qt::green;
            case TaskStatus::Failed:  return Qt::red;
            case TaskStatus::Blocked: return QColor(0xff, 0x80, 0x00);
        }
        return Qt::lightGray;
    }
};

// ─── EdgeItem ─────────────────────────────────────────────────────────────────
// Eine Kante zwischen zwei NodeItems mit Pfeilspitze.
// Vertikal (Eltern→Kind): schwarz, oben/unten
// Horizontal (dependsOn): blau, links/rechts
class EdgeItem : public QGraphicsItem
{
public:
    enum class EdgeType { Vertical, Horizontal };

    EdgeItem(NodeItem *from, NodeItem *to, EdgeType type,
             QGraphicsItem *parent = nullptr)
        : QGraphicsItem(parent), m_from(from), m_to(to), m_type(type)
    {
        setZValue(-1);  // hinter den Nodes
        updateGeometry();
    }

    void updateGeometry()
    {
        prepareGeometryChange();

        if (m_type == EdgeType::Vertical) {
            // Oben/unten verbinden
            m_start = m_from->bottomCenter();
            m_end   = m_to->topCenter();
        } else {
            // Links/rechts verbinden
            // Von: rechts des from-Nodes
            // Zu:  links des to-Nodes
            // Falls to links von from: umgekehrt
            QPointF fc = m_from->scenePos() + QPointF(NodeItem::W/2, NodeItem::H/2);
            QPointF tc = m_to->scenePos()   + QPointF(NodeItem::W/2, NodeItem::H/2);

            if (tc.x() >= fc.x()) {
                m_start = m_from->rightCenter();
                m_end   = m_to->leftCenter();
            } else {
                m_start = m_from->leftCenter();
                m_end   = m_to->rightCenter();
            }
        }
    }

    QRectF boundingRect() const override {
        return QRectF(m_start, m_end).normalized().adjusted(-10, -10, 10, 10);
    }

    void paint(QPainter *painter, const QStyleOptionGraphicsItem *,
               QWidget *) override
    {
        painter->setRenderHint(QPainter::Antialiasing);

        QColor color = (m_type == EdgeType::Vertical)
                       ? QColor(0x33, 0x33, 0x33)   // schwarz
                       : QColor(0x1a, 0x73, 0xe8);  // blau

        QPen pen(color, 1.5);
        pen.setStyle(m_type == EdgeType::Horizontal
                     ? Qt::DashLine : Qt::SolidLine);
        painter->setPen(pen);
        painter->drawLine(m_start, m_end);

        // Pfeilspitze am Ende
        drawArrow(painter, m_start, m_end, color);
    }

private:
    NodeItem *m_from;
    NodeItem *m_to;
    EdgeType  m_type;
    QPointF   m_start;
    QPointF   m_end;

    void drawArrow(QPainter *painter, QPointF from, QPointF to,
                   QColor color) const
    {
        static constexpr qreal arrowSize = 8.0;

        QPointF dir = to - from;
        qreal len = std::sqrt(dir.x()*dir.x() + dir.y()*dir.y());
        if (len < 1.0) return;
        dir /= len;

        QPointF perp(-dir.y(), dir.x());
        QPointF tip  = to;
        QPointF base = to - dir * arrowSize;
        QPointF left = base + perp * (arrowSize * 0.4);
        QPointF right= base - perp * (arrowSize * 0.4);

        QPolygonF arrow;
        arrow << tip << left << right;

        painter->setBrush(QBrush(color));
        painter->setPen(Qt::NoPen);
        painter->drawPolygon(arrow);
    }
};

// ─── GraphicsView mit Zoom ────────────────────────────────────────────────────
class ZoomableGraphicsView : public QGraphicsView
{
    Q_OBJECT
public:
    explicit ZoomableGraphicsView(QGraphicsScene *scene, QWidget *parent = nullptr)
        : QGraphicsView(scene, parent)
    {
        setRenderHint(QPainter::Antialiasing);
        setDragMode(QGraphicsView::ScrollHandDrag);
        setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
        setBackgroundBrush(QBrush(QColor(0xf8, 0xf9, 0xfa)));
    }

protected:
    void wheelEvent(QWheelEvent *event) override {
        qreal factor = (event->angleDelta().y() > 0) ? 1.15 : (1.0 / 1.15);
        // Zoom begrenzen
        qreal currentScale = transform().m11();
        if ((factor > 1.0 && currentScale < 4.0) ||
            (factor < 1.0 && currentScale > 0.1))
            scale(factor, factor);
    }
};

// ─── NodeGraphView ────────────────────────────────────────────────────────────
class NodeGraphView : public QDockWidget
{
    Q_OBJECT

public:
    explicit NodeGraphView(QWidget *parent = nullptr)
        : QDockWidget("🕸 Node-Graph", parent)
    {
        setObjectName("nodeGraphView");
        setFeatures(QDockWidget::DockWidgetClosable |
                    QDockWidget::DockWidgetMovable  |
                    QDockWidget::DockWidgetFloatable);

        m_scene = new QGraphicsScene(this);
        m_view  = new ZoomableGraphicsView(m_scene, this);
        setWidget(m_view);
        setMinimumSize(400, 300);
    }

    // ── Tree neu zeichnen ─────────────────────────────────────────────────────
    void refresh(const TaskTree &tree)
    {
        m_scene->clear();
        m_nodeItems.clear();

        if (tree.isEmpty()) return;

        // Schritt 1: NodeItems erstellen + positionieren
        layoutNodes(tree);

        // Schritt 2: Kanten zeichnen
        drawEdges(tree);

        // Szene-Bounding-Box anpassen
        m_scene->setSceneRect(m_scene->itemsBoundingRect().adjusted(-40, -40, 40, 40));
    }

signals:
    void nodeSelected(qint64 nodeId);

private:
    QGraphicsScene              *m_scene;
    ZoomableGraphicsView        *m_view;
    QHash<qint64, NodeItem*>     m_nodeItems;

    // ── Layout: hierarchisch nach Level ──────────────────────────────────────
    // Spalte X = level * (NodeItem::W + 40)
    // Zeile  Y = Position innerhalb der Ebene * (NodeItem::H + 30)
    //
    // Einfaches Layout — kein Baum-Drawing-Algorithmus.
    // Nodes einer Ebene werden untereinander gestapelt.
    // User kann per Drag verschieben.
    void layoutNodes(const TaskTree &tree)
    {
        // Zähler pro Level für Y-Positionierung
        QHash<int, int> levelCount;

        tree.traverse([&](const TaskNode *node) {
            int col = node->level;
            int row = levelCount.value(col, 0);
            levelCount[col] = row + 1;

            qreal x = col * (NodeItem::W + 60.0);
            qreal y = row * (NodeItem::H + 30.0);

            auto *item = new NodeItem(node);
            item->setPos(x, y);
            m_scene->addItem(item);
            m_nodeItems[node->id] = item;

            // Doppelklick → Signal
            // QGraphicsItem hat kein eigenes Signal-System →
            // wir nutzen QGraphicsScene::selectionChanged
        });

        // Selektion → nodeSelected Signal
        connect(m_scene, &QGraphicsScene::selectionChanged, this, [this]() {
            const auto selected = m_scene->selectedItems();
            if (selected.isEmpty()) return;
            auto *item = dynamic_cast<NodeItem*>(selected.first());
            if (item) emit nodeSelected(item->node()->id);
        });
    }

    // ── Kanten zeichnen ───────────────────────────────────────────────────────
    void drawEdges(const TaskTree &tree)
    {
        tree.traverse([&](const TaskNode *node) {
            NodeItem *fromItem = m_nodeItems.value(node->id, nullptr);
            if (!fromItem) return;

            // Vertikale Kanten: Eltern → Kinder
            for (const TaskNode *child : node->children) {
                NodeItem *toItem = m_nodeItems.value(child->id, nullptr);
                if (!toItem) continue;
                auto *edge = new EdgeItem(fromItem, toItem,
                                          EdgeItem::EdgeType::Vertical);
                m_scene->addItem(edge);
            }

            // Horizontale Kanten: dependsOn
            for (qint64 depId : node->dependsOn) {
                NodeItem *toItem = m_nodeItems.value(depId, nullptr);
                if (!toItem) continue;
                auto *edge = new EdgeItem(fromItem, toItem,
                                          EdgeItem::EdgeType::Horizontal);
                m_scene->addItem(edge);
            }
        });
    }
};
