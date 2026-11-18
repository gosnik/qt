void register_tests() {
  qt_test("AngelScript test API registers and runs tests", "test_registration");
  qt_test("AngelScript assertions compare strings and integers", "test_assertions");
}

void test_registration() {
  qt_log("registration smoke test running");
  qt_assert(true, "true should be accepted");
}

void test_assertions() {
  string label = "qt";
  qt_assert_eq(label, "qt", "string equality should work");
  qt_assert_eq_i(2 + 2, 4, "integer equality should work");
}
