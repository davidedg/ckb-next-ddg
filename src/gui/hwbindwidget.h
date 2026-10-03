#ifndef HWBINDWIDGET_H
#define HWBINDWIDGET_H

#include <QWidget>
#include <memory>
#include "keymap.h"
#include "hwbinding.h"
#include "hwbindedit.h"
#include "hwslotdraft.h"

class KeyWidget;
class QMenu;
class QAction;
class HwTextXkb;
namespace Ui { class HwBindWidget; }

// The Binding tab of a mode of the hardware profile of a K95 RGB Platinum (hwslot1): the bindings of the slot's draft, edited in
// place. The keyboard shows what each key does; for the selected keys the action is Default, a Remap (iCUE's four tabs: a key
// of its keyboard, also with 'Imitate holding key', a mouse button, a Shortcut, a Language key), a Macro (a table of events, with its
// start and run), or a Text typed with the layout of the device. Apply puts the action on every selected key after the same checks
// the daemon makes; Reset takes the selected keys back to what the slot has. Nothing here goes to the daemon: the draft is saved
// with Save slot.
class HwBindWidget : public QWidget
{
    Q_OBJECT
public:
    explicit HwBindWidget(QWidget* parent = nullptr);
    ~HwBindWidget();

    enum Type { DEFAULT, REMAP, MACRO, TEXT, KEPT };
    enum RemapTab { KEYBOARD_TAB, MOUSE_TAB, SHORTCUT_TAB, LANGUAGE_TAB };

    // The slot to edit: its base (nullptr: nothing was read), its draft (edited in place; nullptr: nothing to edit), the key map
    // of the keyboard and its layout (KeyMap::getLayout(), for a new Text). editable false: shown only, why says so.
    void setSlot(const HwBinding::Record* base, HwSlotDraft::Draft* draft, const KeyMap& map, const QString& layout,
                 bool editable, const QString& why);
    void setControlsEnabled(bool e);
    // A slot whose bindings are not a model: offer to recreate them from scratch (active: they are being recreated)
    void setRecreate(bool offered, bool active);
    bool recreateOffered() const;

    // What the editor shows and does, also for the tests
    QStringList selection() const { return currentSelection; }
    void select(const QStringList& keys);
    Type type() const;
    void setType(Type t);
    bool apply();
    void reset();
    QString error() const;
    QString notice() const;
    // The pages. setRemap takes a key, a mouse button ("mouse1".."mouse5") or a Language key, and sets the type and the tab that go
    // with it. pickKey is a click on the picker of the tab shown (Keyboard or Shortcut): false for a key it does not offer there
    void setRemap(const QString& key);
    RemapTab remapTab() const;
    void setRemapTab(RemapTab t);
    bool pickKey(const QString& key);
    // 'Imitate holding key' of the Keyboard tab: on, Toggle (else On press), and for how long On press
    void setImitate(bool on, bool toggle, double seconds);
    void setMacroRows(const std::vector<HwBindEdit::Row>& rows, uint8_t start, uint8_t run);
    std::vector<HwBindEdit::Row> macroRows() const { return rows; }
    // The event table: the selected rows as one block (first, count; (-1, 0) for none), and what its context menu, its shortcuts
    // and a drag do with them. A paste goes below the selection, or at the end with none; a drop moves the block before the row
    // `to` of the order before the drag.
    std::pair<int, int> macroSelection() const;
    void selectMacroRows(int first, int count);
    void copyMacroRows();
    void pasteMacroRows();
    void removeMacroRows();
    void moveMacroRows(int by);
    void dropMacroRows(int to);
    QMenu* macroContextMenu() const { return macroMenu; }
    void setShortcut(const QStringList& keys);
    void setText(const QString& text, unsigned delayMs);
    QString textPreview() const;
    bool textAvailable() const { return xkb != nullptr; }
    // The recorder: while recording only Stop is enabled; the events recorded replace the rows of the Macro
    bool recording() const { return isRecording; }
    void setRecording(bool on);

signals:
    // The draft changed (Apply, Reset)
    void draftChanged();
    // Copy the bindings of these keys to other slots
    void copyRequested(const QStringList& keys);
    // Start (true) or stop (false) the recorder
    void recordRequested(bool start);
    // Recreate the bindings from scratch (true), or keep the slot's (false)
    void recreateRequested(bool on);

protected:
    void resizeEvent(QResizeEvent* e) override;
    void showEvent(QShowEvent* e) override;
    bool eventFilter(QObject* watched, QEvent* e) override;
    bool event(QEvent* e) override;
public:
    QSize minimumSizeHint() const override;
protected:

public slots:
    // One key event of the recorder, us after the one before it (MacroReader::macroLineRead)
    void recordedEvent(const QString& key, qint64 us, bool down);

private slots:
    void newSelection(const QStringList& keys);
    void typeChanged(int index);
    void addPress();
    void addRelease();
    void addDelay();
    void addVarDelay();
    void picked(const QStringList& keys);
    void remapTabChanged(int index);
    void imitateChanged();
    void shortcutChanged();
    void clearShortcut();
    void updateTextPreview();
    // The Text field takes TEXT_CHARS_MAX characters (iCUE's field): what typing or pasting adds past that, or past a longer text the
    // field was filled with, is taken out again in the same undo step. fillText() fills the field from the program, never cut
    void limitText();
    void fillText(const QString& text);

private:
    const HwBinding::Record* base = nullptr;
    HwSlotDraft::Draft* draft = nullptr;
    KeyMap keyMap;
    bool editable = false, controls = true, isRecording = false;
    bool recordFull = false;                 // the recorder stopped taking events at the rows of a Macro
    std::vector<bool> recordDown;
    QStringList currentSelection;
    std::vector<HwBindEdit::Row> rows;
    int keyboardPick = -1, shortcutPick = -1;    // the key of each picker (a key index), -1 for none
    bool pickerFilling = false;                  // the picker's selection is set by the program, not clicked
    QStringList languageNames;                   // the entries of the Language keys as in hwbindwidget.ui
    std::unique_ptr<HwTextXkb> xkb;
    QString xkbLayout, layoutName;           // the layout of ckb-next, and its name as the Settings show it
    bool windowsTable = false;               // a table of the Windows layout exists: AltGr only on its keys
    bool textRebuilt = false;                // the Text shown was rebuilt from the slot's events
    int textAccepted = 0;                    // characters of the Text field after its last change: it may shrink, not grow past TEXT_CHARS_MAX
    QString textPreviewText;

    KeyWidget* keyWidget;                    // the device
    KeyWidget* pickerWidget;                 // iCUE's keyboard, in the Keyboard or the Shortcut tab
    Ui::HwBindWidget* ui;
    int rowSpacing = 6;
    enum { KEYS_MIN = 60 };                  // the smallest height of the keyboard
    QMenu* macroMenu;
    QAction *copyAction, *pasteAction, *upAction, *downAction, *removeAction;
    std::vector<HwBindEdit::Row> macroClipboard;           // Copy of the context menu: inside the editor, not the system's
    bool recreateActive = false;

    int indexOf(const QString& key) const;
    const HwBinding::Action* baseAction(int index) const;
    void loadPanel();
    void refreshKeys();
    void refreshMacroList();
    void updateMacroActions();
    int selectedDelays() const;
    int insertAt() const;
    void addRow(const HwBindEdit::Row& row);
    void uniformControls();
    void fitKeyboard();
    int pickerRoom(int room) const;
    QSize pickerAround() const;
    void arrange();
    QString textRefusal(const std::u32string& rejected, const std::u32string& notOnWindows) const;
    void resetRemap();
    void placePicker();
    void refreshShortcut();
    void fillShortcut(const QStringList& keys);
    std::vector<uint8_t> shortcutKeys() const;
    void updateEnabled();
    void fail(const QString& why);
    bool build(HwBinding::Action& out);
    QString keyName(unsigned index) const;
};

#endif // HWBINDWIDGET_H
