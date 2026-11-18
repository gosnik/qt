void register_tests() {
  qt_test("MCP client supports stdio, HTTP, SSE, retries, redaction, and sampling", "test_mcp_client_probe");
}

void test_mcp_client_probe() {
  qt_assert(qt_run_mcp_client_probe(), "MCP client probe should pass");
}
