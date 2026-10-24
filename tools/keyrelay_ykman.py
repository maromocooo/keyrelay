"""Run the ykman CLI against a KeyRelay receiver.

ykman's CLI works out a YubiKey's USB product ID from the smart-card reader's name ("Yubico YubiKey
OTP+FIDO+CCID"). KeyRelay's reader has its own name, so `ykman --reader KeyRelay ...` stops with
"Failed to connect to YubiKey". This script tells ykman, inside this process only, which product ID
to assume for a reader whose name contains "KeyRelay", then runs the normal ykman CLI. Nothing
changes on the USB bus: the receiver keeps its own USB IDs and names.

It replaces a private ykman function (ykman.pcsc._pid_from_name), so a future ykman release may
break it. Tested with ykman 5.9.2.

Usage (ykman installed with Homebrew):
    "$(brew --prefix ykman)/libexec/bin/python" tools/keyrelay_ykman.py --reader KeyRelay info
"""
import sys

import ykman.pcsc as pcsc
from yubikit.core import PID

# The relayed key's interfaces (a YubiKey 5 with OTP, FIDO and CCID enabled). Only the smart-card
# interface is used through --reader.
KEYRELAY_PID = PID.YK4_OTP_FIDO_CCID

_pid_from_name = pcsc._pid_from_name


def _pid_for_reader(name):
    if "keyrelay" in name.lower():
        return KEYRELAY_PID
    return _pid_from_name(name)


pcsc._pid_from_name = _pid_for_reader

from ykman._cli.__main__ import main  # noqa: E402  (import after the patch)

sys.exit(main())
