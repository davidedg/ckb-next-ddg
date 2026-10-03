#ifndef KBLIGHTWIDGET_H
#define KBLIGHTWIDGET_H

#include <QWidget>
#include <QColor>
#include <QFile>
#include <QResizeEvent>
#include "kblight.h"
#include "keywidget.h"

namespace Ui {
class KbLightWidget;
}

class KbLightWidget : public QWidget
{
    Q_OBJECT

public:
    explicit KbLightWidget(QWidget *parent = nullptr);
    ~KbLightWidget();

    void setLight(KbLight* newLight);
    void setMonochrome();
    void setLegacyM95();
    // Only the base colours are edited (a mode of the hardware profile of a K95 RGB Platinum: animations are software)
    void setStaticOnly(bool on, const QString& why);
    // The keys that show a colour of their own and cannot be selected (KeyWidget::setFixedKeys)
    void setFixedKeys(const QMap<QString, QColor>& keys, const QString& tip);
    // The keyboard takes the light's colours again (a slot of a K95 RGB Platinum read while its mode is shown: Kb colours the light
    // key by key, with no updated() signal, which would also clear the selection)
    void refreshColours();

private slots:
    void updateLight();
    void newSelection(const QStringList& selection);
    void changeColor(const QColor& newColor);
    void changeAnim(KbAnim* newAnim);
    void changeAnimKeys(const QStringList& keys);

    void on_brightnessBox_activated(int index);
    void on_animButton_clicked();

    void on_bgButton_clicked();

    void on_showAnimBox_clicked(bool checked);

    void toggleSidelight(); //strafe
    void toggleM95Light();

    void stateChange(Qt::ApplicationState state);

private:
    KbLight* light;
    QStringList currentSelection;

    Ui::KbLightWidget *ui;
    KeyWidget* keyWidget;

    void startAnimationPreview();
    void stopAnimationPreview();
};

#endif // KBLIGHTWIDGET_H
