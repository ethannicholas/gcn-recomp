// Input logging and replay.
//
// A run leaves behind a record of every controller state the game polled, keyed by the
// presented-frame count, so that the same route can be played again without a human at
// the controls -- "get me to the boss" rather than scripting a handful of presses. It is
// not a deterministic replay: the guest runs on wall-clock timing, so a replay can drift
// by a frame here and there. It is good enough to get back to where something happened.
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

// Called by the SI layer each time the guest reads a pad. Records a line when the state
// of that channel differs from the last line written for it.
void input_log_record(int chan, const PadState& s);

// Load `dir` for replay. The card snapshot, if any, is copied to `dir`/memcard_replay.raw
// and `memcard_out` receives that path; the frontend points the EXI card at it before
// boot. Returns false if there is no inputs.txt.
bool input_replay_load(const std::string& dir, std::string& memcard_out);

// While a replay is loaded and the current frame is within it, overwrites `s` with the
// logged state for `chan` and returns true. Past the end of the log the live controller
// takes over again, and this returns false.
bool input_replay_apply(int chan, PadState& s);

// Whether a replay is in progress (loaded and not yet past its last frame).
bool input_replay_active();
