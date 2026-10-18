#include "hwperfwidget.h"
#include "ui_hwperfwidget.h"
#include "colorbutton.h"

namespace {

// The Win Lock options in iCUE's order, as the check boxes of the .ui are
const uint8_t OPTION_BITS[4] = {0x02, 0x04, 0x08, 0x01};

QColor colourAt(const std::array<uint8_t, 12>& ind, int n){
    return QColor(ind[size_t(3 * n)], ind[size_t(3 * n + 1)], ind[size_t(3 * n + 2)]);
}

} // namespace

HwPerfWidget::HwPerfWidget(QWidget* parent) : QWidget(parent), ui(new Ui::HwPerfWidget){
    ui->setupUi(this);
    colours[PROFILE] = ui->profileColor;
    colours[BRIGHTNESS] = ui->brightnessColor;
    colours[LOCK_ON] = ui->lockColorOn;
    colours[LOCK_OFF] = ui->lockColorOff;
    options[0] = ui->altTabBox;
    options[1] = ui->altF4Box;
    options[2] = ui->shiftTabBox;
    options[3] = ui->winKeyBox;
    copy = ui->copyButton;
    restore = ui->restoreButton;

    for(int n = 0; n < 4; ++n){
        // (the look of the swatch; not properties the .ui can set)
        colours[n]->setLabel(false);
        colours[n]->bigIcons(true);
        connect(colours[n], &ColorButton::colorChanged, this, [this](QColor){ changed(); });
        connect(options[n], &QCheckBox::toggled, this, [this](bool){ changed(); });
    }
    connect(copy, &QPushButton::clicked, this, &HwPerfWidget::copyRequested);
    connect(restore, &QPushButton::clicked, this, &HwPerfWidget::restoreRequested);
    setPerf(HwSlotDraft::Perf());
}

HwPerfWidget::~HwPerfWidget(){
    delete ui;
}

ColorButton* HwPerfWidget::colourButton(Colour c) const {
    return colours[c];
}

QCheckBox* HwPerfWidget::option(uint8_t bit) const {
    for(int n = 0; n < 4; ++n)
        if(OPTION_BITS[n] == bit)
            return options[n];
    return nullptr;
}

void HwPerfWidget::setPerf(const HwSlotDraft::Perf& perf){
    filling = true;
    keptWinlock = perf.winlock & 0x10;
    for(int n = 0; n < 4; ++n){
        colours[n]->color(colourAt(perf.indicators, n));
        options[n]->setChecked(perf.winlock & OPTION_BITS[n]);
    }
    filling = false;
}

HwSlotDraft::Perf HwPerfWidget::perf() const {
    HwSlotDraft::Perf p;
    p.winlock = keptWinlock;   // bit 4, iCUE's encryption option, is kept as the slot had it; iCUE writes none of 5 to 7
    for(int n = 0; n < 4; ++n)
        if(options[n]->isChecked())
            p.winlock |= OPTION_BITS[n];
    for(int n = 0; n < 4; ++n){
        const QColor c = colours[n]->color();
        p.indicators[size_t(3 * n)] = uint8_t(c.red());
        p.indicators[size_t(3 * n + 1)] = uint8_t(c.green());
        p.indicators[size_t(3 * n + 2)] = uint8_t(c.blue());
    }
    return p;
}

void HwPerfWidget::setEditable(bool editable){
    for(int n = 0; n < 4; ++n){
        colours[n]->setEnabled(editable);
        options[n]->setEnabled(editable);
    }
    restore->setEnabled(editable);
}

void HwPerfWidget::setCopyEnabled(bool enabled){
    copy->setEnabled(enabled);
}

void HwPerfWidget::changed(){
    if(filling)
        return;
    emit perfChanged(perf());
}
