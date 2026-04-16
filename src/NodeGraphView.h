#pragma once
#include "TaskTree.h"
#include <QDockWidget>
#include <QGraphicsView>
#include <QGraphicsScene>
#include <QGraphicsItem>
#include <QWheelEvent>
#include <QHash>
#include <QList>
#include <QColor>
#include <QPen>
#include <QBrush>
#include <QFont>
#include <QPointF>
#include <QPolygonF>
#include <QPainter>
#include <cmath>

class EdgeItem;

// ─── NodeItem ─────────────────────────────────────────────────────────────────
class NodeItem : public QGraphicsItem
{
public:
    static constexpr qreal W = 160;
    static constexpr qreal H = 60;
    static constexpr qreal R = 6;

    explicit NodeItem(const TaskNode *node, QGraphicsItem *parent = nullptr)
        : QGraphicsItem(parent), m_node(node)
    {
        setFlag(QGraphicsItem::ItemIsMovable);
        setFlag(QGraphicsItem::ItemIsSelectable);
        setFlag(QGraphicsItem::ItemSendsGeometryChanges);
        setToolTip(node->description.left(200));
    }

    const TaskNode *node() const { return m_node; }

    void addEdge(EdgeItem *edge) {
        if (!m_edges.contains(edge)) m_edges.append(edge);
    }
    void removeEdge(EdgeItem *edge) { m_edges.removeAll(edge); }

    QPointF topCenter()    const { return mapToScene(QPointF(W/2, 0));   }
    QPointF bottomCenter() const { return mapToScene(QPointF(W/2, H));   }
    QPointF leftCenter()   const { return mapToScene(QPointF(0,   H/2)); }
    QPointF rightCenter()  const { return mapToScene(QPointF(W,   H/2)); }

    QRectF boundingRect() const override {
        return QRectF(-1, -1, W + 2, H + 2);
    }

    void paint(QPainter *painter, const QStyleOptionGraphicsItem *,
               QWidget *) override
    {
        painter->setRenderHint(QPainter::Antialiasing);
        QColor bg = statusColor();
        if (isSelected()) bg = bg.lighter(130);
        painter->setBrush(QBrush(bg));
        painter->setPen(QPen(bg.darker(150), 1.5));
        painter->drawRoundedRect(QRectF(0, 0, W, H), R, R);

        painter->setFont(QFont("monospace", 7));
        painter->setPen(Qt::white);
        painter->drawText(QRectF(4, 2, W-8, 14),
                          Qt::AlignLeft | Qt::AlignTop,
                          TaskNode::levelName(m_node->level));

        painter->setFont(QFont("sans-serif", 8, QFont::Bold));
        painter->setPen(Qt::white);
        QString title = m_node->title;
        if (title.length() > 22) title = title.left(19) + "...";
        painter->drawText(QRectF(4, 16, W-8, H-20),
                          Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap,
                          title);

        painter->setBrush(QBrush(statusDotColor()));
        painter->setPen(Qt::NoPen);
        painter->drawEllipse(QPointF(W-8, H-8), 4, 4);
    }

protected:
    // ── itemChange DEKLARATION ────────────────────────────────────────────────
    // Implementierung kommt NACH EdgeItem (wegen forward declaration)
    QVariant itemChange(GraphicsItemChange change,
                        const QVariant &value) override;

private:
    const TaskNode  *m_node;
    QList<EdgeItem*> m_edges;

    QColor statusColor() const {
        switch (m_node->status) {
            case TaskStatus::Pending: return QColor(0x5c,0x6b,0x7a);
            case TaskStatus::Running: return QColor(0xe3,0x74,0x00);
            case TaskStatus::Done:    return QColor(0x18,0x80,0x38);
            case TaskStatus::Failed:  return QColor(0xc5,0x22,0x1f);
            case TaskStatus::Blocked: return QColor(0x8b,0x00,0x8b);
        }
        return QColor(0x5c,0x6b,0x7a);
    }
    QColor statusDotColor() const {
        switch (m_node->status) {
            case TaskStatus::Pending: return Qt::lightGray;
            case TaskStatus::Running: return Qt::yellow;
            case TaskStatus::Done:    return Qt::green;
            case TaskStatus::Failed:  return Qt::red;
            case TaskStatus::Blocked: return QColor(0xff,0x80,0x00);
        }
        return Qt::lightGray;
    }
};

// ─── EdgeItem ─────────────────────────────────────────────────────────────────
class EdgeItem : public QGraphicsItem
{
public:
    enum class EdgeType { Vertical, Horizontal };

    EdgeItem(NodeItem *from, NodeItem *to, EdgeType type,
             QGraphicsItem *parent = nullptr)
        : QGraphicsItem(parent), m_from(from), m_to(to), m_type(type)
    {
        setZValue(-1);
        m_from->addEdge(this);
        m_to->addEdge(this);
        updateGeometry();
    }

    ~EdgeItem() override {
        if (m_from) m_from->removeEdge(this);
        if (m_to)   m_to->removeEdge(this);
    }

    void updateGeometry()
    {
        prepareGeometryChange();
        if (m_type == EdgeType::Vertical) {
            m_start = m_from->bottomCenter();
            m_end   = m_to->topCenter();
        } else {
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
        return QRectF(m_start, m_end).normalized().adjusted(-12,-12,12,12);
    }

    void paint(QPainter *painter, const QStyleOptionGraphicsItem *,
               QWidget *) override
    {
        painter->setRenderHint(QPainter::Antialiasing);
        QColor color = (m_type == EdgeType::Vertical)
                       ? QColor(0x33,0x33,0x33)
                       : QColor(0x1a,0x73,0xe8);
        QPen pen(color, 1.5);
        pen.setStyle(m_type == EdgeType::Horizontal ? Qt::DashLine : Qt::SolidLine);
        painter->setPen(pen);
        painter->drawLine(m_start, m_end);
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
        QPolygonF arrow;
        arrow << to
              << (to - dir*arrowSize + perp*(arrowSize*0.4))
              << (to - dir*arrowSize - perp*(arrowSize*0.4));
        painter->setBrush(QBrush(color));
        painter->setPen(Qt::NoPen);
        painter->drawPolygon(arrow);
    }
};

// ─── NodeItem::itemChange IMPLEMENTIERUNG ────────────────────────────────────
// Muss NACH EdgeItem stehen weil edge->updateGeometry() aufgerufen wird.
// EdgeItem ist jetzt vollständig definiert → kein "incomplete type" Fehler.
inline QVariant NodeItem::itemChange(GraphicsItemChange change,
                                      const QVariant &value)
{
    if (change == ItemPositionHasChanged) {
        for (EdgeItem *edge : m_edges)
            edge->updateGeometry();
    }
    return QGraphicsItem::itemChange(change, value);
}

// ─── ZoomableGraphicsView ─────────────────────────────────────────────────────
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
        setBackgroundBrush(QBrush(QColor(0xf8,0xf9,0xfa)));
    }
protected:
    void wheelEvent(QWheelEvent *event) override {
        qreal factor = (event->angleDelta().y() > 0) ? 1.15 : (1.0/1.15);
        qreal current = transform().m11();
        if ((factor > 1.0 && current < 4.0) || (factor < 1.0 && current > 0.1))
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

    void refresh(const TaskTree &tree)
    {
        m_scene->clear();
        m_nodeItems.clear();
        if (tree.isEmpty()) return;
        layoutNodes(tree);
        drawEdges(tree);
        m_scene->setSceneRect(
            m_scene->itemsBoundingRect().adjusted(-40,-40,40,40));
    }

signals:
    void nodeSelected(qint64 nodeId);

private:
    QGraphicsScene          *m_scene;
    ZoomableGraphicsView    *m_view;
    QHash<qint64, NodeItem*> m_nodeItems;

    void layoutNodes(const TaskTree &tree)
    {
        QHash<int,int> levelCount;
        tree.traverse([&](const TaskNode *node) {
            int col = node->level;
            int row = levelCount.value(col, 0);
            levelCount[col] = row + 1;
            auto *item = new NodeItem(node);
            item->setPos(col * (NodeItem::W + 60.0),
                         row * (NodeItem::H + 30.0));
            m_scene->addItem(item);
            m_nodeItems[node->id] = item;
        });

        connect(m_scene, &QGraphicsScene::selectionChanged, this, [this]() {
            const auto sel = m_scene->selectedItems();
            if (sel.isEmpty()) return;
            auto *item = dynamic_cast<NodeItem*>(sel.first());
            if (item) emit nodeSelected(item->node()->id);
        });
    }

    void drawEdges(const TaskTree &tree)
    {
        tree.traverse([&](const TaskNode *node) {
            NodeItem *from = m_nodeItems.value(node->id, nullptr);
            if (!from) return;
            for (const TaskNode *child : node->children) {
                NodeItem *to = m_nodeItems.value(child->id, nullptr);
                if (to) m_scene->addItem(
                    new EdgeItem(from, to, EdgeItem::EdgeType::Vertical));
            }
            for (qint64 depId : node->dependsOn) {
                NodeItem *to = m_nodeItems.value(depId, nullptr);
                if (to) m_scene->addItem(
                    new EdgeItem(from, to, EdgeItem::EdgeType::Horizontal));
            }
        });
    }
};
