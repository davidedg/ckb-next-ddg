#include "hwlightwidget.h"
#include "colorbutton.h"
#include "keywidget.h"
#include "k95pledmap.h"

#include <QApplication>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QHelpEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QToolTip>
#include <QToolButton>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <new>

extern "C" {
#include "../daemon/cape_names.h"
}

namespace {
const int KindRole = Qt::UserRole + 1, EnabledRole = Qt::UserRole + 2;
const uint32_t MaxPoints = CAPE_XML_CHILDREN_MAX - 1u;

int physicalLayout(const QString& name){
    if(name == "ansi") return CAPE_PROFILE_LAYOUT_ANSI;
    if(name == "iso") return CAPE_PROFILE_LAYOUT_ISO;
    if(name == "abnt") return CAPE_PROFILE_LAYOUT_ABNT;
    if(name == "jis") return CAPE_PROFILE_LAYOUT_JIS;
    if(name == "dubeolsik") return CAPE_PROFILE_LAYOUT_KS;
    return 0;
}
KeyMap lightingMap(const KeyMap& labels, int layout){
    const auto region = labels.layout();
    const int shape = KeyMap::isJP(region) ? CAPE_PROFILE_LAYOUT_JIS : KeyMap::isPTBR(region) ? CAPE_PROFILE_LAYOUT_ABNT :
        KeyMap::isISO(region) ? CAPE_PROFILE_LAYOUT_ISO : CAPE_PROFILE_LAYOUT_ANSI;
    if(!layout || layout == shape) return labels;
    const auto canonical = layout == CAPE_PROFILE_LAYOUT_ISO ? KeyMap::GB : layout == CAPE_PROFILE_LAYOUT_ABNT ? KeyMap::PT_BR :
                           layout == CAPE_PROFILE_LAYOUT_JIS ? KeyMap::JP : KeyMap::US;
    KeyMap result = layout == CAPE_PROFILE_LAYOUT_KS && shape == CAPE_PROFILE_LAYOUT_ANSI ? labels : KeyMap(KeyMap::K95P,canonical);
    if(layout == CAPE_PROFILE_LAYOUT_KS){
        // KS adds Hanja/Hangul on either side of the shorter spacebar. The other keys have ANSI geometry.
        const KeyMap jp(KeyMap::K95P,KeyMap::JP);
        result["lang2"] = jp.key("muhenkan"); result["lang2"].name = "lang2"; result["lang2"]._friendlyName = "Hanja";
        result["lang1"] = jp.key("henkan"); result["lang1"].name = "lang1"; result["lang1"]._friendlyName = "Hangul / English";
        result["space"] = jp.key("space");
    }
    return result;
}

QString text(const cape_profile_text& value){ return QString::fromUtf8(value.p, int(value.len)); }
void setText(cape_profile_text& value, const QByteArray& bytes){
    char* p = static_cast<char*>(std::malloc(size_t(bytes.size()) + 1));
    if(!p) throw std::bad_alloc();
    std::memcpy(p, bytes.constData(), size_t(bytes.size()) + 1);
    std::free(value.p);
    value.p = p;
    value.len = uint32_t(bytes.size());
}
QString kindName(int kind){
    static const char* const names[] = {QT_TRANSLATE_NOOP("HwLightWidget", "Static colour"),
        QT_TRANSLATE_NOOP("HwLightWidget", "Solid"), QT_TRANSLATE_NOOP("HwLightWidget", "Gradient"),
        QT_TRANSLATE_NOOP("HwLightWidget", "Wave"), QT_TRANSLATE_NOOP("HwLightWidget", "Ripple"),
        QT_TRANSLATE_NOOP("HwLightWidget", "Colour pulse"), QT_TRANSLATE_NOOP("HwLightWidget", "Colour shift"),
        QT_TRANSLATE_NOOP("HwLightWidget", "Two-colour wave"), QT_TRANSLATE_NOOP("HwLightWidget", "Rain"),
        QT_TRANSLATE_NOOP("HwLightWidget", "Rainbow wave"), QT_TRANSLATE_NOOP("HwLightWidget", "Spiral rainbow"),
        QT_TRANSLATE_NOOP("HwLightWidget", "Type lighting"), QT_TRANSLATE_NOOP("HwLightWidget", "Visor")};
    return kind >= 0 && kind < CAPE_PROFILE_EFFECTS ? QCoreApplication::translate("HwLightWidget", names[kind]) : QString();
}
QColor colour(const cape_profile_colour& c){
    return QColor::fromRgbF(c.c[0], c.c[1], c.c[2], c.has_alpha ? c.c[3] : 1.);
}
void setRgb(cape_profile_colour& c, const QColor& rgb){
    c.c[0] = rgb.redF(); c.c[1] = rgb.greenF(); c.c[2] = rgb.blueF();
}

void newId(cape_profile_uuid& id){
    const QByteArray bytes = QUuid::createUuid().toRfc4122();
    std::memcpy(id.b, bytes.constData(), sizeof(id.b));
}
QString defaultName(int kind){
    static const char* const names[] = {"Static colour", "Solid", "Gradient", "Wave", "Ripple", "Colour pulse",
        "Colour shift", "Two-colour wave", "Rain", "Rainbow wave", "Spiral rainbow", "Type lighting", "Visor"};
    return QString::fromLatin1(names[kind]);
}
HwLighting::Model newLayer(int kind){
    HwLighting::Model result;
    auto& m = result.data(); m.known = 1;
    m.layers = static_cast<cape_profile_layer*>(std::calloc(1, sizeof(cape_profile_layer)));
    if(!m.layers) throw std::bad_alloc();
    m.count = 1;
    auto& l = m.layers[0]; auto& e = l.effect;
    newId(l.id); newId(e.id);
    l.enabled = 1; l.option = CAPE_PROFILE_OPTION_ALL; l.start_with_profile = 1; l.stop_after_times = -1;
    e.kind = cape_profile_effect_kind(kind); e.duration = 1000; e.brightness = 10;
    if(kind == CAPE_PROFILE_STATIC){
        e.opacity = 255;
        e.transitions = static_cast<cape_profile_transition*>(std::calloc(1, sizeof(cape_profile_transition)));
        if(!e.transitions) throw std::bad_alloc();
        e.ntransitions = 1; e.transitions[0].colour = cape_profile_colour{{1,1,1,1}, 1};
    } else if(kind == CAPE_PROFILE_WAVE || kind == CAPE_PROFILE_RIPPLE){
        e.tail_length = 40; e.velocity = 100;
    } else if(kind >= CAPE_PROFILE_COLOR_PULSE){
        e.speed = 2;
        if(kind != CAPE_PROFILE_RAINBOW_WAVE && kind != CAPE_PROFILE_SPIRAL_RAINBOW){
            e.random = 1; e.colours[0] = 0xffff0000u; e.colours[1] = 0xff0000ffu;
        }
        if(kind == CAPE_PROFILE_COLOR_WAVE || kind == CAPE_PROFILE_RAINBOW_WAVE) e.direction = 2;
        if(kind == CAPE_PROFILE_SPIRAL_RAINBOW) e.clockwise = 1;
        if(kind == CAPE_PROFILE_RAIN || kind == CAPE_PROFILE_TYPE) e.opacity = 255;
        if(kind == CAPE_PROFILE_RAIN) e.direction = 4;
        if(kind == CAPE_PROFILE_VISOR) e.direction = 2;
        if(kind == CAPE_PROFILE_TYPE){ e.type_behaviour = 1; e.type_duration = 2; }
    }
    return result;
}

// The icon is a separate hit target. Its press never reaches the view's selection or drag handling.
class LayerList : public QListWidget {
public:
    std::function<void(int)> toggle;
    QTimer renameTimer;
    explicit LayerList(QWidget* parent) : QListWidget(parent){
        renameTimer.setSingleShot(true);
        connect(&renameTimer, &QTimer::timeout, this, [this](){
            if(currentItem() && isEnabled()) editItem(currentItem());
        });
        connect(this, &QListWidget::currentRowChanged, &renameTimer, &QTimer::stop);
        setMinimumHeight(minimumSizeHint().height());
    }
    QSize minimumSizeHint() const override {
        // Reserve five rows at the normal font size; enlarged fonts can scroll.
        QFont normal = font();
        if(normal.pointSizeF() > 10.) normal.setPointSizeF(10.);
        const int rowHeight = std::max(42, QFontMetrics(normal).height() * 2 + 8);
        return QListWidget::minimumSizeHint().expandedTo(QSize(0, 5 * rowHeight + 2 * frameWidth()));
    }
protected:
    void changeEvent(QEvent* event) override {
        QListWidget::changeEvent(event);
        if(event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange)
            setMinimumHeight(minimumSizeHint().height());
    }
    void mousePressEvent(QMouseEvent* e) override {
        renameTimer.stop();
        auto* item = itemAt(e->pos());
        iconPress = e->button() == Qt::LeftButton && item && e->pos().x() < 34;
        if(iconPress){ pressedRow = row(item); return; }
        renameCandidate = e->button() == Qt::LeftButton && item && item == currentItem() &&
            e->pos().x() >= 34 && e->pos().y() < visualItemRect(item).top() + visualItemRect(item).height()/2;
        pressAt = e->pos();
        QListWidget::mousePressEvent(e);
    }
    void mouseReleaseEvent(QMouseEvent* e) override {
        if(iconPress){
            iconPress = false;
            auto* item = itemAt(e->pos());
            if(item && row(item) == pressedRow && e->pos().x() < 34 && toggle) toggle(pressedRow);
            return;
        }
        QListWidget::mouseReleaseEvent(e);
        if(renameCandidate && (e->pos() - pressAt).manhattanLength() < QApplication::startDragDistance())
            renameTimer.start(QApplication::doubleClickInterval());
        renameCandidate = false;
    }
    void mouseDoubleClickEvent(QMouseEvent* e) override {
        renameTimer.stop(); renameCandidate = false;
        if(e->pos().x() < 34) return;
        // Double click selects; inline rename is deliberately a delayed single click.
        e->accept();
    }
    void startDrag(Qt::DropActions actions) override {
        renameCandidate = false; renameTimer.stop();
        QListWidget::startDrag(actions);
    }
    bool viewportEvent(QEvent* event) override {
        if(event->type() == QEvent::ToolTip){
            auto* help = static_cast<QHelpEvent*>(event);
            auto* item = itemAt(help->pos());
            if(item && help->pos().x() < 34){
                QToolTip::showText(help->globalPos(), item->data(EnabledRole).toBool() ?
                                  tr("Disable layer") : tr("Enable layer"), this);
                return true;
            }
        }
        return QListWidget::viewportEvent(event);
    }
private:
    bool iconPress = false, renameCandidate = false;
    int pressedRow = -1;
    QPoint pressAt;
};

// Follow the controls' font-dependent minimum width without growing for long layer names.
class LayerColumn : public QWidget {
public:
    using QWidget::QWidget;
    QSize minimumSizeHint() const override { return QWidget::minimumSizeHint().expandedTo(QSize(200,0)); }
    QSize sizeHint() const override { return minimumSizeHint(); }
};

// Keep the containing window large enough while this hardware editor is visible.
class LightingWindowMinimum : public QObject {
public:
    LightingWindowMinimum(QWidget* owner, QTableWidget* points) : QObject(owner), editor(owner), table(points){
        editor->installEventFilter(this);
        schedule();
    }
    ~LightingWindowMinimum() override { restore(); }
protected:
    bool eventFilter(QObject*, QEvent* event) override {
        switch(event->type()){
        case QEvent::Hide:
        case QEvent::Show:
        case QEvent::FontChange:
        case QEvent::StyleChange:
        case QEvent::LayoutRequest:
            schedule();
            break;
        default: break;
        }
        return false;
    }
private:
    QWidget* editor;
    QTableWidget* table;
    QPointer<QWidget> host;
    QSize original;
    bool pending = false;
    void restore(){
        if(host){
            host->removeEventFilter(this);
            host->setMinimumSize(original);
            host.clear();
        }
    }
    void schedule(){
        if(pending) return;
        pending = true;
        QTimer::singleShot(0,this,[this](){ pending = false; update(); });
    }
    void update(){
        if(!editor->isVisible() || editor->isWindow()) { restore(); return; }
        QWidget* window = editor->window();
        if(host != window){
            restore(); host = window; original = window->minimumSize(); host->installEventFilter(this);
        }
        // Stretch sections must still leave enough room for every header label.
        int sectionWidth = 0;
        for(int col = 0; col < table->columnCount(); ++col){
            const auto* header = table->horizontalHeaderItem(col);
            if(header) sectionWidth = std::max(sectionWidth,table->horizontalHeader()->fontMetrics().horizontalAdvance(header->text()) + 20);
        }
        table->setMinimumWidth(sectionWidth * table->columnCount() + 2 * table->frameWidth());
        if(editor->layout()) editor->layout()->activate();
        if(window->layout()) window->layout()->activate();
        const QSize minimum = original.expandedTo(window->minimumSizeHint());
        if(window->minimumSize() != minimum) window->setMinimumSize(minimum);
    }
};

class LayerDelegate : public QStyledItemDelegate {
public:
    explicit LayerDelegate(QObject* parent) : QStyledItemDelegate(parent) {}
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex&) const override {
        return QSize(165, std::max(42, option.fontMetrics.height() * 2 + 8));
    }
    void paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& index) const override {
        QStyleOptionViewItem base(opt);
        initStyleOption(&base, index);
        base.text.clear();
        QApplication::style()->drawControl(QStyle::CE_ItemViewItem, &base, p);
        const bool enabled = index.data(EnabledRole).toBool();
        const bool selected = opt.state & QStyle::State_Selected;
        QColor ink = opt.palette.color(selected ? QPalette::HighlightedText : QPalette::Text);
        if(!enabled && !selected) ink = opt.palette.color(QPalette::Disabled, QPalette::Text);
        p->save(); p->setRenderHint(QPainter::Antialiasing);
        const QPointF c(opt.rect.left() + 17, opt.rect.center().y());
        p->setPen(QPen(enabled ? QColor(35, 85, 190) : ink, 1.7));
        if(!enabled){
            p->setBrush(Qt::NoBrush);
            p->drawEllipse(c, 10, 6); p->drawEllipse(c, 3, 3);
            p->drawLine(c + QPointF(-10, -10), c + QPointF(10, 10));
        } else {
            const int kind = index.data(KindRole).toInt();
            p->translate(c - QPointF(12,12));
            if(kind <= CAPE_PROFILE_GRADIENT){
                QLinearGradient gradient(3,3,21,21);
                gradient.setColorAt(0, QColor("#12ab34")); gradient.setColorAt(.5, QColor("#125aff"));
                gradient.setColorAt(1, QColor("#fe7a05"));
                p->setPen(QPen(selected ? Qt::white : QColor("#404040"), 1));
                p->setBrush(kind == CAPE_PROFILE_STATIC ? QBrush(QColor("#125aff")) : QBrush(gradient));
                p->drawRoundedRect(QRectF(3,3,18,18), kind == CAPE_PROFILE_STATIC ? 9 : 3, kind == CAPE_PROFILE_STATIC ? 9 : 3);
            } else if(kind == CAPE_PROFILE_RIPPLE){
                p->setBrush(Qt::NoBrush); p->setPen(QPen(selected ? Qt::white : QColor("#125aff"), 1.5));
                for(int radius : {3,6,9}) p->drawEllipse(QPointF(12,12), radius, radius);
            } else {
                const QColor colours[] = {QColor("#12ab34"), QColor("#125aff"), QColor("#fe7a05")};
                for(int n = 0; n < 3; ++n){
                    p->setPen(QPen(selected ? Qt::white : colours[n], 2, Qt::SolidLine, Qt::RoundCap));
                    if(kind == CAPE_PROFILE_RAIN) p->drawLine(QPointF(7+n*6,4), QPointF(3+n*6,20));
                    else {
                        QPainterPath wave; wave.moveTo(2,6+n*6); wave.cubicTo(8,n*6,16,12+n*6,22,6+n*6);
                        p->drawPath(wave);
                    }
                }
            }
            p->translate(QPointF(12,12) - c);
        }
        p->setPen(ink);
        QRect r = opt.rect.adjusted(36, 3, -3, -3);
        const int h = opt.fontMetrics.height();
        p->drawText(r.adjusted(0, 0, 0, -h), Qt::AlignVCenter,
                    opt.fontMetrics.elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideRight, r.width()));
        p->drawText(r.adjusted(0, h, 0, 0), Qt::AlignVCenter,
                    opt.fontMetrics.elidedText(kindName(index.data(KindRole).toInt()), Qt::ElideRight, r.width()));
        p->restore();
    }
    void updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& option, const QModelIndex&) const override {
        editor->setGeometry(option.rect.adjusted(34, 1, -1, -option.rect.height() / 2));
    }
};

QWidget* rowWidget(QWidget* parent, std::initializer_list<QWidget*> widgets){
    auto* row = new QWidget(parent);
    auto* box = new QHBoxLayout(row); box->setContentsMargins(0, 0, 0, 0);
    for(auto* w : widgets) box->addWidget(w);
    box->addStretch(); return row;
}
QDoubleSpinBox* realBox(QWidget* parent, const char* name, int decimals, double min, double max){
    auto* box = new QDoubleSpinBox(parent); box->setObjectName(name);
    box->setDecimals(decimals); box->setRange(min, max); box->setKeyboardTracking(false);
    box->setSingleStep(std::pow(10., -decimals)); return box;
}
QComboBox* choices(QWidget* parent, const char* name, const QStringList& names){
    auto* box = new QComboBox(parent); box->setObjectName(name); box->addItems(names); return box;
}
}

// All points remain in the model and table. Overlapping handles are never merged.
class HwLightStrip : public QWidget {
public:
    const cape_profile_effect* effect = nullptr;
    int selectedRow = -1;
    std::function<void(int)> select, pick;
    std::function<void(int, int, double)> move;
    explicit HwLightStrip(QWidget* parent) : QWidget(parent){
        setObjectName("hwLightStrip"); setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed);
    }
    QSize sizeHint() const override { return QSize(0,48+fontMetrics().height()); }
    QSize minimumSizeHint() const override { return sizeHint(); }
    void cancelGesture(){ hit = -1; body = dragged = false; }
    void setEffect(const cape_profile_effect* next){
        // Model copies replace pointers during a drag, but a different effect cancels it.
        if(!next || std::memcmp(&effectId,&next->id,sizeof(effectId))) cancelGesture();
        effect = next;
        if(next) effectId = next->id;
    }
protected:
    void changeEvent(QEvent* event) override {
        QWidget::changeEvent(event);
        if(event->type() == QEvent::EnabledChange && !isEnabled()) cancelGesture();
    }
    void paintEvent(QPaintEvent*) override {
        QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
        const QRectF band(7, 9, std::max(1, width() - 14), 28);
        p.save(); p.setClipRect(band);
        for(int x = 0; x < width(); x += 8)
            for(int y = 9; y < 37; y += 8)
                p.fillRect(x, y, 8, 8, ((x / 8 + (y - 9) / 8) % 2) ? QColor(190,190,190) : QColor(250,250,250));
        if(effect){
            const auto& e = *effect;
            if(e.kind == CAPE_PROFILE_SOLID){
                for(uint32_t i = 0; i + 1 < e.ntransitions; i += 2)
                    p.fillRect(QRectF(xAt(e.transitions[i].time), 9,
                                     (e.transitions[i+1].time - e.transitions[i].time) * band.width(), 28), colour(e.transitions[i].colour));
            } else {
                for(uint32_t i = 1; i < e.ntransitions; ++i){
                    const double x = xAt(e.transitions[i-1].time), end = xAt(e.transitions[i].time);
                    if(end <= x) continue;
                    QLinearGradient gradient(x, 0, end, 0);
                    gradient.setColorAt(0, colour(e.transitions[i-1].colour));
                    gradient.setColorAt(1, colour(e.transitions[i].colour));
                    p.fillRect(QRectF(x, 9, end - x, 28), gradient);
                }
            }
        }
        p.restore(); p.setPen(palette().color(QPalette::Text));
        p.drawText(QRect(7, 46, width()/2, fontMetrics().height()), Qt::AlignLeft | Qt::AlignVCenter, tr("0 s"));
        if(!effect) return;
        p.drawText(QRect(width()/2, 46, width()/2-7, fontMetrics().height()), Qt::AlignRight | Qt::AlignVCenter,
                   tr("%1 s").arg(effect->duration / 1000., 0, 'g', 6));
        auto circle = [&](uint32_t i){
            const int row = effect->kind == CAPE_PROFILE_SOLID ? int(i / 2) : int(i);
            p.setPen(QPen(Qt::black, row == selectedRow ? 2. : 1.));
            p.setBrush(colour(effect->transitions[i].colour));
            p.drawEllipse(QPointF(xAt(effect->transitions[i].time), markerY(i)), row == selectedRow ? 5.5 : 4.5, row == selectedRow ? 5.5 : 4.5);
        };
        // Keep the selected handle visible when times coincide.
        for(uint32_t i = 0; i < effect->ntransitions; ++i){
            const int row = effect->kind == CAPE_PROFILE_SOLID ? int(i/2) : int(i);
            if(row != selectedRow) circle(i);
        }
        if(selectedRow >= 0){
            const uint32_t count = effect->kind == CAPE_PROFILE_SOLID ? 2u : 1u;
            for(uint32_t i = uint32_t(selectedRow)*count; i < uint32_t(selectedRow)*count+count && i < effect->ntransitions; ++i) circle(i);
        }
    }
    void mousePressEvent(QMouseEvent* event) override {
        hit = -1; body = false; dragged = false;
        if(!effect || !isEnabled() || event->button() != Qt::LeftButton) return;
        press = event->pos();
        double distance = 8;
        // Prefer the selected row when several handles share a pixel.
        for(uint32_t i = 0; i < effect->ntransitions; ++i){
            const double d = std::hypot(event->pos().x() - xAt(effect->transitions[i].time), event->pos().y() - markerY(i));
            const int previousRow = effect->kind == CAPE_PROFILE_SOLID ? hit/2 : hit;
            if(d <= distance && (hit < 0 || d < distance || previousRow != selectedRow)){ distance = d; hit = int(i); }
        }
        if(hit < 0 && effect->kind == CAPE_PROFILE_SOLID && event->pos().y() >= 9 && event->pos().y() <= 37){
            const double t = tAt(event->pos().x());
            for(uint32_t i = 0; i + 1 < effect->ntransitions; i += 2)
                if(t >= effect->transitions[i].time && t <= effect->transitions[i+1].time){ hit = int(i); body = true; break; }
        }
        if(hit < 0) return;
        initialTime = effect->transitions[hit].time;
        const int row = effect->kind == CAPE_PROFILE_SOLID ? hit/2 : hit;
        if(select) select(row);
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        if(hit < 0 || !effect || !isEnabled() || !(event->buttons() & Qt::LeftButton)) return;
        dragged |= (event->pos() - press).manhattanLength() >= QApplication::startDragDistance();
        if(dragged && move)
            move(effect->kind == CAPE_PROFILE_SOLID ? hit/2 : hit,
                 body ? 2 : effect->kind == CAPE_PROFILE_SOLID ? hit%2 : 0,
                 initialTime + double(event->pos().x() - press.x()) / std::max(1, width() - 14));
    }
    void mouseReleaseEvent(QMouseEvent* event) override {
        if(event->button() != Qt::LeftButton || hit < 0 || !effect || !isEnabled()) return;
        const int row = effect->kind == CAPE_PROFILE_SOLID ? hit/2 : hit;
        hit = -1;
        if(!dragged && pick) pick(row);
    }
private:
    cape_profile_uuid effectId{};
    int hit = -1;
    bool body = false, dragged = false;
    QPoint press;
    double initialTime = 0;
    double xAt(double t) const { return 7 + t * std::max(1, width() - 14); }
    double markerY(uint32_t point) const { return effect->kind == CAPE_PROFILE_SOLID ? (point%2 ? 37. : 9.) : 10.; }
    double tAt(int x) const { return double(x - 7) / std::max(1, width() - 14); }
};

HwLightWidget::HwLightWidget(QWidget* parent) : QWidget(parent){
    setObjectName("hwLightWidget");
    auto* root = new QVBoxLayout(this); root->setContentsMargins(0, 0, 0, 0); root->setSpacing(5);
    root->addWidget(new QLabel(tr("Hardware lighting   |   Select a layer to edit its keys and settings."), this));
    keyboard = new KeyWidget(this); keyboard->setObjectName("hwLightKeys"); keyboard->rgbMode(true);
    keyboard->selectionOutline(true);
    keyboard->setFixedHeight(160);
    root->addWidget(keyboard);
    allKeys = new QCheckBox(tr("All keys"), this); allKeys->setObjectName("hwLightAllKeys");
    clear = new QPushButton(tr("Clear selection"), this); clear->setObjectName("hwLightClear");
    selectionNote = new QLabel(this); selectionNote->setObjectName("hwLightSelectionNote");
    root->addWidget(rowWidget(this, {allKeys, clear, selectionNote}));
    auto* body = new QHBoxLayout; root->addLayout(body, 1);
    auto* left = new LayerColumn(this);
    auto* column = new QVBoxLayout(left); column->setContentsMargins(0,0,0,0); column->setSpacing(5);
    quota = new QLabel(this); quota->setObjectName("hwLightQuota");
    quota->setToolTip(tr("The top layer has priority. Muted Custom layers also count towards the maximum of five.\nDrag layers or use Move up / Move down to change their order."));
    column->addWidget(quota);
    auto* list = new LayerList(this); layers = list; layers->setObjectName("hwLightLayers");
    layers->setItemDelegate(new LayerDelegate(layers)); layers->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layers->setDragDropMode(QAbstractItemView::InternalMove); layers->setDefaultDropAction(Qt::MoveAction);
    layers->setContextMenuPolicy(Qt::CustomContextMenu);
    column->addWidget(layers, 1);
    add = new QToolButton(this); add->setText(tr("Add...")); add->setObjectName("hwLightAdd");
    add->setPopupMode(QToolButton::InstantPopup);
    auto* addMenu = new QMenu(add); add->setMenu(addMenu);
    auto* customMenu = addMenu->addMenu(tr("Custom")); auto* presetMenu = addMenu->addMenu(tr("Presets"));
    for(int kind = 0; kind < CAPE_PROFILE_EFFECTS; ++kind){
        auto* action = (kind <= CAPE_PROFILE_RIPPLE ? customMenu : presetMenu)->addAction(kindName(kind));
        action->setObjectName(QString("hwLightAdd%1").arg(kind)); action->setData(kind);
        connect(action, &QAction::triggered, this, [this,kind](){ addLayer(kind); });
    }
    remove = new QPushButton(tr("Remove"), this); remove->setObjectName("hwLightRemove");
    auto* actions = new QHBoxLayout; actions->addWidget(add); actions->addWidget(remove); column->addLayout(actions);
    up = new QPushButton(tr("Move up"), this); up->setObjectName("hwLightUp");
    down = new QPushButton(tr("Move down"), this); down->setObjectName("hwLightDown");
    column->addWidget(rowWidget(this, {up, down})); body->addWidget(left);
    auto* right = new QVBoxLayout; right->setSpacing(5); body->addLayout(right, 1);
    mutedNote = new QLabel(tr("This layer is muted. Its keys and settings are kept."), this);
    mutedNote->setWordWrap(true); mutedNote->setObjectName("hwLightMutedNote"); right->addWidget(mutedNote);
    panels = new QStackedWidget(this); right->addWidget(panels, 1);
    emptyNote = new QLabel(tr("No lighting layers. Add a layer to light this slot's keys."), this);
    emptyNote->setWordWrap(true); panels->addWidget(emptyNote);
    auto* staticPanel = new QWidget(this); auto* form = new QFormLayout(staticPanel);
    staticColour = new ColorButton(this); staticColour->setObjectName("hwLightStaticColour");
    opacity = realBox(this, "hwLightOpacity", 0, 0, 100); opacity->setSuffix(tr(" %"));
    form->addRow(tr("Colour:"), staticColour); form->addRow(tr("Opacity:"), opacity); panels->addWidget(staticPanel);

    customTabs = new QTabWidget(this); panels->addWidget(customTabs);
    auto* colours = new QWidget(this); auto* coloursBox = new QVBoxLayout(colours); coloursBox->setContentsMargins(7,7,7,7);
    duration = realBox(this, "hwLightDuration", 1, .1, 99.9); duration->setSuffix(tr(" s"));
    addPoint = new QPushButton(this); addPoint->setObjectName("hwLightAddPoint");
    removePoint = new QPushButton(tr("Remove"), this); removePoint->setObjectName("hwLightRemovePoint");
    auto* timeRow = new QHBoxLayout; timeRow->addWidget(new QLabel(tr("Duration:"), this)); timeRow->addWidget(duration);
    timeRow->addStretch(); timeRow->addWidget(addPoint); timeRow->addWidget(removePoint); coloursBox->addLayout(timeRow);
    strip = new HwLightStrip(this); coloursBox->addWidget(strip);
    points = new QTableWidget(this); points->setObjectName("hwLightPoints"); points->setSelectionBehavior(QAbstractItemView::SelectRows);
    points->setSelectionMode(QAbstractItemView::SingleSelection); points->verticalHeader()->hide();
    points->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch); coloursBox->addWidget(points, 1);
    customTabs->addTab(colours, tr("Colours"));
    auto* options = new QWidget(this); auto* optionsForm = new QFormLayout(options);
    start = choices(this, "hwLightStart", {tr("With profile"), tr("On key pressed")});
    stop = choices(this, "hwLightStop", {tr("After"), tr("On key press"), tr("On key release"), tr("Never")});
    after = new QSpinBox(this); after->setObjectName("hwLightAfter"); after->setRange(1,99);
    tail = realBox(this, "hwLightTail", 1, .1, 99.9);
    velocity = realBox(this, "hwLightVelocity", 1, .1, 99.9);
    angle = new QSpinBox(this); angle->setObjectName("hwLightAngle"); angle->setRange(0,359);
    twoSides = new QCheckBox(tr("Two sides"), this); twoSides->setObjectName("hwLightTwoSides");
    optionsForm->addRow(tr("Start:"), start); optionsForm->addRow(tr("Stop:"), rowWidget(this, {stop, after}));
    optionsForm->addRow(tr("Tail:"), tail); optionsForm->addRow(tr("Velocity:"), velocity);
    optionsForm->addRow(tr("Angle:"), angle); optionsForm->addRow(twoSides); customTabs->addTab(options, tr("Options"));

    auto* preset = new QWidget(this); auto* presetForm = new QFormLayout(preset); panels->addWidget(preset);
    colourMode = choices(this, "hwLightColourMode", {tr("Random"), tr("Alternating")});
    presetColours[0] = new ColorButton(this); presetColours[1] = new ColorButton(this);
    for(int n = 0; n < 2; ++n){ presetColours[n]->setObjectName(QString("hwLightPresetColour%1").arg(n)); presetColours[n]->setLabel(false); }
    colourRow = rowWidget(this, {presetColours[0], presetColours[1]});
    speed = choices(this, "hwLightSpeed", {tr("Slow"), tr("Medium"), tr("Fast")});
    direction = choices(this, "hwLightDirection", {});
    typeMode = choices(this, "hwLightTypeMode", {tr("Key"), tr("Ripple")});
    typeDuration = choices(this, "hwLightTypeDuration", {tr("Short"), tr("Medium"), tr("Long")});
    typeRow = rowWidget(this, {new QLabel(tr("Mode:"), this), typeMode}); presetForm->addRow(typeRow);
    presetForm->addRow(colourMode); presetForm->addRow(colourRow);
    speedRow = rowWidget(this, {new QLabel(tr("Speed:"), this), speed}); presetForm->addRow(speedRow);
    directionRow = rowWidget(this, {new QLabel(tr("Direction:"), this), direction}); presetForm->addRow(directionRow);
    typeDurationRow = rowWidget(this, {new QLabel(tr("Duration:"), this), typeDuration}); presetForm->addRow(typeDurationRow);
    auto* note = new QLabel(tr("Note: Enabled effects will run on the keyboard only after saving the slot."), this);
    note->setWordWrap(true); root->addWidget(note);
    new LightingWindowMinimum(this,points);

    list->toggle = [this](int row){ toggle(row); };
    connect(layers, &QListWidget::currentRowChanged, this, [this](int row){ if(!filling){ current = row; refreshPanel(); } });
    connect(layers, &QListWidget::itemChanged, this, [this](QListWidgetItem* item){
        if(filling || !editable) return;
        const QByteArray name = item->text().toUtf8();
        if(size_t(name.size()) > CAPE_XML_VALUE_MAX || name.contains('\0')){
            QTimer::singleShot(0, this, [this](){
                refreshList();
                QMessageBox::warning(this, tr("Hardware lighting"), tr("This name is too long or contains an invalid character. Your draft is kept."));
            });
            return;
        }
        const int saved = current; current = layers->row(item);
        mutate([&](cape_profile_layer& layer){ setText(layer.name, name); }, false); current = saved;
        // Let the view finish committing its editor before replacing its items.
        QTimer::singleShot(0, this, [this](){ refreshList(); });
    });
    connect(layers->model(), &QAbstractItemModel::rowsMoved, this,
            [this](const QModelIndex&, int first, int, const QModelIndex&, int destination){
        if(!filling){
            const int to = destination > first ? destination - 1 : destination;
            const unsigned before = revision;
            QTimer::singleShot(0, this, [this,first,to,before](){ if(revision == before) move(first, to); });
        }
    });
    connect(layers, &QWidget::customContextMenuRequested, this, &HwLightWidget::contextMenu);
    connect(up, &QPushButton::clicked, this, [this](){ move(current, current - 1); });
    connect(down, &QPushButton::clicked, this, [this](){ move(current, current + 1); });
    connect(remove, &QPushButton::clicked, this, &HwLightWidget::removeLayer);
    connect(keyboard, &KeyWidget::selectionChanged, this, &HwLightWidget::changeKeys);
    connect(clear, &QPushButton::clicked, this, [this](){ changeKeys({}); });
    connect(allKeys, &QCheckBox::toggled, this, [this](bool all){
        if(filling || !layout) return;
        if(!all) { changeKeys(selectedKeys()); return; }
        mutate([](cape_profile_layer& l){
            for(uint32_t i = 0; i < l.nkeys; ++i) std::free(l.keys[i].p);
            std::free(l.keys); l.keys = nullptr; l.nkeys = 0; l.option = CAPE_PROFILE_OPTION_ALL;
        });
    });
    connect(staticColour, &ColorButton::colorChanged, this, [this](QColor c){
        if(selected() && selected()->effect.ntransitions && colour(selected()->effect.transitions[0].colour).rgb() == c.rgb()) return;
        mutate([&](cape_profile_layer& l){ if(l.effect.ntransitions) setRgb(l.effect.transitions[0].colour, c); });
    });
    connect(opacity, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v){
        if(!filling) mutate([&](cape_profile_layer& l){ l.effect.opacity = uint32_t(std::lround(v * 255. / 100.)); });
    });
    connect(duration, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v){
        if(!filling) mutate([&](cape_profile_layer& l){ l.effect.duration = uint32_t(std::lround(v * 1000.)); });
    });
    connect(start, QOverload<int>::of(&QComboBox::activated), this, [this](int v){
        mutate([&](cape_profile_layer& l){ l.start_with_profile = v == 0; l.start_on_key_press = v == 1; });
    });
    connect(stop, QOverload<int>::of(&QComboBox::activated), this, [this](int v){
        mutate([&](cape_profile_layer& l){
            l.stop_on_key_press = v == 1; l.stop_on_key_release = v == 2;
            l.stop_after_times = v == 0 ? std::max(1, after->value()) : v == 3 ? -1 : 0;
        });
    });
    connect(after, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int v){
        if(!filling) mutate([&](cape_profile_layer& l){ l.stop_after_times = v; });
    });
    connect(tail, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v){
        if(!filling) mutate([&](cape_profile_layer& l){ l.effect.tail_length = std::round(v * 10.); });
    });
    connect(velocity, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v){
        if(!filling) mutate([&](cape_profile_layer& l){ l.effect.velocity = std::round(v * 10.); });
    });
    connect(angle, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int v){
        if(!filling) mutate([&](cape_profile_layer& l){ l.effect.angle = v; });
    });
    connect(twoSides, &QCheckBox::toggled, this, [this](bool v){
        if(!filling) mutate([&](cape_profile_layer& l){ l.effect.double_sided = v; });
    });
    connect(colourMode, QOverload<int>::of(&QComboBox::activated), this, [this](int v){
        mutate([&](cape_profile_layer& l){ l.effect.random = v == 0; });
    });
    for(int n = 0; n < 2; ++n) connect(presetColours[n], &ColorButton::colorChanged, this, [this,n](QColor c){
        mutate([&](cape_profile_layer& l){ l.effect.colours[n] = (l.effect.colours[n] & 0xff000000u) | (c.rgb() & 0x00ffffffu); });
    });
    connect(speed, QOverload<int>::of(&QComboBox::activated), this, [this](int v){
        mutate([&](cape_profile_layer& l){ l.effect.speed = uint32_t(v + 1); });
    });
    connect(direction, QOverload<int>::of(&QComboBox::activated), this, [this](int v){
        mutate([&](cape_profile_layer& l){
            if(l.effect.kind == CAPE_PROFILE_SPIRAL_RAINBOW) l.effect.clockwise = v == 0;
            else l.effect.direction = uint32_t(v + 1);
        });
    });
    connect(typeMode, QOverload<int>::of(&QComboBox::activated), this, [this](int v){
        mutate([&](cape_profile_layer& l){ l.effect.type_behaviour = uint32_t(v); });
    });
    connect(typeDuration, QOverload<int>::of(&QComboBox::activated), this, [this](int v){
        mutate([&](cape_profile_layer& l){ l.effect.type_duration = uint32_t(v + 1); });
    });
    strip->select = [this](int row){ points->selectRow(row); };
    strip->pick = [this](int row){ pointColour(row); };
    strip->move = [this](int row, int end, double t){ setPointTime(row, end, t); };
    connect(points, &QTableWidget::itemSelectionChanged, this, [this](){ strip->selectedRow = points->currentRow(); strip->update(); });
    connect(points, &QTableWidget::cellClicked, this, [this](int row, int col){
        if(auto* l = selected()) if(col == (l->effect.kind == CAPE_PROFILE_SOLID ? 2 : 1)) pointColour(row);
    });
    connect(points, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item){
        if(filling || !selected()) return;
        const int row = item->row(), col = item->column();
        const QString input = item->text();
        const auto id = selected()->id;
        const unsigned before = revision;
        // A cell delegate still owns its editor while committing data. Rebuild only after that commit has returned.
        QTimer::singleShot(0, this, [this,row,col,input,id,before](){
            if(revision != before || !selected() || std::memcmp(&id, &selected()->id, sizeof(id))) return;
            const bool solid = selected()->effect.kind == CAPE_PROFILE_SOLID;
            const int intensityColumn = solid ? 3 : 2;
            bool ok; const double value = input.toDouble(&ok);
            if(!ok || !std::isfinite(value)){ fillPoints(); return; }
            if(col < (solid ? 2 : 1)){
                const double seconds = selected()->effect.duration / 1000.;
                const uint32_t n = uint32_t(row) * (solid ? 2u : 1u) + uint32_t(col);
                if(n >= selected()->effect.ntransitions) return;
                const double shown = QString::number(selected()->effect.transitions[n].time * seconds, 'f', 3).toDouble();
                if(value != shown && seconds > 0) setPointTime(row, col, value / seconds);
                else fillPoints();
            } else if(col == intensityColumn && value >= 0 && value <= 100){
                const uint32_t first = uint32_t(row) * (solid ? 2u : 1u);
                if(first >= selected()->effect.ntransitions) return;
                const auto& c = selected()->effect.transitions[first].colour;
                const double shown = QString::number((c.has_alpha ? c.c[3] : 1.) * 100., 'f', 1).toDouble();
                if(value == shown){ fillPoints(); return; }
                mutate([&](cape_profile_layer& l){
                    for(uint32_t n = first; n < first + (solid ? 2u : 1u); ++n){
                        l.effect.transitions[n].colour.has_alpha = 1;
                        l.effect.transitions[n].colour.c[3] = value / 100.;
                    }
                });
            } else fillPoints();
        });
    });
    connect(addPoint, &QPushButton::clicked, this, &HwLightWidget::addTransition);
    connect(removePoint, &QPushButton::clicked, this, &HwLightWidget::removeTransition);
    refreshList();
}

cape_profile_layer* HwLightWidget::selected(){
    return draft && current >= 0 && uint32_t(current) < draft->lighting.data().count ? &draft->lighting.data().layers[current] : nullptr;
}

void HwLightWidget::setSlot(HwSlotDraft::Draft* next, const KeyMap& map, const QString& physical,
                           const QMap<QString, QColor>& indicators, bool canEdit){
    strip->cancelGesture();
    if(draft != next) current = -1;
    ++revision;
    draft = next; editable = canEdit && draft && draft->lighting.data().known;
    layout = physicalLayout(physical);
    filling = true;
    keyboard->map(lightingMap(map,layout)); keyboard->setFixedKeys(indicators, tr("Set in Performance tab"));
    filling = false;
    refreshList();
}

void HwLightWidget::mutate(const std::function<void(cape_profile_layer&)>& change, bool refresh){
    if(filling || !editable || !selected()) return;
    try {
        HwLighting::Model next = draft->lighting;
        change(next.data().layers[current]);
        if(!HwLighting::same(next, draft->lighting)){
            draft->lighting.swap(next);
            // A rename or toggle may defer rebuilding the list, but the strip must never keep a pointer into the old model.
            strip->setEffect(selected()->effect.kind >= CAPE_PROFILE_SOLID && selected()->effect.kind <= CAPE_PROFILE_RIPPLE ?
                &selected()->effect : nullptr);
            // Refresh before notifying: a slot update can replace the draft we were editing.
            if(refresh) refreshList();
            emit draftChanged();
        } else if(refresh) refreshList();
    } catch(const std::bad_alloc&){
        QMessageBox::warning(this, tr("Hardware lighting"), tr("Not enough memory to edit this layer. Your draft is kept."));
    }
}

void HwLightWidget::refreshList(){
    filling = true;
    static_cast<LayerList*>(layers)->renameTimer.stop();
    layers->clear();
    const auto* model = draft ? &draft->lighting.data() : nullptr;
    if(model && model->known) for(uint32_t n = 0; n < model->count; ++n){
        const auto& l = model->layers[n];
        auto* item = new QListWidgetItem(text(l.name), layers);
        item->setData(KindRole, int(l.effect.kind)); item->setData(EnabledRole, bool(l.enabled));
        item->setData(Qt::AccessibleTextRole, tr("%1, %2, %3").arg(text(l.name), kindName(l.effect.kind), l.enabled ? tr("enabled") : tr("muted")));
        item->setToolTip(text(l.name) + "\n" + kindName(l.effect.kind));
        item->setFlags(item->flags() | Qt::ItemIsEditable);
    }
    if(current >= layers->count()) current = layers->count() - 1;
    if(current < 0 && layers->count()) current = 0;
    layers->setCurrentRow(current); layers->setEnabled(editable);
    const bool preset = model && model->count && model->layers[0].effect.kind >= CAPE_PROFILE_COLOR_PULSE;
    for(int kind = 0; kind < CAPE_PROFILE_EFFECTS; ++kind)
        add->menu()->findChild<QAction*>(QString("hwLightAdd%1").arg(kind))->setEnabled(
            editable && (kind >= CAPE_PROFILE_COLOR_PULSE || preset || layers->count() < 5));
    quota->setText(preset ? tr("Layers (%1/1)").arg(layers->count()) : tr("Layers (%1/5)").arg(layers->count()));
    filling = false; refreshPanel();
}

void HwLightWidget::refreshPanel(){
    filling = true;
    auto* l = selected();
    const bool solid = l && l->effect.kind == CAPE_PROFILE_SOLID;
    const bool custom = l && l->effect.kind <= CAPE_PROFILE_RIPPLE;
    mutedNote->setVisible(l && !l->enabled);
    remove->setEnabled(editable && l); up->setEnabled(editable && l && current > 0);
    add->setEnabled(editable);
    down->setEnabled(editable && l && current + 1 < layers->count());
    panels->setEnabled(editable && l);
    if(!l){
        emptyNote->setText(draft && draft->lighting.data().known ?
            tr("No lighting layers. Add a layer to light this slot's keys.") : tr("Lighting is unavailable for this slot."));
        panels->setCurrentIndex(0);
    }
    else if(l->effect.kind == CAPE_PROFILE_STATIC){
        panels->setCurrentIndex(1);
        staticColour->color(l->effect.ntransitions ? colour(l->effect.transitions[0].colour) : QColor(Qt::black));
        opacity->setValue(l->effect.opacity * 100. / 255.);
    } else if(custom){
        panels->setCurrentIndex(2);
        const auto& e = l->effect;
        duration->setRange(.1, 99.9);
        duration->setValue(e.duration / 1000.);
        duration->setToolTip(tr("Stored duration: %1 ms. Changing duration keeps relative point positions.").arg(e.duration));
        start->setCurrentIndex(l->start_on_key_press ? 1 : 0);
        stop->setCurrentIndex(l->stop_on_key_press ? 1 : l->stop_on_key_release ? 2 : l->stop_after_times > 0 ? 0 : 3);
        after->setRange(1, 99);
        after->setValue(l->stop_after_times >= 0 ? l->stop_after_times : 1); after->setVisible(stop->currentIndex() == 0);
        tail->setRange(.1, 99.9); tail->setValue(e.tail_length / 10.);
        velocity->setRange(.1, 99.9); velocity->setValue(e.velocity / 10.);
        angle->setValue(int(e.angle)); twoSides->setChecked(e.double_sided);
        auto* form = static_cast<QFormLayout*>(customTabs->widget(1)->layout());
        const bool spatial = e.kind == CAPE_PROFILE_RIPPLE || e.kind == CAPE_PROFILE_WAVE;
        for(auto* box : {tail, velocity}){ box->setVisible(spatial); form->labelForField(box)->setVisible(spatial); }
        angle->setVisible(e.kind == CAPE_PROFILE_WAVE); form->labelForField(angle)->setVisible(e.kind == CAPE_PROFILE_WAVE);
        twoSides->setVisible(e.kind == CAPE_PROFILE_WAVE);
        addPoint->setText(solid ? tr("Add block") : tr("Add point"));
        addPoint->setEnabled(editable && e.ntransitions + (solid ? 2u : 1u) <= MaxPoints);
    } else {
        panels->setCurrentIndex(3);
        const auto& e = l->effect;
        const bool hasColours = e.kind != CAPE_PROFILE_RAINBOW_WAVE && e.kind != CAPE_PROFILE_SPIRAL_RAINBOW;
        colourMode->setVisible(hasColours); colourMode->setCurrentIndex(e.random ? 0 : 1);
        colourRow->setVisible(hasColours && !e.random);
        for(int n = 0; n < 2; ++n) presetColours[n]->color(QColor::fromRgba(e.colours[n]));
        typeRow->setVisible(e.kind == CAPE_PROFILE_TYPE); typeMode->setCurrentIndex(int(e.type_behaviour));
        typeDurationRow->setVisible(e.kind == CAPE_PROFILE_TYPE && e.type_behaviour == 0); typeDuration->setCurrentIndex(int(e.type_duration) - 1);
        speedRow->setVisible(e.kind != CAPE_PROFILE_TYPE || e.type_behaviour != 0); speed->setCurrentIndex(int(e.speed) - 1);
        const bool spiral = e.kind == CAPE_PROFILE_SPIRAL_RAINBOW;
        directionRow->setVisible(spiral || e.kind == CAPE_PROFILE_COLOR_WAVE || e.kind == CAPE_PROFILE_RAINBOW_WAVE);
        direction->clear(); direction->addItems(spiral ? QStringList{tr("Clockwise"), tr("Counterclockwise")} :
                                                QStringList{tr("Left"), tr("Right"), tr("Up"), tr("Down")});
        direction->setCurrentIndex(spiral ? (e.clockwise ? 0 : 1) : int(e.direction) - 1);
    }
    filling = false;
    fillPoints(); refreshKeys();
}

QString HwLightWidget::profileKeyName(const QString& key) const {
    if(layout == CAPE_PROFILE_LAYOUT_KS){
        if(key == "lang1") return QStringLiteral("Lang1");
        if(key == "lang2") return QStringLiteral("Lang2");
    }
    const QByteArray utf = key.toLatin1();
    for(const auto& entry : K95PLedMap::entries) if(utf == entry.name){
        const char* name = cape_profile_led_name(entry.led);
        if(name && cape_profile_light(name, std::strlen(name), layout, nullptr) >= 0) return QString::fromLatin1(name);
    }
    return QString();
}

QStringList HwLightWidget::selectedKeys() const {
    QStringList result;
    if(!draft || current < 0 || uint32_t(current) >= draft->lighting.data().count) return result;
    const auto& l = draft->lighting.data().layers[current];
    for(const auto& key : keyboard->map().keys()){
        const QString name = profileKeyName(key);
        if(name.isEmpty()) continue;
        if(l.option == CAPE_PROFILE_OPTION_ALL){ result << key; continue; }
        for(uint32_t n = 0; n < l.nkeys; ++n) if(text(l.keys[n]) == name){ result << key; break; }
    }
    return result;
}

void HwLightWidget::refreshKeys(){
    const auto* l = selected(); const bool custom = l && l->effect.kind <= CAPE_PROFILE_RIPPLE;
    filling = true;
    allKeys->setChecked(l && l->option == CAPE_PROFILE_OPTION_ALL); allKeys->setEnabled(editable && custom && layout);
    clear->setEnabled(editable && custom && layout); keyboard->setEnabled(editable && custom && layout);
    const QStringList keys = selectedKeys(); keyboard->setSelection(keys);
    QColorMap colours; const QColor c = l && l->effect.kind == CAPE_PROFILE_STATIC && l->effect.ntransitions ?
        colour(l->effect.transitions[0].colour) : QColor(Qt::black);
    for(const auto& key : keyboard->map().keys()) colours[key] = keys.contains(key) ? c.rgb() : qRgb(0,0,0);
    keyboard->colorMap(colours);
    selectionNote->setText(!l ? QString() : !layout ? tr("Physical layout unknown: key selection is unavailable.") : !custom ? tr("Preset: all keys") :
        l->option == CAPE_PROFILE_OPTION_ALL ? tr("All keys") : keys.isEmpty() ?
        tr("This layer has no keys: it is saved, but lights no keys.") : tr("%1 keys selected").arg(keys.size()));
    selectionNote->setWordWrap(true);
    filling = false;
}

void HwLightWidget::changeKeys(const QStringList& keys){
    if(filling || !layout || !selected() || selected()->effect.kind > CAPE_PROFILE_RIPPLE) return;
    std::vector<std::pair<int, QByteArray>> names;
    for(const auto& key : keys){
        QByteArray name = profileKeyName(key).toLatin1(); int order;
        if(cape_profile_light(name.constData(), size_t(name.size()), layout, &order) < 0) continue;
        names.emplace_back(order, name);
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    mutate([&](cape_profile_layer& l){
        for(uint32_t n = 0; n < l.nkeys; ++n) std::free(l.keys[n].p);
        std::free(l.keys); l.keys = nullptr; l.nkeys = 0; l.option = CAPE_PROFILE_OPTION_CONCRETE;
        if(!names.empty()){
            l.keys = static_cast<cape_profile_text*>(std::calloc(names.size(), sizeof(cape_profile_text)));
            if(!l.keys) throw std::bad_alloc();
            l.nkeys = uint32_t(names.size());
            for(size_t n = 0; n < names.size(); ++n) setText(l.keys[n], names[n].second);
        }
    });
}

void HwLightWidget::toggle(int row){
    const int saved = current; current = row;
    mutate([](cape_profile_layer& layer){ layer.enabled = !layer.enabled; }, false);
    current = saved; refreshList();
}
void HwLightWidget::rename(int row){
    if(!editable || row < 0 || row >= layers->count()) return;
    layers->setCurrentRow(row); layers->editItem(layers->item(row));
}
void HwLightWidget::move(int from, int to){
    if(filling || !editable || !draft || from < 0 || to < 0 || uint32_t(from) >= draft->lighting.data().count ||
       uint32_t(to) >= draft->lighting.data().count || from == to) return;
    auto& model = draft->lighting.data();
    const auto layer = model.layers[from];
    if(from < to) std::move(model.layers + from + 1, model.layers + to + 1, model.layers + from);
    else std::move_backward(model.layers + to, model.layers + from, model.layers + from + 1);
    model.layers[to] = layer; current = to; ++revision;
    refreshList(); emit draftChanged();
}
void HwLightWidget::removeLayer(){
    if(!editable || !selected()) return;
    const int row = current; const auto id = selected()->id;
    const unsigned before = revision;
    QMessageBox dialog(QMessageBox::Warning, tr("Remove layer"),
        tr("Remove \"%1\" from this slot's draft?\n\nThe keyboard changes only after saving the slot.").arg(text(selected()->name)),
        QMessageBox::NoButton, this);
    auto* accept = dialog.addButton(tr("Remove"), QMessageBox::DestructiveRole);
    dialog.addButton(QMessageBox::Cancel); dialog.setDefaultButton(QMessageBox::Cancel); dialog.setEscapeButton(QMessageBox::Cancel);
    dialog.exec();
    if(dialog.clickedButton() != accept || revision != before || !editable || row != current || !selected() ||
       std::memcmp(&id, &selected()->id, sizeof(id))) return;
    try {
        const auto& old = draft->lighting.data(); std::vector<cape_profile_layer> remaining;
        for(uint32_t n = 0; n < old.count; ++n) if(int(n) != row) remaining.push_back(old.layers[n]);
        cape_lightmodel model{}; model.known = 1; model.count = uint32_t(remaining.size()); model.layers = remaining.data();
        HwLighting::Model next(model); draft->lighting.swap(next); ++revision; refreshList(); emit draftChanged();
    } catch(const std::bad_alloc&){ QMessageBox::warning(this, tr("Hardware lighting"), tr("Not enough memory. Your draft is kept.")); }
}
void HwLightWidget::addLayer(int kind){
    if(!editable || !draft || kind < 0 || kind >= CAPE_PROFILE_EFFECTS) return;
    const auto& old = draft->lighting.data();
    const bool preset = kind >= CAPE_PROFILE_COLOR_PULSE;
    const bool replace = old.count && (preset || old.layers[0].effect.kind >= CAPE_PROFILE_COLOR_PULSE);
    if(!preset && !replace && old.count >= 5) return;
    const unsigned before = revision;
    // A confirmation can process slot updates. Retain the complete draft it refers to.
    try {
        const HwLighting::Model snapshot = draft->lighting;
        if(replace){
            QMessageBox dialog(QMessageBox::Warning, tr("Replace lighting layers"),
                tr("Replace all lighting layers in this slot's draft with %1?\n\nThe keyboard changes only after saving the slot.").arg(kindName(kind)),
                QMessageBox::NoButton, this);
            auto* accept = dialog.addButton(tr("Replace layers"), QMessageBox::DestructiveRole);
            dialog.addButton(QMessageBox::Cancel); dialog.setDefaultButton(QMessageBox::Cancel); dialog.setEscapeButton(QMessageBox::Cancel);
            dialog.exec();
            if(dialog.clickedButton() != accept || revision != before || !editable || !draft ||
               !HwLighting::same(snapshot, draft->lighting)) return;
        }
        auto fresh = newLayer(kind);
        int number = 1;
        const QString prefix = defaultName(kind) + ' ';
        for(;; ++number){
            bool used = false;
            for(uint32_t n = 0; n < snapshot.data().count; ++n)
                if(text(snapshot.data().layers[n].name) == prefix + QString::number(number)){ used = true; break; }
            if(!used) break;
        }
        setText(fresh.data().layers[0].name, (prefix + QString::number(number)).toUtf8());
        std::vector<cape_profile_layer> rows{fresh.data().layers[0]};
        if(!replace) for(uint32_t n = 0; n < snapshot.data().count; ++n) rows.push_back(snapshot.data().layers[n]);
        cape_lightmodel model{}; model.known = 1; model.count = uint32_t(rows.size()); model.layers = rows.data();
        HwLighting::Model next(model); draft->lighting.swap(next); ++revision; current = 0;
        refreshList(); emit draftChanged();
    } catch(const std::bad_alloc&){ QMessageBox::warning(this, tr("Hardware lighting"), tr("Not enough memory. Your draft is kept.")); }
}
void HwLightWidget::changeType(int row, int kind){
    if(!editable || !draft || row < 0 || uint32_t(row) >= draft->lighting.data().count || kind < 0 || kind >= CAPE_PROFILE_EFFECTS) return;
    const auto& source = draft->lighting.data().layers[row];
    if(int(source.effect.kind) == kind || (source.effect.kind <= CAPE_PROFILE_RIPPLE) != (kind <= CAPE_PROFILE_RIPPLE)) return;
    const unsigned before = revision;
    try {
        const HwLighting::Model snapshot = draft->lighting;
        QMessageBox dialog(QMessageBox::Warning, tr("Change layer type"),
            tr("Change \"%1\" to %2 and reset its effect settings?\n\nIts name, keys, enabled state and position are kept. The keyboard changes only after saving the slot.")
                .arg(text(source.name), kindName(kind)), QMessageBox::NoButton, this);
        auto* accept = dialog.addButton(tr("Change type"), QMessageBox::DestructiveRole);
        dialog.addButton(QMessageBox::Cancel); dialog.setDefaultButton(QMessageBox::Cancel); dialog.setEscapeButton(QMessageBox::Cancel);
        dialog.exec();
        if(dialog.clickedButton() != accept || revision != before || !editable || !draft || !HwLighting::same(snapshot, draft->lighting)) return;
        auto fresh = newLayer(kind);
        cape_profile_layer replacement = fresh.data().layers[0];
        const auto& saved = snapshot.data().layers[row];
        replacement.id = saved.id; replacement.name = saved.name; replacement.enabled = saved.enabled;
        replacement.keys = saved.keys; replacement.nkeys = saved.nkeys; replacement.option = saved.option;
        std::vector<cape_profile_layer> rows(snapshot.data().layers, snapshot.data().layers + snapshot.data().count);
        rows[size_t(row)] = replacement;
        cape_lightmodel model{}; model.known = 1; model.count = uint32_t(rows.size()); model.layers = rows.data();
        HwLighting::Model next(model); draft->lighting.swap(next); ++revision; current = row;
        refreshList(); emit draftChanged();
    } catch(const std::bad_alloc&){ QMessageBox::warning(this, tr("Hardware lighting"), tr("Not enough memory. Your draft is kept.")); }
}
void HwLightWidget::contextMenu(const QPoint& at){
    auto* item = layers->itemAt(at); if(!editable || !item) return;
    const int row = layers->row(item);
    const unsigned before = revision;
    QMenu menu(this);
    menu.addAction(item->data(EnabledRole).toBool() ? tr("Disable layer") : tr("Enable layer"), this,
                   [this,row,before](){ if(revision == before) toggle(row); });
    menu.addAction(tr("Rename"), this, [this,row,before](){ if(revision == before) rename(row); });
    auto* types = menu.addMenu(tr("Change type..."));
    const int oldKind = item->data(KindRole).toInt();
    for(int kind = oldKind <= CAPE_PROFILE_RIPPLE ? 0 : CAPE_PROFILE_COLOR_PULSE;
        kind < (oldKind <= CAPE_PROFILE_RIPPLE ? CAPE_PROFILE_COLOR_PULSE : CAPE_PROFILE_EFFECTS); ++kind){
        auto* action = types->addAction(kindName(kind)); action->setEnabled(kind != oldKind);
        action->setObjectName(QString("hwLightType%1").arg(kind));
        connect(action, &QAction::triggered, this, [this,row,kind,before](){ if(revision == before) changeType(row,kind); });
    }
    menu.exec(layers->viewport()->mapToGlobal(at));
}

void HwLightWidget::fillPoints(){
    const bool saved = filling; filling = true;
    const int selectedRow = points->currentRow();
    points->clear();
    auto* layer = selected();
    const auto* e = layer && layer->effect.kind >= CAPE_PROFILE_SOLID && layer->effect.kind <= CAPE_PROFILE_RIPPLE ? &layer->effect : nullptr;
    strip->setEffect(e);
    if(!e){ points->setRowCount(0); filling = saved; strip->update(); return; }
    const bool solid = e->kind == CAPE_PROFILE_SOLID;
    points->setColumnCount(solid ? 4 : 3);
    points->setHorizontalHeaderLabels(solid ? QStringList{tr("Start (s)"), tr("End (s)"), tr("Colour"), tr("Intensity (%)")} :
                                            QStringList{tr("Time (s)"), tr("Colour"), tr("Intensity (%)")});
    points->setRowCount(int(e->ntransitions / (solid ? 2 : 1)));
    for(int row = 0; row < points->rowCount(); ++row){
        const auto& t = e->transitions[row * (solid ? 2 : 1)];
        for(int col = 0; col < (solid ? 2 : 1); ++col){
            const double relative = e->transitions[row * (solid ? 2 : 1) + col].time;
            auto* cell = new QTableWidgetItem(QString::number(relative * e->duration / 1000., 'f', 3));
            cell->setToolTip(tr("Relative position: %1").arg(relative, 0, 'g', 17)); points->setItem(row, col, cell);
        }
        const QColor c = colour(t.colour);
        auto* swatch = new QTableWidgetItem(c.name().toUpper());
        QPixmap pix(16,16); pix.fill(c); swatch->setData(Qt::DecorationRole, pix);
        swatch->setFlags(swatch->flags() & ~Qt::ItemIsEditable); points->setItem(row, solid ? 2 : 1, swatch);
        auto* intensity = new QTableWidgetItem(QString::number((t.colour.has_alpha ? t.colour.c[3] : 1.) * 100., 'f', 1));
        points->setItem(row, solid ? 3 : 2, intensity);
    }
    if(points->rowCount()) points->selectRow(std::max(0, std::min(selectedRow, points->rowCount() - 1)));
    removePoint->setEnabled(editable && points->rowCount());
    filling = saved; strip->update();
}

void HwLightWidget::pointColour(int row){
    auto* l = selected(); if(!editable || !l || row < 0) return;
    const int first = row * (l->effect.kind == CAPE_PROFILE_SOLID ? 2 : 1);
    if(uint32_t(first) >= l->effect.ntransitions) return;
    const auto id = l->id;
    points->selectRow(row);
    const QColor c = QColorDialog::getColor(colour(l->effect.transitions[first].colour), this);
    if(!c.isValid() || !selected() || std::memcmp(&id, &selected()->id, sizeof(id))) return;
    if(colour(selected()->effect.transitions[first].colour).rgb() == c.rgb()) return;
    mutate([&](cape_profile_layer& layer){
        const int count = layer.effect.kind == CAPE_PROFILE_SOLID ? 2 : 1;
        for(int n = first; n < first + count; ++n) setRgb(layer.effect.transitions[n].colour, c);
    });
}

void HwLightWidget::setPointTime(int row, int end, double time){
    auto* l = selected(); if(!editable || !l || row < 0 || !std::isfinite(time)) return;
    const bool solid = l->effect.kind == CAPE_PROFILE_SOLID;
    const uint32_t first = uint32_t(row) * (solid ? 2u : 1u);
    if(first >= l->effect.ntransitions) return;
    mutate([&](cape_profile_layer& layer){
        auto& e = layer.effect;
        const double low = first ? e.transitions[first - 1].time : 0.;
        const uint32_t next = first + (solid ? 2u : 1u);
        const double high = next < e.ntransitions ? e.transitions[next].time : 1.;
        if(solid && end == 2){
            const double length = e.transitions[first+1].time - e.transitions[first].time;
            // Imported overlaps are preserved until edited; an impossible free interval cannot be dragged.
            if(high - low < length) return;
            e.transitions[first].time = std::max(low, std::min(time, high - length));
            e.transitions[first+1].time = e.transitions[first].time + length;
        } else if(solid){
            if((end == 0 && low > e.transitions[first+1].time) || (end == 1 && high < e.transitions[first].time)) return;
            e.transitions[first + uint32_t(end)].time = end == 0 ?
                std::max(low, std::min(time, e.transitions[first+1].time)) :
                std::min(high, std::max(time, e.transitions[first].time));
        } else e.transitions[first].time = std::max(low, std::min(time, high));
    });
}

void HwLightWidget::addTransition(){
    auto* l = selected(); if(!editable || !l) return;
    const bool solid = l->effect.kind == CAPE_PROFILE_SOLID;
    const uint32_t count = solid ? 2u : 1u;
    if(l->effect.ntransitions + count > MaxPoints) return;
    mutate([&](cape_profile_layer& layer){
        auto& e = layer.effect;
        uint32_t insert = e.ntransitions;
        double low = e.ntransitions ? e.transitions[e.ntransitions - 1].time : 0., high = 1.;
        if(solid && e.ntransitions){
            // Find a free interval, starting after the selected block and wrapping around.
            const uint32_t blocks = e.ntransitions / 2;
            bool found = false;
            for(uint32_t j = 0; j <= blocks; ++j){
                const uint32_t gap = (uint32_t(std::max(0, points->currentRow()) + 1) + j) % (blocks + 1);
                low = gap ? e.transitions[gap * 2 - 1].time : 0.;
                high = gap < blocks ? e.transitions[gap * 2].time : 1.;
                if(high > low){ insert = gap * 2; found = true; break; }
            }
            if(!found) return;
        }
        auto* transitions = static_cast<cape_profile_transition*>(std::calloc(e.ntransitions + count, sizeof(cape_profile_transition)));
        if(!transitions) throw std::bad_alloc();
        if(insert) std::copy(e.transitions, e.transitions + insert, transitions);
        if(e.ntransitions > insert) std::copy(e.transitions + insert, e.transitions + e.ntransitions, transitions + insert + count);
        cape_profile_colour c{{1.,1.,1.,1.},1};
        const int chosen = points->currentRow();
        if(chosen >= 0 && uint32_t(chosen) * count < e.ntransitions) c = e.transitions[uint32_t(chosen) * count].colour;
        for(uint32_t n = 0; n < count; ++n){
            transitions[insert+n].colour = c;
            transitions[insert+n].time = solid ? e.ntransitions ? low + (high - low) * (n ? .75 : .25) : double(n) : (low + high) / 2.;
        }
        std::free(e.transitions); e.transitions = transitions; e.ntransitions += count;
    });
}

void HwLightWidget::removeTransition(){
    auto* l = selected(); if(!editable || !l || points->currentRow() < 0) return;
    const uint32_t count = l->effect.kind == CAPE_PROFILE_SOLID ? 2u : 1u;
    const uint32_t first = uint32_t(points->currentRow()) * count;
    if(first + count > l->effect.ntransitions) return;
    mutate([&](cape_profile_layer& layer){
        auto& e = layer.effect;
        std::move(e.transitions + first + count, e.transitions + e.ntransitions, e.transitions + first);
        e.ntransitions -= count;
    });
}
