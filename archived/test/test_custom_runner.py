import re

import click

from platformio.test.result import TestCase, TestStatus
from platformio.test.runners.base import TestRunnerBase


class CustomTestRunner(TestRunnerBase):
    PASS_RE = re.compile(r"^\[PASS\]\s+(?P<name>.+)$")
    FAIL_RE = re.compile(r"^\[FAIL\]\s+(?P<name>.+)$")

    def on_testing_line_output(self, line):
        click.echo(line, nl=False)
        stripped = line.strip()
        pass_match = self.PASS_RE.match(stripped)
        if pass_match:
            self.test_suite.add_case(
                TestCase(pass_match.group("name"), TestStatus.PASSED, stdout=stripped)
            )
            return

        fail_match = self.FAIL_RE.match(stripped)
        if fail_match:
            self.test_suite.add_case(
                TestCase(fail_match.group("name"), TestStatus.FAILED, stdout=stripped)
            )
