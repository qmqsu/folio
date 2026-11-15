#!/usr/bin/env python3
"""Pure-mock folio_dev regressions; never opens or configures a serial port."""
from contextlib import ExitStack
from pathlib import Path
import sys
import termios
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import folio_dev  # noqa: E402


class DeviceCloseTests(unittest.TestCase):
    def assert_close_once(self, flush_error=None):
        # Skip __init__: even this artificial descriptor must never reach the OS.
        device = folio_dev.Device.__new__(folio_dev.Device)
        device.fd = 12345
        with ExitStack() as stack:
            forbidden = []
            for target, name in (
                (folio_dev.os, 'open'), (folio_dev.os, 'read'),
                (folio_dev.os, 'write'), (folio_dev.fcntl, 'flock'),
                (folio_dev.tty, 'setraw'), (folio_dev.termios, 'tcgetattr'),
                (folio_dev.termios, 'tcsetattr'),
            ):
                forbidden.append(stack.enter_context(mock.patch.object(
                    target, name, side_effect=AssertionError('Unexpected serial operation'))))
            flush = stack.enter_context(mock.patch.object(
                folio_dev.termios, 'tcflush', side_effect=flush_error))
            close = stack.enter_context(mock.patch.object(folio_dev.os, 'close'))
            device.close()
            # A caller's finally may close again after handling disconnection.
            device.close()
            flush.assert_called_once_with(12345, termios.TCIOFLUSH)
            close.assert_called_once_with(12345)
            self.assertIsNone(device.fd)
            for operation in forbidden:
                operation.assert_not_called()

    def test_normal_close_is_idempotent(self):
        self.assert_close_once()

    def test_flush_oserror_still_closes_once(self):
        self.assert_close_once(OSError(6, 'Device not configured'))

    def test_flush_termios_error_still_closes_once(self):
        self.assert_close_once(termios.error(6, 'Device not configured'))

    def test_preview_check_is_explicit_and_check_only(self):
        device = folio_dev.Device.__new__(folio_dev.Device)
        device.command = mock.Mock(return_value=(
            'OK', {'channel': 'preview', 'check_only': True}))
        self.assertEqual(
            device.preview_check(), {'channel': 'preview', 'check_only': True})
        device.command.assert_called_once_with('PREVIEW_CHECK')

    def test_preview_download_and_install_are_separate_explicit_commands(self):
        device = folio_dev.Device.__new__(folio_dev.Device)
        device.command = mock.Mock(return_value=('OK', {}))
        device.preview_download()
        device.preview_install()
        self.assertEqual(device.command.call_args_list, [
            mock.call('PREVIEW_DOWNLOAD'), mock.call('PREVIEW_INSTALL')])


if __name__ == '__main__':
    unittest.main(verbosity=2)
