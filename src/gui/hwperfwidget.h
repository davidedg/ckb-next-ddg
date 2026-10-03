#ifndef HWPERFWIDGET_H
#define HWPERFWIDGET_H

#include <QWidget>
#include "hwslotdraft.h"

namespace Ui { class HwPerfWidget; }
class ColorButton;
class QCheckBox;
class QLabel;
class QPushButton;

// The Performance tab of a mode of the hardware profile of a K95 RGB Platinum: the performance settings of the slot's draft,
// as iCUE's Performance panel has them, in the layout of ckb-next's software Performance tab (hwperfwidget.ui). Top row: the
// indicator colours (Profile, Brightness at 100%, Windows Lock on and off); When Lock button is on: the four Win Lock options (byte
// 1 of PROFILE.DAT, in iCUE's order); Miscellaneous: Copy performance settings to slot... and Restore defaults. The bits of the
// byte iCUE never writes (4..7) are kept as they are, with a note. Nothing here goes to the daemon: the draft is saved with Save
// slot.
class HwPerfWidget : public QWidget
{
    Q_OBJECT
public:
    explicit HwPerfWidget(QWidget* parent = nullptr);
    ~HwPerfWidget();

    // What it shows (no signal)
    void setPerf(const HwSlotDraft::Perf& perf);
    HwSlotDraft::Perf perf() const;
    // The options and colours can be changed (and restored); the copy is apart
    void setEditable(bool editable);
    void setCopyEnabled(bool enabled);

    // For the tests
    enum Colour { PROFILE, BRIGHTNESS, LOCK_ON, LOCK_OFF };
    ColorButton* colourButton(Colour c) const;
    QCheckBox* option(uint8_t bit) const;           // 0x01 Windows key, 0x02 Alt+Tab, 0x04 Alt+F4, 0x08 Shift+Tab
    QLabel* unknownNote() const { return note; }
    QPushButton* restoreButton() const { return restore; }
    QPushButton* copyButton() const { return copy; }

signals:
    void perfChanged(const HwSlotDraft::Perf& perf);
    void restoreRequested();
    void copyRequested();

private:
    Ui::HwPerfWidget* ui;
    // The widgets of the .ui, by what they are
    ColorButton* colours[4];
    QCheckBox* options[4];
    QLabel* note;
    QPushButton* restore;
    QPushButton* copy;
    uint8_t winlock = HwBinding::DEFAULT_WINLOCK;   // what is shown, bits 4..7 included
    bool filling = false;
    void changed();
    void showNote();
};

#endif // HWPERFWIDGET_H
