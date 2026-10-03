#ifndef RGBWIDGET_H
#define RGBWIDGET_H

#include <QBitArray>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QOpenGLWidget>
#include "keymap.h"
#include "colormap.h"
#include <cmath>
#include <ckbnextconfig.h>
#include <QElapsedTimer>

class KeyWidget : public QOpenGLWidget
{
    Q_OBJECT
public:
    // New key widget. rgbMode = true to display colors, false to display key names
    KeyWidget(QWidget* parent);
    inline bool     rgbMode()                   { return _rgbMode; }
    inline void     rgbMode(bool newRgbMode)    { _rgbMode = newRgbMode; update(); }
    // For RGB maps, monochrome = true to covert everything to grayscale
    inline bool     monochrome()                { return _monochrome; }
    inline void     monochrome(bool newMono)    { _monochrome = newMono; update(); }

    // Key map
    const KeyMap&       map() const                         { return keyMap; }
    void                map(const KeyMap& newMap);
    // Key -> color map (must contain exactly the keys in the key map)

    const QColorMap&    colorMap() const                    { return _colorMap; }
    void                colorMap(const QColorMap& newColorMap);
    // Key -> binding map
    typedef QHash<QString, QString> BindMap;
    const BindMap&      bindMap() const                     { return _bindMap; }
    void                bindMap(const BindMap& newBindMap);

    // Set current selection (highlighted in blue)
    void setSelection(const QStringList& keys);
    void selectAll();
    void clearSelection();

    // Keys that show a colour of their own and cannot be selected (the indicator buttons of a hardware slot of the K95 RGB
    // Platinum, whose colours are the Performance tab's): the ring of an indicator, tip as their tooltip; an empty map: none
    void setFixedKeys(const QMap<QString, QColor>& keys, const QString& tip);
    const QMap<QString, QColor>& fixedKeys() const { return _fixedKeys; }
    // The colour a key is drawn in with RGB on: the frame of "Show animated" when it has the key, otherwise the colour map's (grey
    // when monochrome); a fixed key's own colour over both
    QRgb shownColor(const QString& key) const;

    // Pick mode (the key pickers of a hardware remap of the K95 RGB Platinum): a click picks one key, with no rectangle
    // and no modifier keys; with toggle, a second click on the key picked takes it back. The proportions are the map's own with
    // the margin around the keys (no allowance for a keyboard), so what is drawn lines up with the widgets next to it. Off by
    // default
    void setPickMode(bool on, bool toggle = false);
    // Keys drawn faded; in pick mode they cannot be picked (what iCUE does not offer for what is being picked)
    void setDimmedKeys(const QStringList& keys);
    const QStringList& dimmedKeys() const { return _dimmedKeys; }

    // Set animated keys (highlighted in green)
    void setAnimation(const QStringList& keys);
    void setAnimationToSelection();
    void clearAnimation();

    bool hasHeightForWidth() const override { return true; }
    inline float aspectRatio() const {
        return _aspectRatio;
    }
    int heightForWidth(int w) const override {
        return std::round(w / aspectRatio());
    }
    void setDebug(bool debug);

public slots:
    // Sets display colors. Pass an empty map to clear.
    // These will be displayed instead of the regular color map, if supplied.
    void displayColorMap(const ColorMap& newDisplayMap, const QSet<QString>& indicators, quint64 renderInterval);

signals:
    // Emitted when the selection is changed.
    void selectionChanged(QStringList selected);
    void sidelightToggled();
    void M95LightToggled();

private:
    KeyMap keyMap;
    QColorMap _colorMap;
    ColorMap _displayColorMap;
    BindMap _bindMap;
    QSet<QString> _indicators;
    QMap<QString, QColor> _fixedKeys;
    QString _fixedTip;
    bool fixedKey(const char* name) const { return !_fixedKeys.isEmpty() && _fixedKeys.contains(QLatin1String(name)); }
    QRgb drawnColor(const char* name) const;
    bool _pickMode = false, _pickToggle = false;
    QStringList _dimmedKeys;
    bool dimmedKey(const char* name) const { return !_dimmedKeys.isEmpty() && _dimmedKeys.contains(QLatin1String(name)); }
    void pick(const QPointF& at);

    QBitArray selection;
    QBitArray newSelection;
    QBitArray animation;
    // These should not be scaled so that the rect renders correctly
    QPointF mouseDown;
    QRectF mouseHighlightRect;
    enum {
        NONE,
        SET,
        ADD,
        SUBTRACT,
        TOGGLE,
    } mouseDownMode;
    bool _rgbMode, _monochrome;

    float _aspectRatio;

    QImage _currentOverlay, _currentOverlayScaled;
    QPointF _overlayPos;

    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

    QPointF drawInfoOffset;
    float drawInfoScale;

    // Helper functions for rendering keys
    void drawLogo(const Key* key, QPainter* decPainter);
    void drawBottomRightCorner(QPainter* painter, float x, float y, float w, float h, float scale);
    void drawBottomLeftCorner(QPainter* painter, float x, float y, float w, float h, float scale);
    void drawTopRightCorner(QPainter* painter, float x, float y, float w, float h, float scale);
    void drawTopLeftCorner(QPainter* painter, float x, float y, float w, float h, float scale);
    void drawStrafeSidelights(const Key* key, QPainter* painter, const QColor& kC, const QColor& c, const QColor& bgC);
    void paintGL() override;
    void initializeGL() override;
    void calculateDrawInfo(const QSize& size);
    void resizeEvent(QResizeEvent* event) override {
        calculateDrawInfo(event->size());
        QOpenGLWidget::resizeEvent(event);
    }
    bool event(QEvent* e) override;
    bool _debug;
#ifdef FPS_COUNTER
    QElapsedTimer glFpsTimer;
    double kbLoopElapsed;
#endif
#ifndef NDEBUG
    QHash<const char*, QRectF> hitboxes;
#endif
};

#endif // RGBWIDGET_H
