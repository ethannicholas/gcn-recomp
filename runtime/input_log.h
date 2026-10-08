// Input logging and replay.
//
// A run leaves behind a record of every controller state the game polled, keyed by the
// poll's sequence number, so that the same route can be played again without a human at
// the controls -- "get me to the boss" rather than scripting a handful of presses. With
// the virtual clock (runtime.h) the guest's timeline is a function of its own execution,
// so the Nth poll of a replay is the same instant in the game as the Nth poll of the
// recording, and the replay is exact. The log also records the run's start time, which
// the replay feeds to the RTC, so even the calendar looks the same to the game.
//
// Version 1 logs (keyed by presented frame, recorded under the host clock) still load;
// they replay approximately, as they always did.
//
// A log is a directory:
//   inputs.txt       one line per change of pad state (see the file's own header)
//   memcard_a.raw    the memory card as it was when the run started, if there was one
// A replay restores that card to a scratch copy so that the run sees the same saves and
// the real card is untouched.
#pragma once
#include "hw/pad.h"
#include <cstdint>
#include <string>

// Start recording into `dir` (created). `memcard_path` is copied in as the snapshot when
// it exists. Returns false, and records nothing, if the directory cannot be made.
bool input_log_start(const std::string& dir, const std::string& memcard_path);


// Load `dir` for replay. The card snapshot, if any, is copied to `dir`/memcard_replay.raw
// and `memcard_out` receives that path; the frontend points the EXI card at it before
// boot. Returns false if there is no inputs.txt.
bool input_replay_load(const std::string& dir, std::string& memcard_out);

// Marks where the run ended: the log's last line is otherwise the last *change*, and a
// replay would hand control back to the live pad there, which can be well before the end.
// Called on a normal exit and from the crash and check handlers, so that a replay holds
// the last state up to the instant the run stopped.
void input_log_end();

// Called by the SI layer each time the guest polls a pad, with the live state in `s`.
// While a replay is loaded and not yet past its end, overwrites `s` with the logged state
// for `chan` and returns true; past the end the live controller takes over again and this
// returns false. Either way the state the guest will see is recorded, when logging.
bool input_pad_poll(int chan, PadState& s);

// Whether a replay is in progress (loaded and not yet past its last poll).
bool input_replay_active();
