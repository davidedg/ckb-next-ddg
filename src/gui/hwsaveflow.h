#ifndef HWSAVEFLOW_H
#define HWSAVEFLOW_H

#include <cstdint>
#include <string>
#include <vector>

// The save through hwslot1: open (1). A build with K95P_HWSLOT_SAVE 0 checks the slots and never saves them (a build for a trial
// that must not write).
#ifndef K95P_HWSLOT_SAVE
#define K95P_HWSLOT_SAVE 1
#endif

// K95 Platinum save transaction, independent of Qt and of the FIFO implementation.
// The caller owns the notification node exclusively for the life of this object.
// Transaction IDs below are local send/dialog IDs, not IDs on the daemon wire.
class HwSaveFlow {
public:
    struct Slot {
        unsigned mode = 0;                 // GUI mode, 1..3
        std::vector<std::string> prepare; // complete, validated daemon command lines, including newline
        bool animations = false;
        bool invisible_keys = false;
        bool many_colours = false;         // warning only; check decides whether reconstruction is possible
        // hwslot1: the whole "@N hwslot check:/save:/abort:" lines of the slot's transaction (prepare holds its
        // "@N hwslot" preparation lines). Either every slot has them or none (hwsave: "get :hwsavecheck" and "hwsave").
        std::string check_line, save_line, abort_line;
    };
    enum EventKind { Send, Dialog, Progress, Refresh, Summary, ChannelUncertain };
    enum DialogKind { Confirm, Failed, Unknown };
    static const bool hwslot_save_enabled = K95P_HWSLOT_SAVE != 0;
    enum SendResult { Sent, NotSent, Uncertain };
    struct Event {
        EventKind kind = Summary;
        uint64_t id = 0;
        unsigned mode = 0;
        unsigned done = 0;                 // 0..1000; 1000 only after a terminal response
        DialogKind dialog = Confirm;
        std::string text;                  // command, dialog code/parameters or phase
    };
    struct Outcome {
        unsigned mode = 0;
        std::string verdict;              // written, unchanged, empty, refused, recovery, failed, not-tried
        std::string detail;
        bool hwmode_failed = false;        // the keyboard did not go back to hardware mode after the save (hwslot1)
        // Not enough free sectors in the flash ("space=<need>/<free>" of a fail line): nothing was written, and a Retry would give the
        // same, so no Retry is asked
        bool no_space = false;
        unsigned need_sectors = 0, free_sectors = 0;
        Outcome() = default;
        Outcome(unsigned m, const std::string& v, const std::string& d) : mode(m), verdict(v), detail(d) {}
    };

    bool start(const std::vector<Slot>& slots, bool all, int64_t now_ms);
    void send_result(uint64_t id, SendResult result, int64_t now_ms);
    void retry_send(int64_t now_ms);      // only after a proven zero-byte NotSent
    void line(const std::string& line, int64_t now_ms);
    void choose(uint64_t id, bool accept, int64_t now_ms);
    void tick(int64_t now_ms);
    void refresh_result(bool complete);
    // A line of the refresh was accepted: its guard (30 s without one) starts again
    void refresh_alive(int64_t now_ms);
    void device_gone();
    std::vector<Event> take_events();
    bool busy() const { return state_ != Idle && state_ != Done; }
    bool uncertain() const { return state_ == Broken; }
    const std::vector<Outcome>& outcomes() const { return outcomes_; }
    bool refresh_failed() const { return refresh_failed_; }
    // hwslot1 with the save shut in this build: the slots were checked only (verdict "checked", the check in the detail)
    bool check_only() const { return check_only_; }
    // One outcome as the summary shows it: "checked(same) colours=0 layers=1", "checked(changed) …" for a check that would write,
    // "<verdict>: <detail>" for the others ("failed: <err>", "refused: <reason>"; the verdict alone without a detail)
    static std::string outcome_text(const Outcome& o);
    // The word of an outcome that says what it means: the check's own for a check (same, changed, new, refused), else the verdict;
    // and the parts a check found changed (its "<part>=changed" fields, in order)
    static std::string outcome_word(const Outcome& o);
    static std::vector<std::string> changed_parts(const Outcome& o);

private:
    enum State { Idle, Preparing, Checking, Asking, Saving, WaitingUnknown, Refreshing, Aborting, Broken, Done };
    enum CommandKind { PrepareCommand, CheckCommand, SaveCommand, AbortCommand };
    struct Check {
        std::string verdict;
        std::string detail;
        std::string replaces;
        unsigned ignored = 0;
        bool kept = false;
        bool buttons_changed = false;
        bool name_changed = false;
        bool bindings_changed = false;
        bool lights_kept = false;
        std::string text;                  // what the check said, after "hwsavecheck "
    };
    State state_ = Idle;
    CommandKind command_kind_ = PrepareCommand;
    std::vector<Slot> slots_;
    std::vector<Check> checks_;
    std::vector<Outcome> outcomes_;
    std::vector<unsigned> approved_;
    std::vector<Event> events_;
    size_t cursor_ = 0, prep_cursor_ = 0, save_cursor_ = 0;
    unsigned completed_ = 0, last_progress_ = 0, last_beat_ = 0;   // last_beat_: the heartbeat of the check under way
    uint64_t next_id_ = 1, send_id_ = 0, dialog_id_ = 0;
    int64_t deadline_ = 0, now_ = 0;
    DialogKind dialog_kind_ = Confirm;
    bool all_ = false, send_pending_ = false, waiting_send_ = false;
    bool waiting_reply_ = false, aborted_ = false, wrote_ = false, refresh_failed_ = false;
    bool staged_ = false, check_only_ = false;
    std::vector<size_t> aborts_;           // slots whose transaction is still to be forgotten before the summary
    std::string send_line_;
    std::string waiting_type_;

    void queue_event(EventKind kind, unsigned mode = 0, const std::string& text = std::string(), uint64_t id = 0);
    void command(const std::string& line, CommandKind kind, int64_t now_ms);
    void next_prepare(int64_t now_ms);
    void next_check(int64_t now_ms);
    void after_checks(int64_t now_ms);
    void ask(DialogKind kind, const std::string& text);
    void next_save(int64_t now_ms);
    void after_terminal(int64_t now_ms);
    void finish();
    void next_abort(int64_t now_ms);
    void done();
    void unknown(const std::string& why);
};

#endif
