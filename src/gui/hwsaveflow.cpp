#include "hwsaveflow.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <sstream>
#include <unordered_map>

namespace {
struct Reply {
    unsigned mode = 0, done = 0;
    std::string type, verdict, phase;
    std::unordered_map<std::string, std::string> fields;
    bool valid = false;
};

bool number(const std::string& s, unsigned& value) {
    if(s.empty()) return false;
    unsigned n = 0;
    for(char c : s) {
        if(c < '0' || c > '9' || n > (UINT_MAX - unsigned(c - '0')) / 10) return false;
        n = n * 10 + unsigned(c - '0');
    }
    value = n;
    return true;
}

bool field(const Reply& r, const char* key, const char* value = nullptr) {
    auto p = r.fields.find(key);
    return p != r.fields.end() && (!value || p->second == value);
}

// A complete notification line only. In particular, "hwsave progress" is never a terminal.
Reply parse(const std::string& raw) {
    Reply r;
    if(raw.find('\n') != std::string::npos && raw.find('\n') != raw.size() - 1) return r;
    std::string s = raw;
    if(!s.empty() && s.back() == '\n') s.pop_back();
    std::istringstream in(s);
    std::string word, mode;
    if(!(in >> word) || word != "mode" || !(in >> mode) || !number(mode, r.mode) ||
       !(in >> r.type) || !(in >> r.verdict)) return r;
    if(r.type != "hwsave" && r.type != "hwsavecheck") return r;
    // (a check sends "hwsavecheck progress" lines while it reads: a heartbeat)
    if(r.verdict == "progress") {
        std::string done, total, extra;
        unsigned t;
        if(!(in >> done >> total >> r.phase) || (in >> extra) || !number(done, r.done) ||
           !number(total, t) || t != 1000 || r.done > t ||
           (r.phase != "read" && r.phase != "write" && r.phase != "verify")) return r;
        r.valid = true;
        return r;
    }
    std::string token;
    while(in >> token) {
        const size_t eq = token.find('=');
        if(eq == std::string::npos || eq == 0) return r;
        const std::string key = token.substr(0, eq);
        std::string value = token.substr(eq + 1);
        if(key == "reason" || key == "err") {
            std::string tail;
            std::getline(in, tail);
            value += tail;
        }
        const std::string known = "|colours|layers|ignored|packets|kept|buttons|name|replaces|reason|err|stage|file|slot|"
                                  "bindings|lights|hwmode|space|";
        const std::string known_perf = "|winlock|indicators|";   // (the performance settings)
        if(r.fields.count(key) && known.find("|" + key + "|") != std::string::npos) return r;
        if(r.fields.count(key) && known_perf.find("|" + key + "|") != std::string::npos) return r;
        r.fields[key] = value;
    }
    unsigned n;
    for(const char* key : {"colours", "layers", "ignored", "packets"})
        if(field(r, key) && !number(r.fields[key], n)) return r;
    if(field(r, "kept") && !field(r, "kept", "yes") && !field(r, "kept", "no")) return r;
    if(field(r, "slot") && !field(r, "slot", "unchanged") &&
       !field(r, "slot", "incomplete") && !field(r, "slot", "written")) return r;
    if(field(r, "buttons") && !field(r, "buttons", "changed")) return r;
    if(field(r, "name") && !field(r, "name", "changed")) return r;
    if(field(r, "bindings") && !field(r, "bindings", "changed")) return r;
    if(field(r, "winlock") && !field(r, "winlock", "changed")) return r;
    if(field(r, "indicators") && !field(r, "indicators", "changed")) return r;
    if(field(r, "lights") && !field(r, "lights", "kept")) return r;
    if(field(r, "hwmode") && !field(r, "hwmode", "failed")) return r;
    if(field(r, "space")) {
        const std::string& v = r.fields["space"];
        const size_t slash = v.find('/');
        if(slash == std::string::npos || !number(v.substr(0, slash), n) || !number(v.substr(slash + 1), n)) return r;
    }
    if(field(r, "replaces")) {
        const std::string& v = r.fields["replaces"];
        if(v != "effects" && v != "unknown" && v != "effects,unknown" && v != "unknown,effects") return r;
    }
    if(r.type == "hwsavecheck") {
        if(r.verdict == "error") r.valid = field(r, "err") && !r.fields["err"].empty();
        else if(r.verdict == "same" || r.verdict == "write" || r.verdict == "new" || r.verdict == "refused")
            r.valid = field(r, "colours") && field(r, "layers") &&
                      (r.verdict != "refused" || field(r, "reason"));
    } else if(r.verdict == "ok") r.valid = field(r, "packets");
    else if(r.verdict == "skipped")
        r.valid = field(r, "refused"); // daemon sends a bare "refused", handled below
    else if(r.verdict == "fail")
        r.valid = field(r, "stage") && field(r, "file") && field(r, "slot") &&
                  field(r, "kept") && field(r, "err") && !r.fields["err"].empty();
    return r;
}

// The skipped line has a bare subtype before any key=value fields.
Reply parse_save_skipped(const std::string& raw) {
    std::string s = raw;
    if(!s.empty() && s.back() == '\n') s.pop_back();
    std::istringstream in(s);
    std::string a, mode, type, status, subtype, rest;
    Reply r;
    if(!(in >> a >> mode >> type >> status >> subtype) || a != "mode" ||
       !number(mode, r.mode) || type != "hwsave" || status != "skipped" ||
       subtype != "refused") return r;
    r.type = type;
    r.verdict = "skipped";
    r.phase = subtype;
    if(!(in >> rest)) return r;
    if(rest == "hwmode=failed") { // a save from hardware mode (hwslot1) that could not switch back
        r.fields["hwmode"] = "failed";
        if(!(in >> rest)) return r;
    }
    if(rest.compare(0, 7, "reason=") != 0) return r;
    std::string tail;
    std::getline(in, tail);
    r.fields["reason"] = rest.substr(7) + tail;
    r.valid = !r.fields["reason"].empty();
    return r;
}
}

void HwSaveFlow::queue_event(EventKind kind, unsigned mode, const std::string& text, uint64_t id) {
    Event e;
    e.kind = kind; e.mode = mode; e.text = text; e.id = id;
    events_.push_back(e);
}

bool HwSaveFlow::start(const std::vector<Slot>& slots, bool all, int64_t now_ms) {
    // Save ALL: the slots in order, one to three (hwslot1 leaves out a slot not read from the keyboard and not changed)
    if(busy() || slots.empty() || slots.size() > 3u || (!all && slots.size() != 1u)) return false;
    const bool staged = !slots[0].check_line.empty();
    bool seen[4] = {false, false, false, false};
    for(size_t i = 0; i < slots.size(); ++i) {
        const Slot& s = slots[i];
        if(s.mode < 1 || s.mode > 3 || seen[s.mode]) return false;
        if(all && i > 0 && s.mode <= slots[i - 1].mode) return false;
        seen[s.mode] = true;
        std::vector<std::string> lines = s.prepare;
        if(staged) { lines.push_back(s.check_line); lines.push_back(s.save_line); lines.push_back(s.abort_line); }
        else if(!s.check_line.empty() || !s.save_line.empty() || !s.abort_line.empty()) return false;
        for(const std::string& command : lines)
            if(command.empty() || command.back() != '\n' || command.find('\n') != command.size() - 1) return false;
    }
    slots_ = slots; all_ = all; checks_.assign(slots.size(), Check());
    outcomes_.clear();
    for(const Slot& s : slots_) outcomes_.push_back(Outcome(s.mode, "not-tried", ""));
    approved_.clear(); events_.clear(); cursor_ = prep_cursor_ = save_cursor_ = 0;
    completed_ = last_progress_ = 0; send_id_ = dialog_id_ = 0;
    send_pending_ = waiting_send_ = waiting_reply_ = aborted_ = wrote_ = refresh_failed_ = false;
    staged_ = staged; check_only_ = staged && !hwslot_save_enabled; aborts_.clear(); now_ = now_ms;
    state_ = Preparing;
    next_prepare(now_ms);
    return true;
}

void HwSaveFlow::command(const std::string& line, CommandKind kind, int64_t now_ms) {
    command_kind_ = kind;
    send_line_ = line;
    send_pending_ = true;
    waiting_send_ = false;
    send_id_ = next_id_++;
    now_ = now_ms;
    deadline_ = now_ms + 30000;
    queue_event(Send, slots_[cursor_].mode, line, send_id_);
}

void HwSaveFlow::next_prepare(int64_t now_ms) {
    while(cursor_ < slots_.size() && prep_cursor_ == slots_[cursor_].prepare.size()) {
        ++cursor_; prep_cursor_ = 0;
    }
    if(cursor_ == slots_.size()) { cursor_ = 0; next_check(now_ms); return; }
    command(slots_[cursor_].prepare[prep_cursor_], PrepareCommand, now_ms);
}

void HwSaveFlow::next_check(int64_t now_ms) {
    if(cursor_ == slots_.size()) { after_checks(now_ms); return; }
    state_ = Checking; last_beat_ = 0;
    command(staged_ ? slots_[cursor_].check_line :
            "mode " + std::to_string(slots_[cursor_].mode) + " get :hwsavecheck\n", CheckCommand, now_ms);
}

void HwSaveFlow::send_result(uint64_t id, SendResult result, int64_t now_ms) {
    if(!send_pending_ || id != send_id_ || state_ == Broken || state_ == Done) return;
    now_ = now_ms;
    send_pending_ = false;
    if(result == Uncertain) {
        state_ = Broken; queue_event(ChannelUncertain, slots_[cursor_].mode, "partial-or-unknown-delivery"); return;
    }
    if(result == NotSent) { waiting_send_ = true; return; }
    if(command_kind_ == PrepareCommand) { ++prep_cursor_; next_prepare(now_ms); return; }
    if(command_kind_ == AbortCommand) { next_abort(now_ms); return; }
    waiting_reply_ = true;
    waiting_type_ = command_kind_ == CheckCommand ? "hwsavecheck" : "hwsave";
    deadline_ = now_ms + 30000;
}

void HwSaveFlow::retry_send(int64_t now_ms) {
    if(!waiting_send_ || state_ == Broken || state_ == Done) return;
    now_ = now_ms;
    if(now_ms >= deadline_) { tick(now_ms); return; }
    waiting_send_ = false; send_pending_ = true; send_id_ = next_id_++;
    queue_event(Send, slots_[cursor_].mode, send_line_, send_id_);
}

void HwSaveFlow::ask(DialogKind kind, const std::string& text) {
    state_ = Asking; dialog_kind_ = kind; dialog_id_ = next_id_++;
    Event e; e.kind = Dialog; e.mode = slots_[cursor_].mode; e.dialog = kind; e.text = text; e.id = dialog_id_;
    events_.push_back(e);
}

std::string HwSaveFlow::outcome_text(const Outcome& o) {
    if(o.verdict == "checked") {
        // the check's own verdict word goes in brackets, so that "same colours=0" does not read as "0 colours are the same"
        const size_t space = o.detail.find(' ');
        std::string word = o.detail.substr(0, space);
        if(word == "write") word = "changed";
        return "checked(" + word + ")" + (space == std::string::npos ? std::string() : o.detail.substr(space));
    }
    return o.detail.empty() ? o.verdict : o.verdict + ": " + o.detail;   // "failed: <err>", "refused: <reason>"
}

std::string HwSaveFlow::outcome_word(const Outcome& o) {
    if(o.verdict != "checked") return o.verdict;
    const std::string word = o.detail.substr(0, o.detail.find(' '));
    return word == "write" ? "changed" : word;
}

std::vector<std::string> HwSaveFlow::changed_parts(const Outcome& o) {
    std::vector<std::string> parts;
    std::istringstream in(o.detail);
    std::string field;
    const std::string suffix = "=changed";
    while(in >> field)
        if(field.size() > suffix.size() && field.compare(field.size() - suffix.size(), suffix.size(), suffix) == 0)
            parts.push_back(field.substr(0, field.size() - suffix.size()));
    return parts;
}

void HwSaveFlow::after_checks(int64_t now_ms) {
    approved_.clear();
    if(check_only_) {
        // The save is shut in this build: what each check said, and nothing written
        for(size_t i = 0; i < slots_.size(); ++i) outcomes_[i] = Outcome(slots_[i].mode, "checked", checks_[i].text);
        finish();
        return;
    }
    std::string warnings;
    for(size_t i = 0; i < slots_.size(); ++i) {
        const Check& c = checks_[i];
        std::string verdict = c.verdict;
        if(verdict == "refused") verdict = "refused";
        else if(all_ && c.kept) verdict = "recovery";
        else if(all_ && verdict == "same") verdict = "unchanged";
        else if(all_ && verdict == "new") verdict = "empty";
        else { approved_.push_back(unsigned(i)); verdict = "not-tried"; }
        outcomes_[i] = Outcome(slots_[i].mode, verdict, c.detail);
        if(verdict == "not-tried") {
            std::string w;
            if(!c.lights_kept) { // the colours of the GUI are not written when the slot's lighting is kept
                if(slots_[i].animations) w += "animations,";
                if(slots_[i].invisible_keys) w += "invisible-keys,";
                if(slots_[i].many_colours) w += "many-colours,";
            }
            if(c.replaces.find("effects") != std::string::npos) w += "replaces-effects,";
            if(c.replaces.find("unknown") != std::string::npos) w += "replaces-unknown,";
            if(c.ignored) w += "ignored=" + std::to_string(c.ignored) + ",";
            if(c.buttons_changed) w += "buttons-changed,";
            if(c.name_changed && !w.empty()) w += "name-changed-info,";
            if(!w.empty()) warnings += "mode " + std::to_string(slots_[i].mode) + ":" + w + ";";
        }
    }
    if(approved_.empty()) { finish(); return; }
    cursor_ = approved_[0];
    if(!all_ && checks_[cursor_].kept) warnings += "recovery-required;";
    if(!all_ && checks_[cursor_].verdict == "same") warnings += "rewrite-same;";
    if(!all_ && checks_[cursor_].verdict == "new") warnings += "create-new;";
    if(!warnings.empty()) { ask(Confirm, warnings); return; }
    next_save(now_ms);
}

void HwSaveFlow::next_save(int64_t now_ms) {
    if(save_cursor_ == approved_.size()) { after_terminal(now_ms); return; }
    if(check_only_) { finish(); return; } // never a save: line in a build where the save is shut
    cursor_ = approved_[save_cursor_]; state_ = Saving; last_progress_ = 0;
    command(staged_ ? slots_[cursor_].save_line :
            "mode " + std::to_string(slots_[cursor_].mode) + " hwsave\n", SaveCommand, now_ms);
}

void HwSaveFlow::choose(uint64_t id, bool accept, int64_t now_ms) {
    if((state_ != Asking && state_ != WaitingUnknown) || id != dialog_id_) return;
    now_ = now_ms;
    dialog_id_ = 0;
    if(dialog_kind_ == Unknown) { aborted_ = true; return; } // still wait for the daemon terminal
    if(dialog_kind_ == Confirm) {
        if(accept) next_save(now_ms);
        else {
            for(unsigned i : approved_) outcomes_[i].verdict = "cancelled";
            finish();
        }
        return;
    }
    if(!accept) { aborted_ = true; after_terminal(now_ms); return; }
    next_save(now_ms); // explicit Retry of the failed slot; never a second first attempt
}

void HwSaveFlow::unknown(const std::string& why) {
    if(state_ == WaitingUnknown || state_ == Broken) return;
    if(waiting_type_ == "hwsave" && save_cursor_ < approved_.size())
        approved_.resize(save_cursor_ + 1); // a late terminal never restarts the rest of Save ALL
    ask(Unknown, why);
    state_ = WaitingUnknown; // dialog displayed while the daemon terminal is still outstanding
}

void HwSaveFlow::line(const std::string& raw, int64_t now_ms) {
    if(!waiting_reply_ || state_ == Broken || state_ == Done) return;
    now_ = now_ms;
    Reply r = parse(raw);
    if(r.type == "hwsave" && r.verdict == "skipped") r = parse_save_skipped(raw);
    if(r.type.empty()) {
        // A malformed line of the awaited type is unsafe, even if the full parser
        // cannot recover a verdict from it. Lines for other modes/types are noise.
        std::istringstream head(raw);
        std::string keyword, mode, type;
        unsigned n;
        if(head >> keyword >> mode >> type && keyword == "mode" && number(mode, n) &&
           n == slots_[cursor_].mode && type == waiting_type_) unknown("malformed-" + waiting_type_);
        return;
    }
    if(r.mode != slots_[cursor_].mode || r.type != waiting_type_) return;
    if(waiting_type_ == "hwsavecheck" && r.verdict == "progress") {
        // The heartbeat of a check that reads a slot: it renews the guard, and moves the bar over the checks (phase "check"; the
        // save that may follow starts the bar again from 0; without it the bar would stand at 0 for the seconds of a check)
        if(!r.valid || r.phase != "read" || r.done < last_beat_) { unknown("invalid-progress"); return; }
        if(state_ == WaitingUnknown) return;
        last_beat_ = r.done; deadline_ = now_ms + 30000;
        Event e; e.kind = Progress; e.mode = r.mode;
        e.done = std::min(999u, (unsigned(cursor_) * 1000 + r.done) / unsigned(slots_.size()));
        e.text = "check"; events_.push_back(e);
        return;
    }
    if(waiting_type_ == "hwsave" && r.verdict == "progress") {
        if(!r.valid || r.done < last_progress_) { unknown("invalid-progress"); return; }
        if(state_ == WaitingUnknown) return;
        last_progress_ = r.done; deadline_ = now_ms + 30000;
        Event e; e.kind = Progress; e.mode = r.mode; e.done =
            approved_.empty() ? 0 : std::min(999u, (completed_ * 1000 + r.done) / unsigned(approved_.size()));
        e.text = r.phase; events_.push_back(e);
        return;
    }
    if(!r.valid) { unknown("malformed-" + waiting_type_); return; }
    waiting_reply_ = false;
    if(waiting_type_ == "hwsavecheck") {
        if(state_ == WaitingUnknown) { aborted_ = true; finish(); return; }
        if(r.verdict == "error") { outcomes_[cursor_] = Outcome(r.mode, "failed", r.fields["err"]); finish(); return; }
        Check& c = checks_[cursor_]; c.verdict = r.verdict; c.kept = field(r, "kept", "yes");
        c.replaces = field(r, "replaces") ? r.fields["replaces"] : "";
        if(field(r, "ignored")) number(r.fields["ignored"], c.ignored); // already validated by parse()
        c.buttons_changed = field(r, "buttons", "changed");
        c.name_changed = field(r, "name", "changed");
        c.bindings_changed = field(r, "bindings", "changed");
        c.lights_kept = field(r, "lights", "kept");
        c.detail = field(r, "reason") ? r.fields["reason"] : "";
        const size_t at = raw.find(" hwsavecheck ");
        c.text = raw.substr(at + 13);
        if(!c.text.empty() && c.text.back() == '\n') c.text.pop_back();
        ++cursor_; next_check(now_ms);
        return;
    }
    outcomes_[cursor_].hwmode_failed = field(r, "hwmode", "failed");
    if(r.verdict == "ok") { outcomes_[cursor_].verdict = "written"; wrote_ = true; ++completed_; }
    else if(r.verdict == "skipped") {
        outcomes_[cursor_].verdict = "refused";
        outcomes_[cursor_].detail = field(r, "reason") ? r.fields["reason"] : "";
        ++completed_;
    } else {
        outcomes_[cursor_].verdict = "failed"; outcomes_[cursor_].detail = r.fields["err"];
        if(state_ == WaitingUnknown && aborted_) { after_terminal(now_ms); return; }
        if(field(r, "space") && field(r, "slot", "unchanged")) {
            // Not enough room: nothing was written and trying again gives the same, so no Retry; Save ALL goes on with the next slot
            Outcome& o = outcomes_[cursor_];
            const std::string& v = r.fields["space"];
            o.no_space = true;
            number(v.substr(0, v.find('/')), o.need_sectors);   // (validated by parse())
            number(v.substr(v.find('/') + 1), o.free_sectors);
            if(state_ == WaitingUnknown) { aborted_ = true; after_terminal(now_ms); return; }
            ++save_cursor_;
            if(aborted_) after_terminal(now_ms);
            else next_save(now_ms);
            return;
        }
        ask(Failed, "stage=" + r.fields["stage"] + " file=" + r.fields["file"] +
             " slot=" + r.fields["slot"] + " kept=" + r.fields["kept"] +
             (outcomes_[cursor_].hwmode_failed ? " hwmode=failed" : "") + " err=" + r.fields["err"]);
        return;
    }
    if(!approved_.empty()) {
        Event e; e.kind = Progress; e.mode = r.mode;
        e.done = (completed_ * 1000) / unsigned(approved_.size());
        e.text = "terminal"; events_.push_back(e);
    }
    if(state_ == WaitingUnknown) { aborted_ = true; after_terminal(now_ms); return; }
    ++save_cursor_;
    if(aborted_) after_terminal(now_ms);
    else next_save(now_ms);
}

void HwSaveFlow::tick(int64_t now_ms) {
    if(state_ == Broken || state_ == Done || state_ == Asking || state_ == WaitingUnknown) return;
    now_ = now_ms;
    if(waiting_send_ && now_ms >= deadline_ && command_kind_ == AbortCommand) {
        // Not delivered: the daemon forgets the transaction when this node closes or the slot is begun again
        waiting_send_ = false; aborts_.clear(); done();
    } else if(waiting_send_ && now_ms >= deadline_) {
        waiting_send_ = false; aborted_ = true;
        if(state_ == Saving && !outcomes_.empty()) {
            outcomes_[cursor_].verdict = "failed";
            outcomes_[cursor_].detail = "send-not-delivered";
        }
        after_terminal(now_ms); // zero bytes delivered: no daemon request is pending
    } else if(send_pending_ && now_ms >= deadline_) {
        state_ = Broken; queue_event(ChannelUncertain, slots_[cursor_].mode, "send-timeout");
    } else if(waiting_reply_ && now_ms >= deadline_) unknown("reply-timeout-" + waiting_type_);
}

void HwSaveFlow::after_terminal(int64_t now_ms) {
    if(wrote_) {
        state_ = Refreshing; waiting_reply_ = true; waiting_type_ = "refresh";
        deadline_ = now_ms + 30000;
        queue_event(Refresh, 0, "successful-slots-only");
    }
    else finish();
}

void HwSaveFlow::refresh_alive(int64_t now_ms) {
    // A line of the read-back came: the guard of the whole refresh starts again (a big slot has more than a thousand pages)
    if(state_ == Refreshing && waiting_reply_) deadline_ = now_ms + 30000;
}

void HwSaveFlow::refresh_result(bool complete) {
    if(state_ != Refreshing && !(state_ == WaitingUnknown && waiting_type_ == "refresh")) return;
    waiting_reply_ = false; refresh_failed_ = !complete; finish();
}

void HwSaveFlow::finish() {
    if(state_ == Done || state_ == Aborting) return;
    // hwslot1: every transaction that a save did not consume is forgotten, before the summary gives the channel back
    aborts_.clear();
    if(staged_)
        for(size_t i = 0; i < slots_.size(); ++i)
            if(outcomes_[i].verdict != "written") aborts_.push_back(i);
    waiting_reply_ = false;
    if(aborts_.empty()) { done(); return; }
    state_ = Aborting;
    next_abort(now_);
}

void HwSaveFlow::next_abort(int64_t now_ms) {
    if(aborts_.empty()) { done(); return; }
    cursor_ = aborts_.front();
    aborts_.erase(aborts_.begin());
    command(slots_[cursor_].abort_line, AbortCommand, now_ms);
}

void HwSaveFlow::done() {
    state_ = Done;
    queue_event(Summary, 0, refresh_failed_ ? "refresh-incomplete" : "complete");
}

void HwSaveFlow::device_gone() {
    if(!busy()) return;
    for(Outcome& o : outcomes_) if(o.verdict == "not-tried") o.detail = "device-gone";
    waiting_reply_ = send_pending_ = waiting_send_ = false;
    state_ = Done; queue_event(Summary, 0, "device-gone");
}

std::vector<HwSaveFlow::Event> HwSaveFlow::take_events() {
    std::vector<Event> out; out.swap(events_); return out;
}
