//! Nonblocking run control. Stop replies are emitted only after a confirmed halt.

use std::collections::VecDeque;

pub trait RunControl {
    fn resume(&mut self) -> Result<(), ()>;
    fn halt(&mut self) -> Result<(), ()>;
    fn is_halted(&mut self) -> Result<bool, ()>;
}

#[derive(Default)]
pub struct RunState {
    awaiting_stop: bool,
    // Replies can be queued together (S00 then S05). ACKs must retire them
    // in order so a late Ctrl-C cannot enqueue a second stop reply.
    pending_replies: VecDeque<bool>,
}

impl RunState {
    pub fn resume(&mut self, core: &mut impl RunControl) -> &'static str {
        if core.resume().is_err() {
            return "E01";
        }
        self.awaiting_stop = true;
        "S00"
    }

    pub fn reply_sent(&mut self, reply: &str) {
        self.pending_replies
            .push_back(reply == "S05" || reply == "S02");
    }

    pub fn acknowledge_reply(&mut self) {
        self.pending_replies.pop_front();
    }

    pub fn interrupt(&mut self, core: &mut impl RunControl) -> Option<&'static str> {
        if self.pending_replies.contains(&true) {
            return None;
        }
        // Cancel the pending continue reply so an interrupt produces one stop.
        self.awaiting_stop = false;
        Some(match core.halt() {
            Ok(()) => "S02",
            Err(()) => "E01",
        })
    }

    pub fn poll(&mut self, core: &mut impl RunControl) -> Option<&'static str> {
        if !self.awaiting_stop {
            return None;
        }
        match core.is_halted() {
            Ok(false) => None,
            result => {
                self.awaiting_stop = false;
                Some(if result.is_ok() { "S05" } else { "E01" })
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[derive(Default)]
    struct FakeCore {
        halted: bool,
        fail: bool,
        resumes: usize,
        halts: usize,
    }

    impl RunControl for FakeCore {
        fn resume(&mut self) -> Result<(), ()> {
            self.resumes += 1;
            if self.fail {
                return Err(());
            }
            self.halted = false;
            Ok(())
        }
        fn halt(&mut self) -> Result<(), ()> {
            self.halts += 1;
            if self.fail {
                return Err(());
            }
            self.halted = true;
            Ok(())
        }
        fn is_halted(&mut self) -> Result<bool, ()> {
            if self.fail {
                Err(())
            } else {
                Ok(self.halted)
            }
        }
    }

    #[test]
    fn continue_waits_for_a_real_halt_without_a_synthetic_timeout_stop() {
        let mut state = RunState::default();
        let mut core = FakeCore::default();
        assert_eq!(state.resume(&mut core), "S00");
        for _ in 0..20_000 {
            assert_eq!(state.poll(&mut core), None);
        }
        core.halted = true;
        assert_eq!(state.poll(&mut core), Some("S05"));
        assert_eq!(state.poll(&mut core), None);
    }

    #[test]
    fn interrupt_halts_and_does_not_duplicate_the_pending_stop() {
        let mut state = RunState::default();
        let mut core = FakeCore::default();
        assert_eq!(state.resume(&mut core), "S00");
        assert_eq!(state.interrupt(&mut core), Some("S02"));
        assert!(core.halted);
        assert_eq!(core.halts, 1);
        assert_eq!(state.poll(&mut core), None);
        assert_eq!(state.resume(&mut core), "S00");
        assert_eq!(core.resumes, 2);
    }

    #[test]
    fn failures_are_errors_rather_than_stop_signals() {
        let mut state = RunState::default();
        let mut core = FakeCore {
            fail: true,
            ..FakeCore::default()
        };
        assert_eq!(state.resume(&mut core), "E01");
        assert_eq!(state.poll(&mut core), None);
        assert_eq!(state.interrupt(&mut core), Some("E01"));
        core.fail = false;
        assert_eq!(state.resume(&mut core), "S00");
        core.fail = true;
        assert_eq!(state.poll(&mut core), Some("E01"));
        assert_eq!(state.poll(&mut core), None);
    }

    #[test]
    fn breakpoint_stop_racing_interrupt_emits_only_one_stop() {
        let mut state = RunState::default();
        let mut core = FakeCore::default();
        assert_eq!(state.resume(&mut core), "S00");
        state.reply_sent("S00");
        core.halted = true;
        assert_eq!(state.poll(&mut core), Some("S05"));
        state.reply_sent("S05");
        state.acknowledge_reply(); // ACK for S00, not for S05.
        assert_eq!(state.interrupt(&mut core), None);
        assert_eq!(core.halts, 0);
        state.acknowledge_reply();
        assert_eq!(state.interrupt(&mut core), Some("S02"));
    }

    #[test]
    fn resume_restarts_a_core_that_halted_before_the_next_poll() {
        let mut state = RunState::default();
        let mut core = FakeCore::default();
        assert_eq!(state.resume(&mut core), "S00");
        core.halted = true;
        assert_eq!(state.resume(&mut core), "S00");
        assert!(!core.halted);
        assert_eq!(core.resumes, 2);
        assert_eq!(state.poll(&mut core), None);
    }
}
