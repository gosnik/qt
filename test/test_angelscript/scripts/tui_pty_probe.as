void register_tests() {
  qt_test("TUI PTY probe validates slash overlay and native scrollback", "test_tui_pty_probe");
}

void test_tui_pty_probe() {
  qt_assert(qt_run_tui_pty_probe(), "TUI PTY/tmux probe should pass");
}
