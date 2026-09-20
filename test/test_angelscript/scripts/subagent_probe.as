void register_tests() {
  qt_test("Subagents discover roles and execute isolated single, parallel, and chain tasks",
          "test_subagent_probe");
}

void test_subagent_probe() {
  qt_assert(qt_run_subagent_probe(), "Subagent probe should pass");
}
